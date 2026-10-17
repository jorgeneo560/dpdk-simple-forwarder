#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <netinet/in.h>

#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_prefetch.h>

#include "common.h"
#include "flow_table.h"
#include "parse.h"

#define PREFETCH_AHEAD 4

/* What the hash table compares. Unused IP bytes are zero (IPv4 uses 4 of 16).
 */
struct flow_key {
  uint8_t src_ip[16];
  uint8_t dst_ip[16];
  uint16_t src_port;
  uint16_t dst_port;
  uint8_t family;
  uint8_t proto;
  uint8_t pad[2]; // explicit, so the key has no uninitialised bytes
};

struct flow_entry {
  uint64_t
      last_seen; // time of the latest packet of the flow, 0 = slot not in use
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint64_t rx_packets;
  uint64_t tx_packets;
};

struct flow_table {
  bool pass_arp;
  struct rte_hash *hash;
  struct flow_entry *entries; // indexed by the slot the hash gives to a flow
  bool full; // an insert failed: new flows are refused until the cleaner frees
             // room
  unsigned int id;
  uint64_t timeout_cycles;
  uint64_t cleanup_cycles;
  uint64_t next_cleanup;
  uint32_t export_next; // where the running export stopped
};

static void make_flow_key(struct flow_key *key, const struct pkt_info *info) {
  memset(key, 0, sizeof(*key));
  memcpy(key->src_ip, info->src_ip, sizeof(key->src_ip));
  memcpy(key->dst_ip, info->dst_ip, sizeof(key->dst_ip));
  key->src_port = info->src_port;
  key->dst_port = info->dst_port;
  key->family = info->family;
  key->proto = info->proto;
}

struct flow_table *flow_table_create(unsigned int id, int socket,
                                     const struct app_config *cfg) {
  char name[RTE_HASH_NAMESIZE];
  struct rte_hash_parameters params = {
      .name = name,
      .entries = cfg->table_size,
      .key_len = sizeof(struct flow_key),
      .hash_func = rte_jhash,
      .socket_id = socket,
  };
  struct flow_table *table =
      rte_zmalloc_socket("flow_table", sizeof(*table), 0, socket);

  if (table == NULL) {
    RTE_LOG(ERR, FLOWFWD, "flow table %u: out of memory\n", id);
    return NULL;
  }
  table->id = id;
  table->pass_arp = cfg->pass_arp != 0;
  table->timeout_cycles = rte_get_tsc_hz() * cfg->flow_timeout;
  table->cleanup_cycles = rte_get_tsc_hz() * cfg->cleanup_interval;
  table->next_cleanup = rte_rdtsc() + table->cleanup_cycles;
  snprintf(name, sizeof(name), "flows_%u", id);
  table->hash = rte_hash_create(&params);
  if (table->hash == NULL) {
    RTE_LOG(ERR, FLOWFWD, "flow table %u: cannot create %u entries: %s\n", id,
            cfg->table_size, rte_strerror(rte_errno));
    rte_free(table);
    return NULL;
  }
  table->entries = rte_zmalloc_socket("flow_entries",
                                      cfg->table_size * sizeof(*table->entries),
                                      RTE_CACHE_LINE_SIZE, socket);
  if (table->entries == NULL) {
    RTE_LOG(ERR, FLOWFWD, "flow table %u: out of memory\n", id);
    flow_table_free(table);
    return NULL;
  }
  return table;
}

void flow_table_free(struct flow_table *table) {
  if (table == NULL)
    return;
  rte_hash_free(table->hash);
  rte_free(table->entries);
  rte_free(table);
}

static void track_flow(struct flow_table *table, const struct pkt_info *info,
                       uint16_t len, bool sent, uint64_t now) {
  struct flow_key key;
  struct flow_entry *entry;
  int slot;

  make_flow_key(&key, info);
  slot = rte_hash_add_key(table->hash, &key);
  if (slot < 0) {
    table->full = true;
    return;
  }
  entry = &table->entries[slot];
  // last_seen is 0 only for a slot that is not in use.
  if (entry->last_seen == 0) {
    memset(entry, 0, sizeof(*entry));
  }
  entry->last_seen = now;
  entry->rx_packets++;
  entry->rx_bytes += len;
  if (sent) {
    entry->tx_packets++;
    entry->tx_bytes += len;
  }
}

static bool flow_exists(const struct flow_table *table,
                        const struct pkt_info *info) {
  struct flow_key key;

  make_flow_key(&key, info);
  return rte_hash_lookup(table->hash, &key) >= 0;
}

// Removes the flows that were last seen more than timeout cycles before now.
static unsigned int expire_flows(struct flow_table *table, uint64_t now,
                                 uint64_t timeout) {
  const void *key;
  void *data;
  uint32_t next = 0;
  unsigned int removed = 0;
  int slot;

  while ((slot = rte_hash_iterate(table->hash, &key, &data, &next)) >= 0) {
    if (now - table->entries[slot].last_seen <= timeout) {
      continue;
    }
    if (rte_hash_del_key(table->hash, key) >= 0) {
      table->entries[slot].last_seen = 0;
      removed++;
    }
  }
  return removed;
}

// Only TCP has a clear end, so only TCP flows are tracked.
// A TCP packet without ports is a non-first fragment: it belongs to a flow seen
// earlier.
static bool is_trackable(const struct pkt_info *info) {
  return info->proto == IPPROTO_TCP && info->src_port != 0 &&
         info->dst_port != 0;
}

uint16_t flow_table_remove_non_ip_pkts(const struct flow_table *table,
                                       struct rte_mbuf **bufs, uint16_t n,
                                       struct flow_batch *batch) {
  uint16_t kept = 0;

  batch->table_full_drops = 0;
  for (uint16_t i = 0; i < PREFETCH_AHEAD && i < n; i++) {
    rte_prefetch0(rte_pktmbuf_mtod(bufs[i], void *));
  }

  for (uint16_t i = 0; i < n; i++) {
    enum parse_result res;
    bool keep;

    if (i + PREFETCH_AHEAD < n)
      rte_prefetch0(rte_pktmbuf_mtod(bufs[i + PREFETCH_AHEAD], void *));

    // Parsed straight into the next free slot, which a dropped packet leaves
    // unused.
    res = parse_packet(rte_pktmbuf_mtod(bufs[i], const uint8_t *),
                       rte_pktmbuf_data_len(bufs[i]), &batch->info[kept]);
    keep = res == PARSE_IP || (res == PARSE_ARP && table->pass_arp);
    // Only while the table is full: a packet of a flow that cannot be added is
    // dropped.
    if (keep && table->full && is_trackable(&batch->info[kept]) &&
        !flow_exists(table, &batch->info[kept])) {
      keep = false;
      batch->table_full_drops++;
    }
    if (keep) {
      batch->len[kept] = (uint16_t)rte_pktmbuf_pkt_len(bufs[i]);
      bufs[kept++] = bufs[i];
    } else {
      rte_pktmbuf_free(bufs[i]);
    }
  }
  batch->count = kept;
  return kept;
}

void flow_table_update(struct flow_table *table, const struct flow_batch *batch,
                       uint16_t nb_sent) {
  uint64_t now = rte_rdtsc();

  for (uint16_t i = 0; i < batch->count; i++) {
    if (is_trackable(&batch->info[i])) {
      track_flow(table, &batch->info[i], batch->len[i], i < nb_sent, now);
    }
  }
}

void flow_table_cleanup(struct flow_table *table, uint64_t now) {
  unsigned int removed;

  if (now < table->next_cleanup)
    return;
  removed = expire_flows(table, now, table->timeout_cycles);
  if (removed > 0) {
    table->full = false;
  }
  table->next_cleanup = now + table->cleanup_cycles;
}

unsigned int flow_table_count(const struct flow_table *table) {
  return (unsigned int)rte_hash_count(table->hash);
}

void flow_table_export_begin(struct flow_table *table) {
  table->export_next = 0;
}

size_t flow_table_export_next(struct flow_table *table,
                              struct flow_record *recs, size_t max) {
  const void *key;
  void *data;
  size_t n = 0;
  int slot;

  while (n < max && (slot = rte_hash_iterate(table->hash, &key, &data,
                                             &table->export_next)) >= 0) {
    const struct flow_key *k = key;
    const struct flow_entry *e = &table->entries[slot];
    struct flow_record *r = &recs[n++];

    r->family = k->family;
    r->proto = k->proto;
    r->src_port = k->src_port;
    r->dst_port = k->dst_port;
    memcpy(r->src_ip, k->src_ip, sizeof(r->src_ip));
    memcpy(r->dst_ip, k->dst_ip, sizeof(r->dst_ip));
    r->rx_bytes = e->rx_bytes;
    r->tx_bytes = e->tx_bytes;
    r->rx_packets = e->rx_packets;
    r->tx_packets = e->tx_packets;
  }
  return n;
}

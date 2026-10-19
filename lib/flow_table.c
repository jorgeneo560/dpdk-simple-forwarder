#include <stdatomic.h>
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

/* What the hash table compares. The unused bytes of an IPv4 address are zero.
 */
struct flow_key {
  union ip_addr src_ip;
  union ip_addr dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
  uint8_t family;
  uint8_t proto;
  uint8_t pad[2]; // explicit, so the key has no uninitialised bytes
};

// Written only by the RX thread of the queue.
struct flow_entry {
  uint64_t
      last_seen; // time of the latest packet of the flow, 0 = slot not in use
  uint64_t rx_bytes;
  uint64_t rx_packets;
  uint64_t tx_bytes_base; // tx counters of the slot when the flow was created
  uint64_t tx_packets_base;
};

// Written only by the TX thread of the queue, in its own array so that the two
// threads never write to the same cache line. Never reset: the RX thread notes
// the values when a flow is created and the export subtracts them.
struct flow_tx_entry {
  atomic_uint_fast64_t bytes;
  atomic_uint_fast64_t packets;
  atomic_uint_fast32_t gen; // changes every time the slot gets a new flow
};

struct flow_table {
  bool pass_arp;
  struct rte_hash *hash;
  struct flow_entry *entries; // indexed by the slot the hash gives to a flow
  struct flow_tx_entry *tx_entries; // same index as entries
  unsigned int id;
  uint64_t timeout_cycles;
  uint64_t cleanup_cycles;
  uint64_t next_cleanup;
  uint32_t export_next; // where the running export stopped
};

static void make_flow_key(struct flow_key *key, const struct pkt_info *info) {
  memset(key, 0, sizeof(*key));
  key->src_ip = info->src_ip;
  key->dst_ip = info->dst_ip;
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
  table->tx_entries = rte_zmalloc_socket(
      "flow_tx_entries", cfg->table_size * sizeof(*table->tx_entries),
      RTE_CACHE_LINE_SIZE, socket);
  if (table->tx_entries == NULL) {
    RTE_LOG(ERR, FLOWFWD, "flow table %u: out of memory\n", id);
    flow_table_free(table);
    return NULL;
  }
  return table;
}

void flow_table_free(struct flow_table *table) {
  if (table == NULL) {
    return;
  }
  rte_hash_free(table->hash);
  rte_free(table->entries);
  rte_free(table->tx_entries);
  rte_free(table);
}

// Adds a received packet to its flow and returns the flow slot, or FLOW_DROP if
// the table has no room for the flow. *gen is the generation of the slot.
static int32_t track_flow(struct flow_table *table, const struct pkt_info *info,
                          uint16_t len, uint64_t now, uint32_t *gen) {
  struct flow_key key;
  struct flow_entry *entry;
  struct flow_tx_entry *tx;
  int slot;

  *gen = 0;
  make_flow_key(&key, info);
  slot = rte_hash_add_key(table->hash, &key);
  if (slot < 0) {
    return FLOW_DROP;
  }
  entry = &table->entries[slot];
  tx = &table->tx_entries[slot];
  // last_seen is 0 only for a slot that is not in use. A new flow gets a new
  // generation, so a packet of the old flow that is still waiting for the TX
  // thread is not counted in it.
  if (entry->last_seen == 0) {
    atomic_store_explicit(
        &tx->gen, atomic_load_explicit(&tx->gen, memory_order_relaxed) + 1,
        memory_order_release);
    memset(entry, 0, sizeof(*entry));
    entry->tx_bytes_base =
        atomic_load_explicit(&tx->bytes, memory_order_relaxed);
    entry->tx_packets_base =
        atomic_load_explicit(&tx->packets, memory_order_relaxed);
  }
  entry->last_seen = now;
  entry->rx_packets++;
  entry->rx_bytes += len;
  *gen = (uint32_t)atomic_load_explicit(&tx->gen, memory_order_relaxed);
  return slot;
}

void flow_table_count_sent(struct flow_table *table, int32_t slot, uint32_t gen,
                           uint16_t len) {
  struct flow_tx_entry *tx = &table->tx_entries[slot];

  // The slot went to another flow while the packet was waiting: not its packet.
  if ((uint32_t)atomic_load_explicit(&tx->gen, memory_order_acquire) != gen) {
    return;
  }
  // A single writer, so a plain load and store is enough.
  atomic_store_explicit(
      &tx->packets,
      atomic_load_explicit(&tx->packets, memory_order_relaxed) + 1,
      memory_order_relaxed);
  atomic_store_explicit(
      &tx->bytes, atomic_load_explicit(&tx->bytes, memory_order_relaxed) + len,
      memory_order_relaxed);
}

// Removes the flows that were last seen more than timeout cycles before now.
static void expire_flows(struct flow_table *table, uint64_t now,
                         uint64_t timeout) {
  const void *key;
  void *data;
  uint32_t next = 0;
  int slot;

  while ((slot = rte_hash_iterate(table->hash, &key, &data, &next)) >= 0) {
    if (now - table->entries[slot].last_seen <= timeout) {
      continue;
    }
    if (rte_hash_del_key(table->hash, key) >= 0) {
      table->entries[slot].last_seen = 0;
    }
  }
}

// TCP and UDP flows are tracked: they have ports, so the 5-tuple is complete.
// The timeout removes the ones that stop, so no connection end is needed. A
// packet without ports is a non-first fragment, or another protocol (ICMP).
static bool is_trackable(const struct pkt_info *info) {
  return (info->proto == IPPROTO_TCP || info->proto == IPPROTO_UDP) &&
         info->src_port != 0 && info->dst_port != 0;
}

uint16_t flow_table_filter_burst(const struct flow_table *table,
                                 struct rte_mbuf **bufs, uint16_t n,
                                 struct burst_meta *meta) {
  uint16_t kept = 0;

  for (uint16_t i = 0; i < PREFETCH_AHEAD && i < n; i++) {
    rte_prefetch0(rte_pktmbuf_mtod(bufs[i], void *));
  }

  for (uint16_t i = 0; i < n; i++) {
    enum parse_result res;
    bool keep;

    if (i + PREFETCH_AHEAD < n) {
      rte_prefetch0(rte_pktmbuf_mtod(bufs[i + PREFETCH_AHEAD], void *));
    }

    // Parsed straight into the next free slot, which a dropped packet leaves
    // unused.
    res = parse_packet(rte_pktmbuf_mtod(bufs[i], const uint8_t *),
                       rte_pktmbuf_data_len(bufs[i]), &meta->info[kept]);
    keep = res == PARSE_IP || (res == PARSE_ARP && table->pass_arp);
    if (keep) {
      meta->len[kept] = (uint16_t)rte_pktmbuf_pkt_len(bufs[i]);
      bufs[kept++] = bufs[i];
    } else {
      rte_pktmbuf_free(bufs[i]);
    }
  }
  meta->count = kept;
  return kept;
}

void flow_table_track(struct flow_table *table, struct burst_meta *meta) {
  uint64_t now = rte_rdtsc();

  for (uint16_t i = 0; i < meta->count; i++) {
    if (is_trackable(&meta->info[i])) {
      meta->slot[i] =
          track_flow(table, &meta->info[i], meta->len[i], now, &meta->gen[i]);
    } else {
      meta->slot[i] = FLOW_NO_SLOT;
      meta->gen[i] = 0;
    }
  }
}

void flow_table_cleanup(struct flow_table *table, uint64_t now) {
  if (now < table->next_cleanup) {
    return;
  }
  expire_flows(table, now, table->timeout_cycles);
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
    struct flow_tx_entry *tx = &table->tx_entries[slot];
    struct flow_record *r = &recs[n++];

    r->family = k->family;
    r->proto = k->proto;
    r->src_port = k->src_port;
    r->dst_port = k->dst_port;
    r->src_ip = k->src_ip;
    r->dst_ip = k->dst_ip;
    r->rx_bytes = e->rx_bytes;
    r->tx_bytes = atomic_load_explicit(&tx->bytes, memory_order_relaxed) -
                  e->tx_bytes_base;
    r->rx_packets = e->rx_packets;
    r->tx_packets = atomic_load_explicit(&tx->packets, memory_order_relaxed) -
                    e->tx_packets_base;
  }
  return n;
}

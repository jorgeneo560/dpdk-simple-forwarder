#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
#include <rte_ring.h>
#include <rte_ring_elem.h>

#include "common.h"
#include "flow_export.h"
#include "flow_table.h"
#include "forward.h"

// What the RX thread hands to the TX thread for each packet.
struct tx_item {
  struct rte_mbuf *mbuf;
  int32_t slot; // flow slot of the packet, FLOW_NO_SLOT if it is not tracked
  uint32_t gen; // generation of that slot when the packet was seen
  uint16_t len; // bytes of the packet on the wire
};

// Everything the RX and the TX thread of a queue need.
struct queue_args {
  uint16_t queue;
  uint16_t in_port;
  uint16_t out_port;
  struct flow_table *table;
  const struct rte_ether_addr
      *forward_mac; // NULL = leave the destination MAC alone
  const struct rte_ether_addr *reverse_mac;
  struct rte_ring *forward_ring; // packets going from input to output
  struct rte_ring *reverse_ring; // packets going from output to input
};

static struct queue_args queue_args[MAX_QUEUES];
static struct rte_ring *forward_rings[MAX_QUEUES];
static struct rte_ring *reverse_rings[MAX_QUEUES];
static atomic_bool rx_quit; // the RX threads stop first
static atomic_bool tx_quit; // the TX threads stop when their rings are empty
static unsigned int rx_lcore_ids[MAX_QUEUES], tx_lcore_ids[MAX_QUEUES];

// One per RX thread, written only by it and read by the main thread. Each one
// fills its own cache lines, so threads never share a line.
struct rx_stats {
  atomic_uint_fast64_t rx_packets;
  atomic_uint_fast64_t
      table_drops; // packets dropped because the flow table was full
  atomic_uint_fast64_t ring_drops; // packets dropped because the ring was full
  atomic_uint_fast64_t
      busy_cycles; // time spent in iterations that handled packets
  atomic_uint_fast64_t total_cycles;
  atomic_uint_fast64_t flows; // flows currently in the queue's table
} __rte_cache_aligned;

// The same for a TX thread.
struct tx_stats {
  atomic_uint_fast64_t tx_packets;
  atomic_uint_fast64_t tx_drops; // packets the NIC did not accept
  atomic_uint_fast64_t busy_cycles;
  atomic_uint_fast64_t total_cycles;
} __rte_cache_aligned;

static struct rx_stats rx_stats[MAX_QUEUES];
static struct tx_stats tx_stats[MAX_QUEUES];
static struct flow_table *flow_tables[MAX_QUEUES];
static unsigned int nb_queues;
static uint64_t export_cycles;

// Sets the destination MAC of the IP packets of a burst. ARP frames (no IP
// family) are left alone.
static void rewrite_dst_mac(struct rte_mbuf **bufs,
                            const struct flow_batch *batch,
                            const struct rte_ether_addr *mac) {
  for (uint16_t i = 0; i < batch->count; i++) {
    if (batch->info[i].family != 0) {
      struct rte_ether_hdr *eth =
          rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);

      rte_ether_addr_copy(mac, &eth->dst_addr);
    }
  }
}

// Counters are updated once per burst, not per packet. A counter has a single
// writer, so a plain load and store is enough, without a locked instruction.
static inline void add_stat(atomic_uint_fast64_t *counter, uint64_t n) {
  atomic_store_explicit(
      counter, atomic_load_explicit(counter, memory_order_relaxed) + n,
      memory_order_relaxed);
}

// RX thread: reads a burst from one interface, filters and counts it, and
// queues the packets for the TX thread. Returns how many packets were read.
static uint16_t receive_burst(const struct queue_args *args, uint16_t from,
                              struct rte_ring *ring,
                              const struct rte_ether_addr *dst_mac) {
  struct rx_stats *stats = &rx_stats[args->queue];
  struct rte_mbuf *bufs[BURST_SIZE];
  uint16_t nb_rx = rte_eth_rx_burst(from, args->queue, bufs, BURST_SIZE);

  if (nb_rx == 0)
    return 0;
  add_stat(&stats->rx_packets, nb_rx);

  struct flow_batch batch;
  struct tx_item items[BURST_SIZE];
  unsigned int nb_items = 0, nb_queued, full_drops = 0;
  uint16_t nb_kept =
      flow_table_remove_non_ip_pkts(args->table, bufs, nb_rx, &batch);

  if (batch.table_full_drops > 0) {
    add_stat(&stats->table_drops, batch.table_full_drops);
  }
  if (nb_kept == 0)
    return nb_rx;

  if (dst_mac != NULL) {
    rewrite_dst_mac(bufs, &batch, dst_mac);
  }
  flow_table_track(args->table, &batch);

  for (uint16_t i = 0; i < nb_kept; i++) {
    if (batch.slot[i] == FLOW_DROP) {
      rte_pktmbuf_free(bufs[i]);
      full_drops++;
    } else {
      items[nb_items++] = (struct tx_item){bufs[i], batch.slot[i], batch.gen[i],
                                           batch.len[i]};
    }
  }
  if (full_drops > 0) {
    add_stat(&stats->table_drops, full_drops);
  }
  nb_queued =
      rte_ring_enqueue_burst_elem(ring, items, sizeof(items[0]), nb_items, NULL);
  for (unsigned int i = nb_queued; i < nb_items; i++) {
    rte_pktmbuf_free(items[i].mbuf);
  }
  if (nb_queued < nb_items) {
    add_stat(&stats->ring_drops, nb_items - nb_queued);
  }
  return nb_rx;
}

// TX thread: takes a burst from a ring, sends it and counts the sent packets in
// their flows. Returns how many packets were taken.
static uint16_t send_burst(const struct queue_args *args, struct rte_ring *ring,
                           uint16_t to) {
  struct tx_stats *stats = &tx_stats[args->queue];
  struct tx_item items[BURST_SIZE];
  struct rte_mbuf *bufs[BURST_SIZE];
  unsigned int n =
      rte_ring_dequeue_burst_elem(ring, items, sizeof(items[0]), BURST_SIZE, NULL);
  uint16_t nb_tx;

  if (n == 0)
    return 0;
  for (unsigned int i = 0; i < n; i++) {
    bufs[i] = items[i].mbuf;
  }
  nb_tx = rte_eth_tx_burst(to, args->queue, bufs, (uint16_t)n);
  add_stat(&stats->tx_packets, nb_tx);

  // Only the saved slot and length are used: a sent packet is no longer ours.
  for (uint16_t i = 0; i < nb_tx; i++) {
    if (items[i].slot != FLOW_NO_SLOT) {
      flow_table_count_sent(args->table, items[i].slot, items[i].gen,
                            items[i].len);
    }
  }
  if (nb_tx < n) {
    rte_pktmbuf_free_bulk(&bufs[nb_tx], n - nb_tx);
    add_stat(&stats->tx_drops, n - nb_tx);
  }
  return (uint16_t)n;
}

// One export of the queue's flows. The table is copied to the writer thread in
// chunks, one per loop pass, so forwarding is not held up. All rows of an
// export share one timestamp.
struct export_pass {
  bool active;
  struct timespec ts;
};

static void export_pass_start(struct export_pass *pass,
                              struct flow_table *table) {
  clock_gettime(CLOCK_REALTIME, &pass->ts);
  flow_table_export_begin(table);
  pass->active = true;
}

static void export_pass_step(struct export_pass *pass, uint16_t queue,
                             struct flow_table *table) {
  struct flow_record *recs = flow_export_begin(queue);
  size_t n;

  if (recs == NULL) {
    return; // the writer is still busy with the previous chunk
  }
  n = flow_table_export_next(table, recs, FLOW_EXPORT_MAX);
  if (n == 0) {
    pass->active = false;
    return;
  }
  flow_export_commit(queue, &pass->ts, n);
}

static int rx_main(void *arg) {
  const struct queue_args *args = arg;
  struct rx_stats *stats = &rx_stats[args->queue];
  uint64_t busy = 0, total = 0, last, next_export;
  struct export_pass export_pass = {.active = false};

  RTE_LOG(DEBUG, FLOWFWD, "RX thread started: queue %u, ports %u <-> %u\n",
          args->queue, args->in_port, args->out_port);
  last = rte_rdtsc();
  next_export = last + export_cycles;
  while (!atomic_load_explicit(&rx_quit, memory_order_relaxed)) {
    unsigned int handled = receive_burst(args, args->in_port,
                                         args->forward_ring, args->forward_mac);
    uint64_t now;

    handled += receive_burst(args, args->out_port, args->reverse_ring,
                             args->reverse_mac);

    now = rte_rdtsc();
    total += now - last;
    if (handled > 0)
      busy += now - last;
    last = now;
    if (!export_pass.active && now >= next_export) {
      export_pass_start(&export_pass, args->table);
      next_export = now + export_cycles;
    }
    if (export_pass.active) {
      export_pass_step(&export_pass, args->queue, args->table);
    }
    flow_table_cleanup(args->table, now);
    atomic_store_explicit(&stats->busy_cycles, busy, memory_order_relaxed);
    atomic_store_explicit(&stats->total_cycles, total, memory_order_relaxed);
    atomic_store_explicit(&stats->flows, flow_table_count(args->table),
                          memory_order_relaxed);
  }
  RTE_LOG(DEBUG, FLOWFWD, "RX thread stopped: queue %u\n", args->queue);
  return 0;
}

// After the stop request the TX thread keeps going until its rings are empty (the
// RX threads have already stopped, so nothing is added), so every accepted
// packet is sent or counted as dropped.
static bool rings_empty(const struct queue_args *args) {
  return rte_ring_empty(args->forward_ring) &&
         rte_ring_empty(args->reverse_ring);
}

static int tx_main(void *arg) {
  const struct queue_args *args = arg;
  struct tx_stats *stats = &tx_stats[args->queue];
  uint64_t busy = 0, total = 0, last;

  RTE_LOG(DEBUG, FLOWFWD, "TX thread started: queue %u, ports %u <-> %u\n",
          args->queue, args->in_port, args->out_port);
  last = rte_rdtsc();
  while (!atomic_load_explicit(&tx_quit, memory_order_relaxed) ||
         !rings_empty(args)) {
    unsigned int handled = send_burst(args, args->forward_ring, args->out_port);
    uint64_t now;

    handled += send_burst(args, args->reverse_ring, args->in_port);

    now = rte_rdtsc();
    total += now - last;
    if (handled > 0)
      busy += now - last;
    last = now;
    atomic_store_explicit(&stats->busy_cycles, busy, memory_order_relaxed);
    atomic_store_explicit(&stats->total_cycles, total, memory_order_relaxed);
  }
  RTE_LOG(DEBUG, FLOWFWD, "TX thread stopped: queue %u\n", args->queue);
  return 0;
}

static void free_flow_tables(void) {
  for (unsigned int i = 0; i < MAX_QUEUES; i++) {
    flow_table_free(flow_tables[i]);
    flow_tables[i] = NULL;
  }
}

// One table per queue, in the memory of the socket its RX lcore runs on.
static bool create_flow_tables(const struct app_config *cfg, //ok
                               const unsigned int *rx_lcores, unsigned int nb) {
  for (unsigned int i = 0; i < nb; i++) {
    flow_tables[i] =
        flow_table_create(i, (int)rte_lcore_to_socket_id(rx_lcores[i]), cfg);
    if (flow_tables[i] == NULL) {
      free_flow_tables();
      return false;
    }
  }
  return true;
}

// Frees the packets still waiting in a ring, then the ring.
static void free_ring(struct rte_ring *ring) {
  struct tx_item item;

  if (ring == NULL)
    return;
  while (rte_ring_dequeue_elem(ring, &item, sizeof(item)) == 0) {
    rte_pktmbuf_free(item.mbuf);
  }
  rte_ring_free(ring);
}

static void free_rings(void) {
  for (unsigned int i = 0; i < MAX_QUEUES; i++) {
    free_ring(forward_rings[i]);
    free_ring(reverse_rings[i]);
    forward_rings[i] = NULL;
    reverse_rings[i] = NULL;
  }
}

// A ring with one producer (the RX thread) and one consumer (the TX thread),
// which makes it lock-free.
static struct rte_ring *create_ring(const char *prefix, unsigned int queue,
                                    int socket) {
  char name[RTE_RING_NAMESIZE];
  struct rte_ring *ring;

  snprintf(name, sizeof(name), "%s_%u", prefix, queue);
  ring = rte_ring_create_elem(name, sizeof(struct tx_item), RING_SIZE, socket,
                              RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (ring == NULL) {
    RTE_LOG(ERR, FLOWFWD, "cannot create ring %s: %s\n", name,
            rte_strerror(rte_errno));
  }
  return ring;
}

static bool create_rings(const unsigned int *rx_lcores, unsigned int nb) {
  for (unsigned int i = 0; i < nb; i++) {
    int socket = (int)rte_lcore_to_socket_id(rx_lcores[i]);

    forward_rings[i] = create_ring("forward", i, socket);
    reverse_rings[i] = create_ring("reverse", i, socket);
    if (forward_rings[i] == NULL || reverse_rings[i] == NULL) {
      free_rings();
      return false;
    }
  }
  return true;
}

static struct rte_ether_addr forward_mac_storage, reverse_mac_storage;

// Returns the MAC to write, or NULL when the setting is all zero (no rewrite).
static const struct rte_ether_addr *
rewrite_target(const uint8_t mac[RTE_ETHER_ADDR_LEN],
               struct rte_ether_addr *storage) {
  static const uint8_t zero[RTE_ETHER_ADDR_LEN];

  if (memcmp(mac, zero, RTE_ETHER_ADDR_LEN) == 0) {
    return NULL;
  }
  memcpy(storage->addr_bytes, mac, RTE_ETHER_ADDR_LEN);
  return storage;
}

static void format_rewrite(const struct rte_ether_addr *mac, char *buf,
                           size_t len) {
  if (mac == NULL) {
    snprintf(buf, len, "unchanged");
  } else {
    rte_ether_format_addr(buf, len, mac);
  }
}

bool forward_start(const struct app_config *cfg) {
  unsigned int rx_lcores[MAX_QUEUES], tx_lcores[MAX_QUEUES];
  unsigned int lcore, nb = 0, found = 0;
  const unsigned int needed = 2 * cfg->nb_queues;

  if (rte_lcore_count() - 1 < needed) {
    RTE_LOG(ERR, FLOWFWD,
            "need %u worker lcores (an RX and a TX thread per queue) plus the "
            "main lcore, have %u in total\n",
            needed, rte_lcore_count());
    return false;
  }

  // The lcores are taken in pairs: the first of a pair is the RX thread of a
  // queue and the second one its TX thread.
  RTE_LCORE_FOREACH_WORKER(lcore) {
    if (found == needed){
      break;
    }
    if (found % 2 == 0) {
      rx_lcores[nb] = lcore;
    } else {
      tx_lcores[nb++] = lcore;
    }
    found++;
  }

  nb_queues = nb;
  memcpy(rx_lcore_ids, rx_lcores, nb * sizeof(rx_lcores[0]));
  memcpy(tx_lcore_ids, tx_lcores, nb * sizeof(tx_lcores[0]));
  atomic_store(&rx_quit, false);
  atomic_store(&tx_quit, false);
  export_cycles = rte_get_tsc_hz() * cfg->export_interval;

  if (!create_flow_tables(cfg, rx_lcores, nb))
    return false;
  if (!create_rings(rx_lcores, nb)) {
    free_flow_tables();
    return false;
  }
  if (!flow_export_start(cfg, rx_lcores, nb)) {
    RTE_LOG(ERR, FLOWFWD, "cannot start flow export\n");
    free_rings();
    free_flow_tables();
    return false;
  }

  const struct rte_ether_addr *forward_mac =
      rewrite_target(cfg->forward_dst_mac, &forward_mac_storage);
  const struct rte_ether_addr *reverse_mac =
      rewrite_target(cfg->reverse_dst_mac, &reverse_mac_storage);
  char forward_text[RTE_ETHER_ADDR_FMT_SIZE],
      reverse_text[RTE_ETHER_ADDR_FMT_SIZE];

  format_rewrite(forward_mac, forward_text, sizeof(forward_text));
  format_rewrite(reverse_mac, reverse_text, sizeof(reverse_text));
  RTE_LOG(
      INFO, FLOWFWD,
      "destination MAC of IP packets: input -> output %s, output -> input %s\n",
      forward_text, reverse_text);

  for (uint16_t queue = 0; queue < nb; queue++) {
    queue_args[queue] = (struct queue_args){
        queue,           cfg->in_port, cfg->out_port,
        flow_tables[queue], forward_mac,  reverse_mac,
        forward_rings[queue], reverse_rings[queue]};
    // The consumer is started first, so the ring never fills before it runs.
    if (rte_eal_remote_launch(tx_main, &queue_args[queue], tx_lcores[queue]) !=
            0 ||
        rte_eal_remote_launch(rx_main, &queue_args[queue], rx_lcores[queue]) !=
            0) {
      RTE_LOG(ERR, FLOWFWD, "cannot launch the threads of queue %u\n", queue);
      forward_stop();
      return false;
    }
    RTE_LOG(INFO, FLOWFWD,
            "queue %u: RX thread on lcore %u, TX thread on lcore %u\n", queue,
            rx_lcores[queue], tx_lcores[queue]);
  }
  return true;
}

// The last export, made when all threads have stopped, so the flows of the final
// interval are in the files with their final counters.
static void final_export(void) {
  for (unsigned int q = 0; q < nb_queues; q++) {
    struct export_pass pass;

    export_pass_start(&pass, flow_tables[q]);
    while (pass.active) {
      export_pass_step(&pass, (uint16_t)q, flow_tables[q]);
      rte_pause();
    }
  }
}

// Order: the RX threads stop reading, the TX threads send what is left in the
// rings and stop, then the last export is written.
void forward_stop(void) {
  atomic_store(&rx_quit, true);
  for (unsigned int q = 0; q < nb_queues; q++) {
    rte_eal_wait_lcore(rx_lcore_ids[q]);
  }
  atomic_store(&tx_quit, true);
  for (unsigned int q = 0; q < nb_queues; q++) {
    rte_eal_wait_lcore(tx_lcore_ids[q]);
  }
  final_export();
  flow_export_stop();
  free_rings();
  free_flow_tables();
}

uint64_t forward_rx_packets(void) {
  uint64_t sum = 0;

  for (unsigned int i = 0; i < nb_queues; i++) {
    sum += atomic_load_explicit(&rx_stats[i].rx_packets, memory_order_relaxed);
  }
  return sum;
}

uint64_t forward_tx_packets(void) {
  uint64_t sum = 0;

  for (unsigned int i = 0; i < nb_queues; i++) {
    sum += atomic_load_explicit(&tx_stats[i].tx_packets, memory_order_relaxed);
  }
  return sum;
}

static uint64_t port_drops(uint16_t port) {
  struct rte_eth_stats stats;

  if (rte_eth_stats_get(port, &stats) != 0)
    return 0;
  return stats.imissed + stats.rx_nombuf;
}

uint64_t forward_drops(const struct app_config *cfg) {
  uint64_t drops = port_drops(cfg->in_port) + port_drops(cfg->out_port);

  for (unsigned int i = 0; i < nb_queues; i++) {
    drops += atomic_load_explicit(&rx_stats[i].table_drops,
                                  memory_order_relaxed);
    drops +=
        atomic_load_explicit(&rx_stats[i].ring_drops, memory_order_relaxed);
    drops +=
        atomic_load_explicit(&tx_stats[i].tx_drops, memory_order_relaxed);
  }
  return drops;
}

uint64_t forward_flows(void) {
  uint64_t sum = 0;

  for (unsigned int i = 0; i < nb_queues; i++) {
    sum += atomic_load_explicit(&rx_stats[i].flows, memory_order_relaxed);
  }
  return sum;
}

// Share of the time since the previous call that a thread spent handling
// packets, in percent. The previous values are kept in prev_busy/prev_total.
static unsigned int load_percent(atomic_uint_fast64_t *busy_cycles,
                                 atomic_uint_fast64_t *total_cycles,
                                 uint64_t *prev_busy, uint64_t *prev_total) {
  uint64_t busy = atomic_load_explicit(busy_cycles, memory_order_relaxed);
  uint64_t total = atomic_load_explicit(total_cycles, memory_order_relaxed);
  uint64_t d_total = total - *prev_total;
  unsigned int pct =
      d_total > 0 ? (unsigned int)((busy - *prev_busy) * 100 / d_total) : 0;

  *prev_busy = busy;
  *prev_total = total;
  return pct;
}

void forward_worker_usage(char *buf, size_t len) {
  static uint64_t rx_prev_busy[MAX_QUEUES], rx_prev_total[MAX_QUEUES];
  static uint64_t tx_prev_busy[MAX_QUEUES], tx_prev_total[MAX_QUEUES];
  size_t off = 0;

  buf[0] = '\0';
  for (unsigned int i = 0; i < nb_queues && off < len; i++) {
    unsigned int rx = load_percent(&rx_stats[i].busy_cycles,
                                   &rx_stats[i].total_cycles, &rx_prev_busy[i],
                                   &rx_prev_total[i]);
    unsigned int tx = load_percent(&tx_stats[i].busy_cycles,
                                   &tx_stats[i].total_cycles, &tx_prev_busy[i],
                                   &tx_prev_total[i]);

    off += (size_t)snprintf(buf + off, len - off, "%s%u%%/%u%%", i > 0 ? "," : "",
                            rx, tx);
  }
}

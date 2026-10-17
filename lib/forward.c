#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

#include "common.h"
#include "flow_export.h"
#include "flow_table.h"
#include "forward.h"

struct worker_args {
  uint16_t queue;
  uint16_t in_port;
  uint16_t out_port;
  struct flow_table *table;
  const struct rte_ether_addr
      *forward_mac; // NULL = leave the destination MAC alone
  const struct rte_ether_addr *reverse_mac;
};

static struct worker_args worker_args[MAX_QUEUES];
static atomic_bool quit;

// One per worker, written only by that worker and read by the main thread.
// Each one fills its own cache lines, so workers never share a line.
struct worker_stats {
  atomic_uint_fast64_t rx_packets;
  atomic_uint_fast64_t tx_packets;
  atomic_uint_fast64_t
      table_drops; // packets dropped because the flow table was full
  atomic_uint_fast64_t
      busy_cycles; // time spent in iterations that handled packets
  atomic_uint_fast64_t total_cycles;
  atomic_uint_fast64_t flows; // flows currently in the worker's table
} __rte_cache_aligned;

static struct worker_stats worker_stats[MAX_QUEUES];
static struct flow_table *flow_tables[MAX_QUEUES];
static unsigned int nb_workers;
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

static uint16_t forward_burst(uint16_t from, uint16_t to, uint16_t queue,
                              struct flow_table *table,
                              const struct rte_ether_addr *dst_mac) {
  struct worker_stats *stats = &worker_stats[queue];
  struct rte_mbuf *bufs[BURST_SIZE];
  uint16_t nb_rx = rte_eth_rx_burst(from, queue, bufs, BURST_SIZE);

  if (nb_rx == 0)
    return 0;
  add_stat(&stats->rx_packets, nb_rx);

  uint16_t nb_kept, nb_tx;
  struct flow_batch batch;
  nb_kept = flow_table_remove_non_ip_pkts(table, bufs, nb_rx, &batch);
  if (batch.table_full_drops > 0) {
    add_stat(&stats->table_drops, batch.table_full_drops);
  }
  if (nb_kept == 0)
    return nb_rx;

  if (dst_mac != NULL) {
    rewrite_dst_mac(bufs, &batch, dst_mac);
  }
  nb_tx = rte_eth_tx_burst(to, queue, bufs, nb_kept);
  add_stat(&stats->tx_packets, nb_tx);

  if (nb_tx < nb_kept) {
    rte_pktmbuf_free_bulk(&bufs[nb_tx], nb_kept - nb_tx);
  }

  flow_table_update(table, &batch, nb_tx);
  return nb_rx;
}

// One export of the worker's flows. The table is copied to the writer thread in
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

static int worker_main(void *arg) {
  const struct worker_args *args = arg;
  struct worker_stats *stats = &worker_stats[args->queue];
  uint64_t busy = 0, total = 0, last, next_export;
  struct export_pass export_pass = {.active = false};

  RTE_LOG(DEBUG, FLOWFWD, "worker started: queue %u, ports %u <-> %u\n",
          args->queue, args->in_port, args->out_port);
  last = rte_rdtsc();
  next_export = last + export_cycles;
  while (!atomic_load_explicit(&quit, memory_order_relaxed)) {
    unsigned int handled =
        forward_burst(args->in_port, args->out_port, args->queue, args->table,
                      args->forward_mac);
    uint64_t now;

    handled += forward_burst(args->out_port, args->in_port, args->queue,
                             args->table, args->reverse_mac);

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
  // Last export, so the flows of the final interval are not lost.
  export_pass_start(&export_pass, args->table);
  while (export_pass.active) {
    export_pass_step(&export_pass, args->queue, args->table);
    rte_pause();
  }
  RTE_LOG(DEBUG, FLOWFWD, "worker stopped: queue %u\n", args->queue);
  return 0;
}

static void free_flow_tables(void) {
  for (unsigned int i = 0; i < MAX_QUEUES; i++) {
    flow_table_free(flow_tables[i]);
    flow_tables[i] = NULL;
  }
}

// One table per worker, in the memory of the socket its lcore runs on.
static bool create_flow_tables(const struct app_config *cfg,
                               const struct placement *plan) {
  for (unsigned int i = 0; i < plan->nb_workers; i++) {
    flow_tables[i] = flow_table_create(i, plan->sockets[i], cfg);
    if (flow_tables[i] == NULL) {
      free_flow_tables();
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

bool forward_start(const struct app_config *cfg,
                   const struct placement *plan) {
  const unsigned int *lcores = plan->lcores;
  const unsigned int nb = plan->nb_workers;

  atomic_store(&quit, false);
  nb_workers = nb;
  export_cycles = rte_get_tsc_hz() * cfg->export_interval;

  if (!create_flow_tables(cfg, plan))
    return false;
  if (!flow_export_start(cfg, lcores, nb)) {
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
    worker_args[queue] = (struct worker_args){queue,         cfg->in_port,
                                              cfg->out_port, flow_tables[queue],
                                              forward_mac,   reverse_mac};
    if (rte_eal_remote_launch(worker_main, &worker_args[queue],
                              lcores[queue]) != 0) {
      RTE_LOG(ERR, FLOWFWD, "cannot launch worker on lcore %u\n",
              lcores[queue]);
      forward_stop();
      return false;
    }
    RTE_LOG(INFO, FLOWFWD, "worker on lcore %u (socket %d) forwards queue %u\n",
            lcores[queue], plan->sockets[queue], queue);
  }
  return true;
}

void forward_stop(void) {
  unsigned int lcore;

  atomic_store(&quit, true);
  RTE_LCORE_FOREACH_WORKER(lcore) { rte_eal_wait_lcore(lcore); }
  flow_export_stop();
  free_flow_tables();
}

uint64_t forward_rx_packets(void) {
  uint64_t sum = 0;

  for (unsigned int i = 0; i < nb_workers; i++) {
    sum += atomic_load_explicit(&worker_stats[i].rx_packets,
                                memory_order_relaxed);
  }
  return sum;
}

uint64_t forward_tx_packets(void) {
  uint64_t sum = 0;

  for (unsigned int i = 0; i < nb_workers; i++) {
    sum += atomic_load_explicit(&worker_stats[i].tx_packets,
                                memory_order_relaxed);
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

  for (unsigned int i = 0; i < nb_workers; i++) {
    drops += atomic_load_explicit(&worker_stats[i].table_drops,
                                  memory_order_relaxed);
  }
  return drops;
}

uint64_t forward_flows(void) {
  uint64_t sum = 0;

  for (unsigned int i = 0; i < nb_workers; i++) {
    sum += atomic_load_explicit(&worker_stats[i].flows, memory_order_relaxed);
  }
  return sum;
}

void forward_worker_usage(char *buf, size_t len) {
  static uint64_t prev_busy[MAX_QUEUES], prev_total[MAX_QUEUES];
  size_t off = 0;

  buf[0] = '\0';
  for (unsigned int i = 0; i < nb_workers && off < len; i++) {
    uint64_t busy = atomic_load_explicit(&worker_stats[i].busy_cycles,
                                         memory_order_relaxed);
    uint64_t total = atomic_load_explicit(&worker_stats[i].total_cycles,
                                          memory_order_relaxed);
    uint64_t d_total = total - prev_total[i];
    unsigned int pct =
        d_total > 0 ? (unsigned int)((busy - prev_busy[i]) * 100 / d_total) : 0;

    prev_busy[i] = busy;
    prev_total[i] = total;
    off +=
        (size_t)snprintf(buf + off, len - off, "%s%u%%", i > 0 ? "," : "", pct);
  }
}

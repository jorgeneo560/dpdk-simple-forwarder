#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_prefetch.h>

#include "common.h"
#include "flow_export.h"
#include "forward.h"
#include "parse.h"

#define PREFETCH_AHEAD 4

struct worker_args {
	uint16_t queue;
	uint16_t in_port;
	uint16_t out_port;
};

static struct worker_args worker_args[MAX_QUEUES];
static atomic_bool quit;
static atomic_uint_fast64_t rx_total;
static atomic_uint_fast64_t tx_total;

// One per worker, written only by that worker and read by the main thread.
struct worker_stats {
	atomic_uint_fast64_t busy_cycles;	// time spent in iterations that handled packets
	atomic_uint_fast64_t total_cycles;
} __rte_cache_aligned;

static struct worker_stats worker_stats[MAX_QUEUES];
static unsigned int nb_workers;
static uint64_t export_cycles;
static bool pass_arp;

// Keeps IP packets (and ARP if configured) and frees the rest. Returns how many were kept.
static uint16_t
filter_burst(struct rte_mbuf **bufs, uint16_t n)
{
	uint16_t kept = 0;

	for (uint16_t i = 0; i < PREFETCH_AHEAD && i < n; i++)
		rte_prefetch0(rte_pktmbuf_mtod(bufs[i], void *));

	for (uint16_t i = 0; i < n; i++) {
		struct pkt_info info;
		enum parse_result res;

		if (i + PREFETCH_AHEAD < n)
			rte_prefetch0(rte_pktmbuf_mtod(bufs[i + PREFETCH_AHEAD], void *));

		res = parse_packet(rte_pktmbuf_mtod(bufs[i], const uint8_t *),
				   rte_pktmbuf_data_len(bufs[i]), &info);
		if (res == PARSE_IP || (res == PARSE_ARP && pass_arp))
			bufs[kept++] = bufs[i];
		else
			rte_pktmbuf_free(bufs[i]);
	}
	return kept;
}

// Counters are updated once per burst, not per packet, to keep cache-line contention low.
static uint16_t
forward_burst(uint16_t from, uint16_t to, uint16_t queue)
{
	struct rte_mbuf *bufs[BURST_SIZE];
	uint16_t nb_rx = rte_eth_rx_burst(from, queue, bufs, BURST_SIZE);
	uint16_t nb_kept, nb_tx;

	if (nb_rx == 0)
		return 0;
	atomic_fetch_add_explicit(&rx_total, nb_rx, memory_order_relaxed);

	nb_kept = filter_burst(bufs, nb_rx);
	if (nb_kept == 0)
		return nb_rx;

	nb_tx = rte_eth_tx_burst(to, queue, bufs, nb_kept);
	atomic_fetch_add_explicit(&tx_total, nb_tx, memory_order_relaxed);

	if (nb_tx < nb_kept)
		rte_pktmbuf_free_bulk(&bufs[nb_tx], nb_kept - nb_tx);
	return nb_rx;
}

// DUMMY DATA: stands in for a dump of the worker's flow table until the table exists.
static size_t
fill_dummy_flows(struct flow_record *recs, uint16_t queue, uint64_t round)
{
	struct flow_record *tcp = &recs[0];
	struct flow_record *udp = &recs[1];

	memset(recs, 0, 2 * sizeof(*recs));

	tcp->family = 4;
	tcp->proto = 6;
	tcp->src_port = (uint16_t)(40000 + queue);
	tcp->dst_port = 443;
	tcp->src_ip[0] = 10;
	tcp->src_ip[2] = (uint8_t)queue;
	tcp->src_ip[3] = 1;
	tcp->dst_ip[0] = 10;
	tcp->dst_ip[2] = (uint8_t)queue;
	tcp->dst_ip[3] = 2;
	tcp->rx_bytes = round * 1500;
	tcp->tx_bytes = round * 900;
	tcp->rx_packets = round * 10;
	tcp->tx_packets = round * 6;

	udp->family = 6;
	udp->proto = 17;
	udp->src_port = 5353;
	udp->dst_port = 53;
	udp->src_ip[0] = 0x20;
	udp->src_ip[1] = 0x01;
	udp->src_ip[2] = 0x0d;
	udp->src_ip[3] = 0xb8;
	udp->src_ip[14] = (uint8_t)queue;
	udp->src_ip[15] = 1;
	memcpy(udp->dst_ip, udp->src_ip, sizeof(udp->dst_ip));
	udp->dst_ip[15] = 2;
	udp->rx_bytes = round * 120;
	udp->tx_bytes = round * 300;
	udp->rx_packets = round * 2;
	udp->tx_packets = round * 3;

	return 2;
}

static void
export_snapshot(uint16_t queue, uint64_t round)
{
	struct flow_record *recs = flow_export_begin(queue);

	if (recs == NULL) {
		RTE_LOG(WARNING, FLOWFWD, "export skipped for queue %u: the writer is still busy\n", queue);
		return;
	}
	flow_export_commit(queue, fill_dummy_flows(recs, queue, round));
}

static int
worker_main(void *arg)
{
	const struct worker_args *a = arg;
	struct worker_stats *stats = &worker_stats[a->queue];
	uint64_t busy = 0, total = 0, last, next_export, round = 0;

	RTE_LOG(DEBUG, FLOWFWD, "worker started: queue %u, ports %u <-> %u\n",
		a->queue, a->in_port, a->out_port);
	last = rte_rdtsc();
	next_export = last + export_cycles;
	while (!atomic_load_explicit(&quit, memory_order_relaxed)) {
		unsigned int handled = forward_burst(a->in_port, a->out_port, a->queue);
		uint64_t now;

		handled += forward_burst(a->out_port, a->in_port, a->queue);

		now = rte_rdtsc();
		total += now - last;
		if (handled > 0)
			busy += now - last;
		last = now;
		if (now >= next_export) {
			export_snapshot(a->queue, ++round);
			next_export = now + export_cycles;
		}
		atomic_store_explicit(&stats->busy_cycles, busy, memory_order_relaxed);
		atomic_store_explicit(&stats->total_cycles, total, memory_order_relaxed);
	}
	RTE_LOG(DEBUG, FLOWFWD, "worker stopped: queue %u\n", a->queue);
	return 0;
}

int
forward_start(const struct app_config *cfg)
{
	unsigned int lcores[MAX_QUEUES];
	unsigned int lcore, nb = 0;

	if (rte_lcore_count() - 1 < cfg->nb_queues) {
		RTE_LOG(ERR, FLOWFWD,
			"need %u worker lcores (one per queue) plus the main lcore, have %u in total\n",
			cfg->nb_queues, rte_lcore_count());
		return -1;
	}

	RTE_LCORE_FOREACH_WORKER(lcore) {
		if (nb == cfg->nb_queues)
			break;
		lcores[nb++] = lcore;
	}

	atomic_store(&quit, false);
	atomic_store(&rx_total, 0);
	atomic_store(&tx_total, 0);
	nb_workers = cfg->nb_queues;
	export_cycles = rte_get_tsc_hz() * cfg->export_interval;
	pass_arp = cfg->pass_arp != 0;

	if (flow_export_start(cfg, lcores, nb) != 0)
		return -1;

	for (uint16_t queue = 0; queue < nb; queue++) {
		worker_args[queue] = (struct worker_args){ queue, cfg->in_port, cfg->out_port };
		if (rte_eal_remote_launch(worker_main, &worker_args[queue], lcores[queue]) != 0) {
			RTE_LOG(ERR, FLOWFWD, "cannot launch worker on lcore %u\n", lcores[queue]);
			forward_stop();
			return -1;
		}
		RTE_LOG(INFO, FLOWFWD, "worker on lcore %u forwards queue %u\n", lcores[queue], queue);
	}
	return 0;
}

void
forward_stop(void)
{
	unsigned int lcore;

	atomic_store(&quit, true);
	RTE_LCORE_FOREACH_WORKER(lcore)
		rte_eal_wait_lcore(lcore);
	flow_export_stop();
}

uint64_t
forward_rx_packets(void)
{
	return atomic_load_explicit(&rx_total, memory_order_relaxed);
}

uint64_t
forward_tx_packets(void)
{
	return atomic_load_explicit(&tx_total, memory_order_relaxed);
}

static uint64_t
port_drops(uint16_t port)
{
	struct rte_eth_stats stats;

	if (rte_eth_stats_get(port, &stats) != 0)
		return 0;
	return stats.imissed + stats.rx_nombuf;
}

uint64_t
forward_nic_drops(const struct app_config *cfg)
{
	return port_drops(cfg->in_port) + port_drops(cfg->out_port);
}

void
forward_worker_usage(char *buf, size_t len)
{
	static uint64_t prev_busy[MAX_QUEUES], prev_total[MAX_QUEUES];
	size_t off = 0;

	buf[0] = '\0';
	for (unsigned int i = 0; i < nb_workers && off < len; i++) {
		uint64_t busy = atomic_load_explicit(&worker_stats[i].busy_cycles, memory_order_relaxed);
		uint64_t total = atomic_load_explicit(&worker_stats[i].total_cycles, memory_order_relaxed);
		uint64_t d_total = total - prev_total[i];
		unsigned int pct = d_total > 0 ? (unsigned int)((busy - prev_busy[i]) * 100 / d_total) : 0;

		prev_busy[i] = busy;
		prev_total[i] = total;
		off += (size_t)snprintf(buf + off, len - off, "%s%u%%", i > 0 ? "," : "", pct);
	}
}

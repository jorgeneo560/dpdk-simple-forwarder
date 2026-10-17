#ifndef FLOWFWD_FLOW_TABLE_H
#define FLOWFWD_FLOW_TABLE_H

#include <stdint.h>

#include <rte_mbuf.h>

#include "common.h"
#include "config.h"
#include "flow.h"
#include "parse.h"

/*
 * The flows one worker has seen, and the processing of the packets that
 * belong to them. A flow is identified by its 5-tuple: source IP, destination
 * IP, source port, destination port and protocol. A table belongs to a single
 * worker and is never shared, so it needs no locks.
 */
struct flow_table;

/*
 * Creates a table in memory of the given NUMA socket (the one the worker runs
 * on). id makes the table name unique. The filter, timeout and cleanup
 * settings are taken from cfg. Returns NULL on failure (message logged).
 */
struct flow_table *flow_table_create(unsigned int id, int socket,
                                     const struct app_config *cfg);

void flow_table_free(struct flow_table *table);

/* Headers of the packets kept by flow_table_remove_non_ip_pkts(), in the same
 * order as the packets. */
struct flow_batch {
  uint16_t count;
  uint16_t table_full_drops; // packets dropped because their flow did not fit
                             // in the table
  struct pkt_info info[BURST_SIZE];
  uint16_t len[BURST_SIZE]; // bytes of each kept packet on the wire
};

/*
 * Step 1, before sending: reads the headers of each packet of a received
 * burst, keeps the IP packets (and ARP if cfg->pass_arp) and frees the rest.
 * While the table is full (an insert failed and the cleaner has not freed room
 * yet), a TCP packet of a flow that is not in the table is dropped too and
 * counted in batch->table_full_drops. The kept packets are moved to the front
 * of bufs and their headers are saved in batch. Returns how many were kept.
 */
uint16_t flow_table_remove_non_ip_pkts(const struct flow_table *table,
                                       struct rte_mbuf **bufs, uint16_t n,
                                       struct flow_batch *batch);

/*
 * Step 2, after sending: adds the TCP flows of the batch to the table and
 * counts each packet in its flow: always as received, and as transmitted if it
 * is among the first nb_sent packets (the ones the NIC accepted). It works
 * from the saved headers because a sent packet is no longer ours to read. If
 * the table has no room for a new flow, it is marked full, see step 1.
 */
void flow_table_update(struct flow_table *table, const struct flow_batch *batch,
                       uint16_t nb_sent);

/*
 * Removes the flows that had no packets for cfg->flow_timeout seconds. Cheap
 * to call on every loop: it scans the table only when cfg->cleanup_interval
 * seconds have passed since the previous scan. now is rte_rdtsc().
 */
void flow_table_cleanup(struct flow_table *table, uint64_t now);

/* Number of flows currently in the table. */
unsigned int flow_table_count(const struct flow_table *table);

/* Starts a new pass over the flows of the table, for flow_table_export_next().
 */
void flow_table_export_begin(struct flow_table *table);

/*
 * Copies the next flows of the pass (at most max) into recs: their 5-tuple and
 * counters. Returns how many were copied, 0 when the pass is finished.
 */
size_t flow_table_export_next(struct flow_table *table,
                              struct flow_record *recs, size_t max);

#endif

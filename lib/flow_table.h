#ifndef FLOWFWD_FLOW_TABLE_H
#define FLOWFWD_FLOW_TABLE_H

#include <stdint.h>

#include <rte_mbuf.h>

#include "common.h"
#include "config.h"
#include "flow.h"
#include "parse.h"

/**
 * @brief The flows one queue has seen, and the processing of their packets.
 *
 * A flow is identified by its 5-tuple: source IP, destination IP, source port,
 * destination port and protocol. A table belongs to the RX thread of one
 * queue, which is the only one that adds, removes and counts received packets.
 * The TX thread of the same queue only adds to the sent counters of a flow,
 * which are kept apart from the rest. So no locks are needed.
 */
struct flow_table;

/**
 * @brief Creates a table.
 *
 * @param id     Makes the table name unique (one table per worker).
 * @param socket NUMA socket whose memory is used (the one the worker runs on).
 * @param cfg    Filter, timeout, cleanup and size settings.
 * @return The table, or NULL on failure (message logged).
 */
struct flow_table *flow_table_create(unsigned int id, int socket,
                                     const struct app_config *cfg);

/** @brief Frees a table. Accepts NULL. */
void flow_table_free(struct flow_table *table);

/** Slot value of a packet that is not tracked in the table. */
#define FLOW_NO_SLOT (-1)

/** Slot value of a packet whose flow does not fit in the full table: the caller
 *  has to drop it. */
#define FLOW_DROP (-2)

/**
 * Headers of the packets kept by flow_table_remove_non_ip_pkts(), in the same
 * order as the packets.
 */
struct flow_batch {
  uint16_t count;            ///< number of kept packets
  uint16_t table_full_drops; ///< packets dropped because their flow did not fit
  struct pkt_info info[BURST_SIZE]; ///< parsed headers of each kept packet
  uint16_t len[BURST_SIZE];         ///< bytes of each kept packet on the wire
  int32_t slot[BURST_SIZE]; ///< flow slot of each packet, or FLOW_NO_SLOT
  uint32_t gen[BURST_SIZE]; ///< generation of that slot when the packet was seen
};

/**
 * @brief Step 1 (RX thread): filters a received burst.
 *
 * Reads the headers of each packet, keeps the IP packets (and ARP if
 * cfg->pass_arp) and frees the rest. While the table is full (an insert failed
 * and the cleaner has not freed room yet), a TCP or UDP packet of a flow that is not
 * in the table is dropped too and counted in batch->table_full_drops. The kept
 * packets are moved to the front of bufs and their headers are saved in batch.
 *
 * @param table Worker's table.
 * @param bufs  Received packets; the kept ones are compacted to the front.
 * @param n     Number of received packets.
 * @param batch Receives the headers of the kept packets.
 * @return How many packets were kept.
 */
uint16_t flow_table_remove_non_ip_pkts(const struct flow_table *table,
                                       struct rte_mbuf **bufs, uint16_t n,
                                       struct flow_batch *batch);

/**
 * @brief Step 2 (RX thread): counts the packets of the batch as received.
 *
 * Adds the TCP and UDP flows to the table and counts each packet in its flow. The
 * flow slot of each packet is saved in batch->slot, so the TX thread can count
 * the packet as sent later. A packet that is not tracked (not TCP or UDP) gets
 * FLOW_NO_SLOT. A packet whose flow does not fit in the table gets FLOW_DROP
 * and marks the table full, see step 1: the caller drops it, like the packets
 * of new flows that step 1 already drops while the table is full.
 *
 * @param table Table of the queue.
 * @param batch Result of flow_table_remove_non_ip_pkts().
 */
void flow_table_track(struct flow_table *table, struct flow_batch *batch);

/**
 * @brief Step 3 (TX thread): counts one packet as sent in its flow.
 *
 * @param table Table of the queue.
 * @param slot  Slot saved by flow_table_track(); not FLOW_NO_SLOT.
 * @param gen   Generation saved with the slot. If the slot has since been given
 *              to another flow, the packet is ignored.
 * @param len   Bytes of the packet on the wire.
 */
void flow_table_count_sent(struct flow_table *table, int32_t slot, uint32_t gen,
                           uint16_t len);

/**
 * @brief Removes the flows that had no packets for cfg->flow_timeout seconds.
 *
 * Cheap to call on every loop: it scans the table only when
 * cfg->cleanup_interval seconds have passed since the previous scan.
 *
 * @param table Worker's table.
 * @param now   Current time, from rte_rdtsc().
 */
void flow_table_cleanup(struct flow_table *table, uint64_t now);

/** @brief Number of flows currently in the table. */
unsigned int flow_table_count(const struct flow_table *table);

/** @brief Starts a new pass over the flows, for flow_table_export_next(). */
void flow_table_export_begin(struct flow_table *table);

/**
 * @brief Copies the next flows of the pass into recs.
 *
 * Copies the 5-tuple and counters of each flow.
 *
 * @param table Worker's table.
 * @param recs  Receives the records.
 * @param max   Space available in recs.
 * @return How many were copied, 0 when the pass is finished.
 */
size_t flow_table_export_next(struct flow_table *table,
                              struct flow_record *recs, size_t max);

#endif

#ifndef FLOWFWD_FORWARD_H
#define FLOWFWD_FORWARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/**
 * @brief Launches an RX thread and a TX thread for every queue.
 *
 * For queue i, the RX thread reads queue i of both interfaces, filters and
 * counts the packets and hands them to the TX thread through a ring. The TX
 * thread sends them out on queue i of the other interface and counts them as
 * sent. The RX thread also owns the flow table of the queue (in the memory of
 * its own socket). The lcores are taken in pairs, so 2 * nb_queues worker
 * lcores are needed.
 *
 * @param cfg Configuration.
 * @return true on success, false on failure (message logged).
 */
bool forward_start(const struct app_config *cfg);

/** @brief Asks the workers to stop and waits for them. */
void forward_stop(void);

/** @brief Packets received from the interfaces, summed over all workers. */
uint64_t forward_rx_packets(void);

/** @brief Packets sent out through the interfaces, summed over all workers. */
uint64_t forward_tx_packets(void);

/**
 * @brief Packets lost by the application.
 *
 * Counts the packets dropped by the NICs before the workers saw them (receive
 * queue full, imissed; mbuf pool exhausted, rx_nombuf; summed over both ports)
 * plus the packets dropped because a flow table was full, because a ring
 * between an RX and a TX thread was full, or because the NIC did not accept
 * them for sending. A rising value means the threads
 * cannot keep up or the tables are too small.
 *
 * @param cfg Configuration with the port ids.
 */
uint64_t forward_drops(const struct app_config *cfg);

/** @brief Flows currently tracked, summed over all workers. */
uint64_t forward_flows(void);

/**
 * @brief Writes the load of each queue's threads since the previous call.
 *
 * The text has one "rx%/tx%" pair per queue, separated by commas, e.g.
 * "12%/10%,3%/3%". The load is the share of time a thread spent handling
 * packets instead of polling empty queues; it is not CPU use, since polling
 * always keeps the core busy. Near 100% means the thread is about to fall
 * behind.
 *
 * @param buf Receives the text.
 * @param len Size of buf.
 */
void forward_load(char *buf, size_t len);

#endif

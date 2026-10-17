#ifndef FLOWFWD_FORWARD_H
#define FLOWFWD_FORWARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "placement.h"

/*
 * Launches one worker per queue on the lcores chosen by placement_plan().
 * Worker i forwards queue i between the input and output port, in both
 * directions, and keeps its flow table in the memory of its own socket.
 * Returns true on success, false on failure (message logged).
 */
bool forward_start(const struct app_config *cfg, const struct placement *plan);

/* Asks the workers to stop and waits for them. */
void forward_stop(void);

/* Global counters: packets received from the interfaces / sent out through
 * them. */
uint64_t forward_rx_packets(void);
uint64_t forward_tx_packets(void);

/*
 * Packets lost by the application: dropped by the NICs before the workers saw
 * them (receive queue full, imissed; mbuf pool exhausted, rx_nombuf; summed
 * over both ports) plus the packets dropped because a flow table was full.
 * A rising value means the workers cannot keep up or the tables are too small.
 */
uint64_t forward_drops(const struct app_config *cfg);

/* Flows currently tracked, summed over all workers. */
uint64_t forward_flows(void);

/*
 * Writes the load of each worker since the previous call into buf, as
 * comma-separated percentages (e.g. "12%,3%"). The load is the share of time a
 * worker spent handling packets instead of polling empty queues; it is not CPU
 * use, since polling always keeps the core busy. Near 100% means the worker is
 * about to fall behind.
 */
void forward_worker_usage(char *buf, size_t len);

#endif

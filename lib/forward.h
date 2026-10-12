#ifndef FLOWFWD_FORWARD_H
#define FLOWFWD_FORWARD_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/*
 * Launches one worker per queue on the worker lcores. Worker i forwards queue i
 * between the input and output port, in both directions.
 * Returns 0 on success, -1 on failure (message logged).
 */
int forward_start(const struct app_config *cfg);

/* Asks the workers to stop and waits for them. */
void forward_stop(void);

/* Global counters: packets received from the interfaces / sent out through them. */
uint64_t forward_rx_packets(void);
uint64_t forward_tx_packets(void);

/*
 * Packets the NICs dropped before the workers saw them: receive queue full
 * (imissed) plus mbuf pool exhausted (rx_nombuf), summed over both ports.
 * A rising value means the workers cannot keep up.
 */
uint64_t forward_nic_drops(const struct app_config *cfg);

/*
 * Writes the load of each worker since the previous call into buf, as
 * comma-separated percentages (e.g. "12%,3%"). The load is the share of time a
 * worker spent handling packets instead of polling empty queues; it is not CPU
 * use, since polling always keeps the core busy. Near 100% means the worker is
 * about to fall behind.
 */
void forward_worker_usage(char *buf, size_t len);

#endif

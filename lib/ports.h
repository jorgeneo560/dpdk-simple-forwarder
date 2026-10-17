#ifndef FLOWFWD_PORTS_H
#define FLOWFWD_PORTS_H

#include "config.h"
#include "placement.h"

#define NB_PORTS 2

/*
 * Checks that the configured input and output ports exist, builds the
 * symmetric RSS key, creates one mbuf pool on every socket that has workers and
 * starts both ports with one queue per worker. Queue q of both ports takes its
 * packet buffers from the pool of the socket of worker q. Call after
 * rte_eal_init() and placement_plan().
 * Returns true on success, false on failure (reason logged).
 */
bool ports_start(const struct app_config *cfg, const struct placement *plan);

/* Stops and closes both ports. */
void ports_stop(const struct app_config *cfg);

#endif

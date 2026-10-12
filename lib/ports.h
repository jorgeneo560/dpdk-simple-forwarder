#ifndef FLOWFWD_PORTS_H
#define FLOWFWD_PORTS_H

#include "config.h"

#define NB_PORTS 2

/*
 * Checks that the configured input and output ports exist, builds the
 * symmetric RSS key, creates the mbuf pool and starts both ports with
 * cfg->nb_queues queues each. Call after rte_eal_init().
 * Returns 0 on success, -1 on failure (reason logged).
 */
int ports_start(const struct app_config *cfg);

/* Stops and closes both ports. */
void ports_stop(const struct app_config *cfg);

#endif

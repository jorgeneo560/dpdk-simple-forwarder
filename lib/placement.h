#ifndef FLOWFWD_PLACEMENT_H
#define FLOWFWD_PLACEMENT_H

#include <stdbool.h>

#include "config.h"

/* Where each worker runs. Worker i polls queue i of both ports. */
struct placement {
	unsigned int nb_workers;
	unsigned int lcores[MAX_QUEUES];
	int sockets[MAX_QUEUES];	// NUMA socket of each worker's lcore
};

/*
 * Chooses the lcores of the workers so that the packets stay in memory close to
 * the cores: the lcores on the NUMA socket of the NICs are used first, the
 * others only if there are not enough of them (a warning is logged).
 * Where a NIC is on a socket DPDK does not know (virtual devices, one-socket
 * machines) any lcore is fine. Call after rte_eal_init() and before
 * ports_start(). Returns false (message logged) if there are fewer worker
 * lcores than cfg->nb_queues.
 */
bool placement_plan(const struct app_config *cfg, struct placement *plan);

#endif

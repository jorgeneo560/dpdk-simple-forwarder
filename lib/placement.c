#include <stdbool.h>

#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_memory.h>

#include "common.h"
#include "placement.h"

// Socket the workers should run on: the one of the input NIC, or of the output
// NIC if that one is unknown. SOCKET_ID_ANY if DPDK does not know either.
static int
preferred_socket(const struct app_config *cfg)
{
	int in_socket = rte_eth_dev_socket_id(cfg->in_port);
	int out_socket = rte_eth_dev_socket_id(cfg->out_port);

	if (in_socket != SOCKET_ID_ANY && out_socket != SOCKET_ID_ANY &&
	    in_socket != out_socket) {
		RTE_LOG(WARNING, FLOWFWD,
			"input port %u is on socket %d and output port %u on socket %d: "
			"every worker reaches one of them across sockets\n",
			cfg->in_port, in_socket, cfg->out_port, out_socket);
	}
	return in_socket != SOCKET_ID_ANY ? in_socket : out_socket;
}

bool
placement_plan(const struct app_config *cfg, struct placement *plan)
{
	int wanted;
	unsigned int lcore, nb = 0;

	if (rte_lcore_count() - 1 < cfg->nb_queues) {
		RTE_LOG(ERR, FLOWFWD,
			"need %u worker lcores (one per queue) plus the main lcore, have %u in total\n",
			cfg->nb_queues, rte_lcore_count());
		return false;
	}

	wanted = preferred_socket(cfg);

	// Pass 0 takes the lcores of the wanted socket, pass 1 the others.
	for (int pass = 0; pass < 2; pass++) {
		RTE_LCORE_FOREACH_WORKER(lcore) {
			int socket = (int)rte_lcore_to_socket_id(lcore);
			bool local = wanted == SOCKET_ID_ANY || socket == wanted;

			if (nb == cfg->nb_queues) {
				break;
			}
			if (local != (pass == 0)) {
				continue;
			}
			if (!local) {
				RTE_LOG(WARNING, FLOWFWD,
					"worker lcore %u is on socket %d but the NICs are on socket %d: "
					"not enough lcores there, its packets cross sockets\n",
					lcore, socket, wanted);
			}
			plan->lcores[nb] = lcore;
			plan->sockets[nb] = socket;
			nb++;
		}
	}
	plan->nb_workers = nb;
	return true;
}

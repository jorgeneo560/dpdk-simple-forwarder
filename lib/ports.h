#ifndef FLOWFWD_PORTS_H
#define FLOWFWD_PORTS_H

#include "config.h"

/** Number of network interfaces the application uses (input and output). */
#define NB_PORTS 2

/**
 * @brief Starts the input and output interfaces.
 *
 * Checks that the configured ports exist, builds the symmetric RSS key,
 * creates the mbuf pool and starts both ports with one queue per worker. Call
 * after rte_eal_init().
 *
 * @param cfg Configuration with the port ids, queue count and RSS pattern.
 * @return true on success, false on failure (reason logged).
 */
bool ports_start(const struct app_config *cfg);

/**
 * @brief Stops and closes both ports.
 *
 * @param cfg Configuration with the port ids.
 */
void ports_stop(const struct app_config *cfg);

#endif

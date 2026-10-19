#ifndef FLOWFWD_COMMON_H
#define FLOWFWD_COMMON_H

/** @file common.h
 *  @brief Definitions shared by all modules.
 */

#include <rte_log.h>

/** Log type of the application, used as RTE_LOG(level, FLOWFWD, ...). */
#define RTE_LOGTYPE_FLOWFWD RTE_LOGTYPE_USER1

/** Packets a worker reads from a queue in one call (one burst). */
#define BURST_SIZE 32

/** Packets the RX thread of a queue can hold for its TX thread, per direction.
 *  A power of 2. */
#define RING_SIZE 1024

#endif

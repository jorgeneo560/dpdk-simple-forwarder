#ifndef FLOWFWD_FLOW_EXPORT_H
#define FLOWFWD_FLOW_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "config.h"
#include "flow.h"

/** Most records a worker can hand over at once; a larger table is exported in
 *  several chunks. */
#define FLOW_EXPORT_MAX 4096

/**
 * @brief Opens the CSV files and starts the writer thread.
 *
 * Each queue gets two files, flow_stats_core_<lcore>.csv for its RX lcore and
 * one for its TX lcore (next to the config file). Both receive the same rows,
 * so the rows of a queue must be counted once, not twice. The workers never
 * touch files: they fill a buffer and the writer thread writes it out.
 *
 * @param cfg       Configuration (file location, export interval for the log).
 * @param rx_lcores RX lcore of each queue; queue i uses rx_lcores[i].
 * @param tx_lcores TX lcore of each queue; queue i uses tx_lcores[i].
 * @param n         Number of queues.
 * @return true on success, false on failure (message logged).
 */
bool flow_export_start(const struct app_config *cfg,
                       const unsigned int *rx_lcores,
                       const unsigned int *tx_lcores, unsigned int n);

/**
 * @brief Worker side, step 1: gets the buffer to fill.
 *
 * @param worker Worker index.
 * @return A buffer of FLOW_EXPORT_MAX records, or NULL if the writer has not
 *         finished the previous export yet.
 */
struct flow_record *flow_export_begin(unsigned int worker);

/**
 * @brief Worker side, step 2: hands the filled buffer to the writer.
 *
 * All chunks of one export use the same timestamp.
 *
 * @param worker Worker index.
 * @param ts     Timestamp written in every row.
 * @param n      Number of records filled, from the start of the buffer.
 */
void flow_export_commit(unsigned int worker, const struct timespec *ts,
                        size_t n);

/**
 * @brief Writes out anything pending, stops the writer thread and closes the
 *        files.
 *
 * Safe to call twice.
 */
void flow_export_stop(void);

#endif

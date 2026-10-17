#ifndef FLOWFWD_FLOW_EXPORT_H
#define FLOWFWD_FLOW_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "config.h"
#include "flow.h"

/* Most records a worker can hand over at once. A larger table is exported in
 * several chunks. */
#define FLOW_EXPORT_MAX 4096

/*
 * Opens flow_stats_core_<lcore>.csv (next to the config file) for each worker
 * lcore and starts the writer thread. Worker i uses lcores[i]. The workers
 * never touch files: they fill a buffer and the writer thread writes it out.
 * Returns true on success, false on failure (message logged).
 */
bool flow_export_start(const struct app_config *cfg, const unsigned int *lcores,
                       unsigned int n);

/*
 * Worker side, step 1: returns a buffer of FLOW_EXPORT_MAX records to fill, or
 * NULL if the writer has not finished the previous export yet.
 */
struct flow_record *flow_export_begin(unsigned int worker);

/*
 * Worker side, step 2: hands the first n records of the buffer to the writer,
 * which writes them stamped with ts. All chunks of one export use the same ts.
 */
void flow_export_commit(unsigned int worker, const struct timespec *ts,
                        size_t n);

/* Writes out anything pending, stops the writer thread and closes the files.
 * Safe to call twice. */
void flow_export_stop(void);

#endif

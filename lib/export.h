#ifndef FLOWFWD_EXPORT_H
#define FLOWFWD_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include "flow.h"

/** Handle of one open CSV export file. */
struct flow_exporter {
  FILE *file; ///< the open file
};

/**
 * @brief Opens a CSV file for appending.
 *
 * The CSV header line is written only if the file is empty.
 *
 * @param e    Exporter to initialise.
 * @param path File to open.
 * @return true on success, false on failure (errno is set).
 */
bool export_open(struct flow_exporter *e, const char *path);

/**
 * @brief Appends one row per record, all stamped with ts, then flushes.
 *
 * Row format: "timestamp,src_ip,dst_ip,src_port,dst_port,protocol,rx_bytes,
 * tx_bytes,rx_packets,tx_packets", with the timestamp in Unix seconds with
 * milliseconds.
 *
 * @param e    Open exporter.
 * @param ts   Timestamp written in every row.
 * @param recs Records to write.
 * @param n    Number of records.
 * @return true on success, false on failure (errno is set; EINVAL for a bad
 *         record).
 */
bool export_write(struct flow_exporter *e, const struct timespec *ts,
                  const struct flow_record *recs, size_t n);

/** @brief Closes the file. Safe to call twice. */
void export_close(struct flow_exporter *e);

#endif

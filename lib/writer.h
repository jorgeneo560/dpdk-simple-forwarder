#ifndef FLOWFWD_WRITER_H
#define FLOWFWD_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include "flow.h"

/** One open CSV file. */
struct writer {
  FILE *file; ///< the open file
};

/**
 * @brief Opens a CSV file for appending.
 *
 * The CSV header line is written only if the file is empty.
 *
 * @param e    Writer to initialise.
 * @param path File to open.
 * @return true on success, false on failure (errno is set).
 */
bool writer_open(struct writer *e, const char *path);

/**
 * @brief Appends one row per record, all stamped with ts, then flushes.
 *
 * Row format: "timestamp,src_ip,dst_ip,src_port,dst_port,protocol,rx_bytes,
 * tx_bytes,rx_packets,tx_packets", with the timestamp in Unix seconds with
 * milliseconds.
 *
 * @param e    Open writer.
 * @param ts   Timestamp written in every row.
 * @param recs Records to write.
 * @param n    Number of records.
 * @return true on success, false on failure (errno is set; EINVAL for a bad
 *         record).
 */
bool writer_write(struct writer *e, const struct timespec *ts,
                  const struct flow_record *recs, size_t n);

/** @brief Closes the file. Safe to call twice. */
void writer_close(struct writer *e);

#endif

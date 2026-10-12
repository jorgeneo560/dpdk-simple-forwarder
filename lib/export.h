#ifndef FLOWFWD_EXPORT_H
#define FLOWFWD_EXPORT_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include "flow.h"

struct flow_exporter {
	FILE *file;
};

/*
 * Opens path for appending; writes the CSV header only if the file is empty.
 * Returns 0 on success, -1 on failure (errno is set).
 */
int export_open(struct flow_exporter *e, const char *path);

/*
 * Appends one row per record, all stamped with ts, then flushes. Rows are
 * "timestamp,src_ip,dst_ip,src_port,dst_port,protocol,rx_bytes,tx_bytes,
 * rx_packets,tx_packets" with the timestamp in Unix seconds with milliseconds.
 * Returns 0 on success, -1 on failure (errno is set; EINVAL for a bad record).
 */
int export_write(struct flow_exporter *e, const struct timespec *ts,
		 const struct flow_record *recs, size_t n);

/* Closes the file. Safe to call twice. */
void export_close(struct flow_exporter *e);

#endif

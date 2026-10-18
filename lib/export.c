#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "export.h"

#define CSV_HEADER                                                             \
  "timestamp,src_ip,dst_ip,src_port,dst_port,protocol,rx_bytes,tx_bytes,rx_"   \
  "packets,tx_packets\n"

bool export_open(struct flow_exporter *e, const char *path) {
  struct stat st;
  int err;

  e->file = fopen(path, "a");
  if (e->file == NULL)
    return false;

  if (fstat(fileno(e->file), &st) != 0 ||
      (st.st_size == 0 &&
       (fputs(CSV_HEADER, e->file) == EOF || fflush(e->file) != 0))) {
    err = errno;
    fclose(e->file);
    e->file = NULL;
    errno = err;
    return false;
  }
  return true;
}

bool export_write(struct flow_exporter *e, const struct timespec *ts,
                  const struct flow_record *recs, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const struct flow_record *r = &recs[i];
    char src[INET6_ADDRSTRLEN], dst[INET6_ADDRSTRLEN];
    int af = r->family == 6 ? AF_INET6 : AF_INET;

    if ((r->family != 4 && r->family != 6) ||
        inet_ntop(af, &r->src_ip, src, sizeof(src)) == NULL ||
        inet_ntop(af, &r->dst_ip, dst, sizeof(dst)) == NULL) {
      errno = EINVAL;
      return false;
    }
    fprintf(e->file,
            "%lld.%03ld,%s,%s,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 "\n",
            (long long)ts->tv_sec, ts->tv_nsec / 1000000, src, dst, r->src_port,
            r->dst_port, r->proto, r->rx_bytes, r->tx_bytes, r->rx_packets,
            r->tx_packets);
  }
  return fflush(e->file) == 0 && !ferror(e->file);
}

void export_close(struct flow_exporter *e) {
  if (e->file != NULL) {
    fclose(e->file);
    e->file = NULL;
  }
}

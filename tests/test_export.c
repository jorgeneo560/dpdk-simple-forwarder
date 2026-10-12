#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "export.h"

#define LINE_LEN  256
#define MAX_LINES 16

static int failures;

#define CHECK(cond, msg)						\
	do {								\
		if (cond) {						\
			printf("ok   %s\n", msg);			\
		} else {						\
			printf("FAIL %s\n", msg);			\
			failures++;					\
		}							\
	} while (0)

static struct flow_record
make_flow(uint8_t family, const char *src, const char *dst, uint16_t sport, uint16_t dport,
	  uint8_t proto, uint64_t rx_bytes, uint64_t tx_bytes, uint64_t rx_pkts, uint64_t tx_pkts)
{
	struct flow_record r;
	int af = family == 6 ? AF_INET6 : AF_INET;

	memset(&r, 0, sizeof(r));
	r.family = family;
	r.proto = proto;
	r.src_port = sport;
	r.dst_port = dport;
	inet_pton(af, src, r.src_ip);
	inet_pton(af, dst, r.dst_ip);
	r.rx_bytes = rx_bytes;
	r.tx_bytes = tx_bytes;
	r.rx_packets = rx_pkts;
	r.tx_packets = tx_pkts;
	return r;
}

static size_t
read_lines(const char *path, char lines[][LINE_LEN], size_t max)
{
	FILE *f = fopen(path, "r");
	size_t n = 0;

	if (f == NULL)
		return 0;
	while (n < max && fgets(lines[n], LINE_LEN, f) != NULL) {
		lines[n][strcspn(lines[n], "\n")] = '\0';
		n++;
	}
	fclose(f);
	return n;
}

int
main(void)
{
	static const char header[] =
		"timestamp,src_ip,dst_ip,src_port,dst_port,protocol,rx_bytes,tx_bytes,rx_packets,tx_packets";
	char dir[] = "/tmp/flowfwd_test_XXXXXX";
	char path[128];
	char lines[MAX_LINES][LINE_LEN];
	struct flow_exporter e;
	struct timespec t1 = { .tv_sec = 1700000000, .tv_nsec = 123000000 };
	struct timespec t2 = { .tv_sec = 1700000010, .tv_nsec = 5000000 };
	struct flow_record first[2], second[1];
	size_t n, headers;

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(path, sizeof(path), "%s/flow_stats_core_3.csv", dir);

	first[0] = make_flow(4, "10.0.0.1", "10.0.0.2", 51234, 443, 6, 1200, 800, 10, 8);
	first[1] = make_flow(6, "2001:db8::1", "2001:db8::2", 5353, 53, 17, 90, 120, 1, 2);
	second[0] = make_flow(4, "10.0.0.1", "10.0.0.2", 51234, 443, 6, 4000, 2500, 30, 25);

	CHECK(export_open(&e, path) == 0, "open creates the file");
	CHECK(export_write(&e, &t1, first, 2) == 0, "write two records (IPv4 and IPv6)");
	CHECK(export_write(&e, &t2, second, 1) == 0, "write the same flow again at a later time");
	export_close(&e);
	export_close(&e);

	n = read_lines(path, lines, MAX_LINES);
	CHECK(n == 4, "file has a header and three rows");
	CHECK(n > 0 && strcmp(lines[0], header) == 0, "header line is correct");
	CHECK(n > 1 && strcmp(lines[1], "1700000000.123,10.0.0.1,10.0.0.2,51234,443,6,1200,800,10,8") == 0,
	      "IPv4 row is correct");
	CHECK(n > 2 && strcmp(lines[2], "1700000000.123,2001:db8::1,2001:db8::2,5353,53,17,90,120,1,2") == 0,
	      "IPv6 row is correct");
	CHECK(n > 3 && strcmp(lines[3], "1700000010.005,10.0.0.1,10.0.0.2,51234,443,6,4000,2500,30,25") == 0,
	      "later record of the same flow has its own timestamp and counters");

	CHECK(export_open(&e, path) == 0, "reopening an existing file works");
	CHECK(export_write(&e, &t2, second, 1) == 0, "append after reopening");
	export_close(&e);

	n = read_lines(path, lines, MAX_LINES);
	headers = 0;
	for (size_t i = 0; i < n; i++)
		if (strncmp(lines[i], "timestamp,", 10) == 0)
			headers++;
	CHECK(n == 5, "reopening appends and keeps the earlier rows");
	CHECK(headers == 1, "the header is written only once");

	CHECK(export_open(&e, "/nonexistent_dir/x.csv") == -1 && errno == ENOENT,
	      "opening a path that cannot be created fails with ENOENT");

	first[0].family = 5;
	CHECK(export_open(&e, path) == 0, "open for the bad-record test");
	CHECK(export_write(&e, &t1, first, 1) == -1 && errno == EINVAL,
	      "a record with an invalid IP family is rejected with EINVAL");
	export_close(&e);
	CHECK(read_lines(path, lines, MAX_LINES) == 5, "the rejected record wrote nothing");

	unlink(path);
	rmdir(dir);

	printf("%s (%d failure(s))\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures);
	return failures == 0 ? 0 : 1;
}

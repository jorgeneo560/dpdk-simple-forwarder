#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "parse.h"

static int failures;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      printf("ok   %s\n", msg);                                                \
    } else {                                                                   \
      printf("FAIL %s\n", msg);                                                \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xff);
}

// Writes MACs, up to two VLAN tags and the final EtherType; returns the header
// length.
static size_t put_eth(uint8_t *p, int vlan_tags, uint16_t type) {
  size_t off = 12;

  memset(p, 0x11, 6);
  memset(p + 6, 0x22, 6);
  for (int i = 0; i < vlan_tags; i++) {
    put16(p + off, (vlan_tags == 2 && i == 0) ? 0x88a8 : 0x8100);
    put16(p + off + 2, 0x0064);
    off += 4;
  }
  put16(p + off, type);
  return off + 2;
}

static void put_ipv4(uint8_t *p, unsigned int ihl_words, uint16_t total,
                     uint8_t proto, const char *src, const char *dst,
                     uint16_t frag) {
  memset(p, 0, ihl_words * 4);
  p[0] = (uint8_t)(0x40 | ihl_words);
  put16(p + 2, total);
  put16(p + 6, frag);
  p[8] = 64;
  p[9] = proto;
  inet_pton(AF_INET, src, p + 12);
  inet_pton(AF_INET, dst, p + 16);
}

static void put_ipv6(uint8_t *p, uint16_t payload, uint8_t next,
                     const char *src, const char *dst) {
  memset(p, 0, 40);
  p[0] = 0x60;
  put16(p + 4, payload);
  p[6] = next;
  p[7] = 64;
  inet_pton(AF_INET6, src, p + 8);
  inet_pton(AF_INET6, dst, p + 24);
}

static void put_l4(uint8_t *p, uint16_t sport, uint16_t dport, size_t len) {
  memset(p, 0, len);
  put16(p, sport);
  put16(p + 2, dport);
}

static void check_flow(const char *name, const uint8_t *buf, size_t len,
                       uint8_t family, uint8_t proto, const char *src,
                       const char *dst, uint16_t sport, uint16_t dport) {
  struct pkt_info info;
  uint8_t want_src[16] = {0}, want_dst[16] = {0};
  int af = family == 6 ? AF_INET6 : AF_INET;
  int ok;

  inet_pton(af, src, want_src);
  inet_pton(af, dst, want_dst);
  ok = parse_packet(buf, len, &info) == PARSE_IP && info.family == family &&
       info.proto == proto && info.src_port == sport &&
       info.dst_port == dport && memcmp(&info.src_ip, want_src, 16) == 0 &&
       memcmp(&info.dst_ip, want_dst, 16) == 0;
  CHECK(ok, name);
}

static void check_result(const char *name, const uint8_t *buf, size_t len,
                         enum parse_result want) {
  struct pkt_info info;

  CHECK(parse_packet(buf, len, &info) == want, name);
}

int main(void) {
  uint8_t buf[256];
  struct pkt_info info;
  size_t n;

  n = put_eth(buf, 0, 0x0800);
  put_ipv4(buf + n, 5, 40, 6, "10.0.0.1", "10.0.0.2", 0);
  put_l4(buf + n + 20, 51234, 443, 20);
  check_flow("IPv4 TCP", buf, n + 40, 4, 6, "10.0.0.1", "10.0.0.2", 51234, 443);

  n = put_eth(buf, 0, 0x0800);
  put_ipv4(buf + n, 6, 32, 17, "192.168.1.5", "8.8.8.8", 0);
  put_l4(buf + n + 24, 5353, 53, 8);
  check_flow("IPv4 UDP with IP options", buf, n + 32, 4, 17, "192.168.1.5",
             "8.8.8.8", 5353, 53);

  n = put_eth(buf, 1, 0x0800);
  put_ipv4(buf + n, 5, 28, 17, "10.1.1.1", "10.1.1.2", 0);
  put_l4(buf + n + 20, 1000, 2000, 8);
  check_flow("IPv4 UDP behind one VLAN tag", buf, n + 28, 4, 17, "10.1.1.1",
             "10.1.1.2", 1000, 2000);

  n = put_eth(buf, 2, 0x0800);
  put_ipv4(buf + n, 5, 40, 6, "10.2.2.1", "10.2.2.2", 0);
  put_l4(buf + n + 20, 80, 8080, 20);
  check_flow("IPv4 TCP behind two VLAN tags (QinQ)", buf, n + 40, 4, 6,
             "10.2.2.1", "10.2.2.2", 80, 8080);

  n = put_eth(buf, 2, 0x8100);
  put16(buf + n, 0x0064);
  put16(buf + n + 2, 0x0800);
  memset(buf + n + 4, 0, 40);
  check_result("three VLAN tags are rejected", buf, n + 44, PARSE_MALFORMED);

  n = put_eth(buf, 0, 0x86dd);
  put_ipv6(buf + n, 8, 17, "2001:db8::1", "2001:db8::2");
  put_l4(buf + n + 40, 5353, 53, 8);
  check_flow("IPv6 UDP", buf, n + 48, 6, 17, "2001:db8::1", "2001:db8::2", 5353,
             53);

  n = put_eth(buf, 1, 0x86dd);
  put_ipv6(buf + n, 20, 6, "fe80::1", "fe80::2");
  put_l4(buf + n + 40, 22, 40000, 20);
  check_flow("IPv6 TCP behind a VLAN tag", buf, n + 60, 6, 6, "fe80::1",
             "fe80::2", 22, 40000);

  n = put_eth(buf, 0, 0x86dd);
  put_ipv6(buf + n, 16, 0, "2001:db8::a", "2001:db8::b");
  memset(buf + n + 40, 0, 16);
  check_flow("IPv6 with an extension header: proto only, no ports", buf, n + 56,
             6, 0, "2001:db8::a", "2001:db8::b", 0, 0);

  n = put_eth(buf, 0, 0x0800);
  put_ipv4(buf + n, 5, 28, 1, "10.0.0.1", "10.0.0.9", 0);
  memset(buf + n + 20, 0, 8);
  check_flow("IPv4 ICMP has no ports", buf, n + 28, 4, 1, "10.0.0.1",
             "10.0.0.9", 0, 0);

  n = put_eth(buf, 0, 0x0800);
  put_ipv4(buf + n, 5, 28, 17, "10.0.0.1", "10.0.0.2", 0x00b9);
  put_l4(buf + n + 20, 1111, 2222, 8);
  check_flow("non-first IPv4 fragment has no ports", buf, n + 28, 4, 17,
             "10.0.0.1", "10.0.0.2", 0, 0);

  n = put_eth(buf, 0, 0x0800);
  put_ipv4(buf + n, 5, 28, 17, "10.0.0.1", "10.0.0.2", 0x2000);
  put_l4(buf + n + 20, 1111, 2222, 8);
  check_flow("first IPv4 fragment keeps its ports", buf, n + 28, 4, 17,
             "10.0.0.1", "10.0.0.2", 1111, 2222);

  memset(buf, 0, sizeof(buf));
  n = put_eth(buf, 0, 0x0800);
  put_ipv4(buf + n, 5, 28, 17, "10.0.0.3", "10.0.0.4", 0);
  put_l4(buf + n + 20, 53, 53, 8);
  check_flow("short frame padded to 60 bytes", buf, 60, 4, 17, "10.0.0.3",
             "10.0.0.4", 53, 53);

  n = put_eth(buf, 0, 0x0806);
  CHECK(parse_packet(buf, 60, &info) == PARSE_ARP && info.family == 0 &&
            info.proto == 0 && info.ether_type == 0x0806,
        "ARP is recognised and carries no IP fields");

  n = put_eth(buf, 0, 0x88cc);
  CHECK(parse_packet(buf, 60, &info) == PARSE_OTHER &&
            info.ether_type == 0x88cc,
        "LLDP is reported as other");
  (void)n;

  n = put_eth(buf, 0, 0x0800);
  check_result("frame shorter than an Ethernet header", buf, 13,
               PARSE_MALFORMED);

  put_ipv4(buf + n, 5, 40, 6, "10.0.0.1", "10.0.0.2", 0);
  check_result("IPv4 header cut short", buf, n + 10, PARSE_MALFORMED);

  put_ipv4(buf + n, 4, 40, 6, "10.0.0.1", "10.0.0.2", 0);
  check_result("IPv4 header length below 5 words", buf, n + 40,
               PARSE_MALFORMED);

  put_ipv4(buf + n, 5, 40, 6, "10.0.0.1", "10.0.0.2", 0);
  buf[n] = 0x55;
  check_result("IPv4 with the wrong version", buf, n + 40, PARSE_MALFORMED);

  put_ipv4(buf + n, 5, 100, 6, "10.0.0.1", "10.0.0.2", 0);
  check_result("IPv4 total length larger than the frame", buf, n + 40,
               PARSE_MALFORMED);

  put_ipv4(buf + n, 5, 22, 6, "10.0.0.1", "10.0.0.2", 0);
  check_result("TCP header cut before the ports", buf, n + 22, PARSE_MALFORMED);

  n = put_eth(buf, 0, 0x86dd);
  put_ipv6(buf + n, 8, 17, "2001:db8::1", "2001:db8::2");
  check_result("IPv6 payload longer than the frame", buf, n + 44,
               PARSE_MALFORMED);

  put_ipv6(buf + n, 8, 17, "2001:db8::1", "2001:db8::2");
  buf[n] = 0x50;
  check_result("IPv6 with the wrong version", buf, n + 48, PARSE_MALFORMED);

  printf("%s (%d failure(s))\n",
         failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures);
  return failures == 0 ? 0 : 1;
}

#ifndef FLOWFWD_PARSE_H
#define FLOWFWD_PARSE_H

#include <stddef.h>
#include <stdint.h>

enum parse_result {
  PARSE_IP, // IPv4 or IPv6, info is filled in
  PARSE_ARP,
  PARSE_OTHER,     // any other EtherType
  PARSE_MALFORMED, // truncated or inconsistent headers
};

/* What parse_packet() extracts. All fields are zero unless the packet is IP. */
struct pkt_info {
  uint16_t ether_type; // after any VLAN tags (also set for ARP and other)
  uint8_t family;      // 4 or 6
  uint8_t proto;       // IP protocol (IPv6: the next-header field)
  uint16_t src_port;   // host byte order; 0 when the protocol has no ports
  uint16_t dst_port;
  uint8_t src_ip[16]; // network byte order; IPv4 uses the first 4 bytes
  uint8_t dst_ip[16];
};

/*
 * Reads the headers of the Ethernet frame in data[0..len). Handles up to two
 * VLAN tags (802.1Q, 802.1ad), IPv4 with options, and IPv6. Ports are read for
 * TCP and UDP only; a non-first IPv4 fragment has none. IPv6 extension headers
 * are not walked: the proto is the first next-header and ports stay 0 unless
 * TCP/UDP follows the fixed header directly.
 */
enum parse_result parse_packet(const uint8_t *data, size_t len,
                               struct pkt_info *info);

#endif

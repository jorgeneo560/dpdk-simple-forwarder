#ifndef FLOWFWD_PARSE_H
#define FLOWFWD_PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "ip_addr.h"

/** Result of parse_packet(). */
enum parse_result {
  PARSE_IP,        ///< IPv4 or IPv6, info is filled in
  PARSE_ARP,       ///< ARP frame
  PARSE_OTHER,     ///< any other EtherType
  PARSE_MALFORMED, ///< truncated or inconsistent headers
};

/** What parse_packet() extracts. All fields are zero unless the packet is IP. */
struct pkt_info {
  uint16_t ether_type; ///< after any VLAN tags (also set for ARP and other)
  uint8_t family;      ///< 4 or 6
  uint8_t proto;       ///< IP protocol (IPv6: the next-header field)
  uint16_t src_port;   ///< host byte order; 0 when the protocol has no ports
  uint16_t dst_port;   ///< host byte order
  union ip_addr src_ip; ///< valid member given by family; zero unless IP
  union ip_addr dst_ip;
};

/**
 * @brief Reads the headers of an Ethernet frame.
 *
 * Handles up to two VLAN tags (802.1Q, 802.1ad), IPv4 with options, and IPv6.
 * Ports are read for TCP and UDP only; a non-first IPv4 fragment has none.
 * IPv6 extension headers are not walked: proto is the first next-header and
 * the ports stay 0 unless TCP/UDP follows the fixed header directly.
 *
 * @param data Start of the frame.
 * @param len  Bytes available in data.
 * @param info Receives the extracted fields.
 * @return What kind of packet it is.
 */
enum parse_result parse_packet(const uint8_t *data, size_t len,
                               struct pkt_info *info);

#endif

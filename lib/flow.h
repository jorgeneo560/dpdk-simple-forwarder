#ifndef FLOWFWD_FLOW_H
#define FLOWFWD_FLOW_H

#include <stdint.h>

/* Snapshot of one tracked flow, as written to the export file. */
struct flow_record {
  uint8_t family;    // 4 or 6
  uint8_t proto;     // IP protocol number (6 = TCP, 17 = UDP)
  uint16_t src_port; // host byte order, 0 if the protocol has no ports
  uint16_t dst_port;
  uint8_t src_ip[16]; // network byte order; IPv4 uses the first 4 bytes
  uint8_t dst_ip[16];
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint64_t rx_packets;
  uint64_t tx_packets;
};

#endif

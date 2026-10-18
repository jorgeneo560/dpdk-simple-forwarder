#ifndef FLOWFWD_FLOW_H
#define FLOWFWD_FLOW_H

#include <stdint.h>

#include "ip_addr.h"

/** Snapshot of one tracked flow, as written to the export file. */
struct flow_record {
  uint8_t family;    ///< 4 or 6
  uint8_t proto;     ///< IP protocol number (6 = TCP, 17 = UDP)
  uint16_t src_port; ///< host byte order, 0 if the protocol has no ports
  uint16_t dst_port; ///< host byte order
  union ip_addr src_ip; ///< valid member given by family
  union ip_addr dst_ip;
  uint64_t rx_bytes;   ///< bytes received
  uint64_t tx_bytes;   ///< bytes sent
  uint64_t rx_packets; ///< packets received
  uint64_t tx_packets; ///< packets sent
};

#endif

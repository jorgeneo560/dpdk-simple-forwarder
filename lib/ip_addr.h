#ifndef FLOWFWD_IP_ADDR_H
#define FLOWFWD_IP_ADDR_H

#include <stdint.h>

/**
 * An IPv4 or IPv6 address in network byte order. Which member is valid is
 * given by the IP version stored next to it (4 or 6). An IPv4 address leaves
 * the rest of the union zero, so two addresses can be compared as 16 bytes.
 */
union ip_addr {
  uint8_t v4[4];
  uint8_t v6[16];
};

#endif

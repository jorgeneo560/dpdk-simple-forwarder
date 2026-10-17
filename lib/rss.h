#ifndef FLOWFWD_RSS_H
#define FLOWFWD_RSS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Fills key[0..len) with the 16-bit pattern repeated (big-endian). A Toeplitz
 * key that repeats every 16 bits makes the NIC's RSS hash symmetric: swapping
 * source and destination (addresses and ports) gives the same hash, so both
 * directions of a flow reach the same receive queue.
 */
void rss_fill_key(uint8_t *key, size_t len, uint16_t pattern);

#endif

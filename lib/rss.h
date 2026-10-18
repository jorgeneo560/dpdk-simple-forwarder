#ifndef FLOWFWD_RSS_H
#define FLOWFWD_RSS_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Fills an RSS key with a repeated 16-bit pattern (big-endian).
 *
 * A Toeplitz key that repeats every 16 bits makes the NIC's RSS hash
 * symmetric: swapping source and destination (addresses and ports) gives the
 * same hash, so both directions of a flow reach the same receive queue.
 *
 * @param key     Buffer to fill, key[0..len).
 * @param len     Size of the key in bytes.
 * @param pattern The 16-bit pattern to repeat.
 */
void rss_fill_key(uint8_t *key, size_t len, uint16_t pattern);

#endif

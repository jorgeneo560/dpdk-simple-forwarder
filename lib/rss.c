#include "rss.h"

void rss_fill_key(uint8_t *key, size_t len, uint16_t pattern) {//ok
  for (size_t i = 0; i < len; i++) {
    key[i] = (i % 2 == 0) ? (uint8_t)(pattern >> 8) : (uint8_t)(pattern & 0xff);
  }
}

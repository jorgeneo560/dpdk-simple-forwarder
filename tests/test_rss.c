#include <stdio.h>
#include <string.h>

#include "rss.h"

#define KEY_LEN 40 // what the Intel X540 uses
#define NB_FLOWS 10000
#define NB_QUEUES 4

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

// The Toeplitz hash as the NIC computes it: for every set input bit, XOR in the
// 32 key bits starting there.
static uint32_t toeplitz(const uint8_t *key, size_t key_len, const uint8_t *in,
                         size_t in_len) {
  uint32_t hash = 0;
  uint32_t window =
      ((uint32_t)key[0] << 24) | (key[1] << 16) | (key[2] << 8) | key[3];

  for (size_t i = 0; i < in_len; i++) {
    for (int b = 7; b >= 0; b--) {
      size_t next_bit = i * 8 + (size_t)(7 - b) + 32;

      if (in[i] & (1u << b)) {
        hash ^= window;
      }
      window <<= 1;
      if (next_bit / 8 < key_len &&
          (key[next_bit / 8] >> (7 - next_bit % 8)) & 1) {
        window |= 1;
      }
    }
  }
  return hash;
}

static uint32_t rng_state = 2463534242u;

static uint32_t next_random(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static void fill_random(uint8_t *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    p[i] = (uint8_t)next_random();
  }
}

// Input layout of the 4-tuple hash: src addr, dst addr, src port, dst port.
static size_t make_input(uint8_t *in, const uint8_t *src, const uint8_t *dst,
                         size_t addr_len, const uint8_t *sport,
                         const uint8_t *dport) {
  memcpy(in, src, addr_len);
  memcpy(in + addr_len, dst, addr_len);
  memcpy(in + 2 * addr_len, sport, 2);
  memcpy(in + 2 * addr_len + 2, dport, 2);
  return 2 * addr_len + 4;
}

// How many random flows hash differently when source and destination are
// swapped.
static int count_asymmetric(const uint8_t *key, size_t addr_len) {
  int asymmetric = 0;

  for (int n = 0; n < NB_FLOWS; n++) {
    uint8_t a[16], b[16], sp[2], dp[2], fwd[36], rev[36];
    size_t len;

    fill_random(a, sizeof(a));
    fill_random(b, sizeof(b));
    fill_random(sp, sizeof(sp));
    fill_random(dp, sizeof(dp));
    len = make_input(fwd, a, b, addr_len, sp, dp);
    make_input(rev, b, a, addr_len, dp, sp);
    if (toeplitz(key, KEY_LEN, fwd, len) != toeplitz(key, KEY_LEN, rev, len)) {
      asymmetric++;
    }
  }
  return asymmetric;
}

int main(void) {
  uint8_t key[KEY_LEN], other[KEY_LEN];
  int per_queue[NB_QUEUES] = {0};
  int balanced = 1;
  int periodic = 1;

  rss_fill_key(key, sizeof(key), 0x6d5a);
  CHECK(key[0] == 0x6d && key[1] == 0x5a && key[38] == 0x6d && key[39] == 0x5a,
        "the key is the pattern repeated, big-endian");
  for (size_t i = 2; i < sizeof(key); i++) {
    periodic = periodic && key[i] == key[i - 2];
  }
  CHECK(periodic, "the key repeats every 2 bytes");

  CHECK(count_asymmetric(key, 4) == 0,
        "IPv4: swapping source and destination keeps the hash");
  CHECK(count_asymmetric(key, 16) == 0,
        "IPv6: swapping source and destination keeps the hash");

  // The same test must fail for a key that does not repeat, or it proves
  // nothing.
  for (size_t i = 0; i < sizeof(other); i++) {
    other[i] = (uint8_t)(i * 37 + 11);
  }
  CHECK(count_asymmetric(other, 4) > NB_FLOWS / 2,
        "a key that does not repeat loses the symmetry (the test can fail)");

  for (int n = 0; n < NB_FLOWS; n++) {
    uint8_t a[4], b[4], sp[2], dp[2], in[12];

    fill_random(a, sizeof(a));
    fill_random(b, sizeof(b));
    fill_random(sp, sizeof(sp));
    fill_random(dp, sizeof(dp));
    make_input(in, a, b, 4, sp, dp);
    per_queue[toeplitz(key, KEY_LEN, in, sizeof(in)) % NB_QUEUES]++;
  }
  for (int q = 0; q < NB_QUEUES; q++) {
    balanced = balanced && per_queue[q] > NB_FLOWS / NB_QUEUES * 8 / 10 &&
               per_queue[q] < NB_FLOWS / NB_QUEUES * 12 / 10;
  }
  CHECK(balanced,
        "random flows spread evenly over 4 queues (within 20% of equal)");

  printf("%s (%d failure(s))\n",
         failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures);
  return failures == 0 ? 0 : 1;
}

/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_SECURE_RANDOM_H
#define PEDIGREE_SECURE_RANDOM_H

#include <stddef.h>
#include <stdint.h>

// Original implementations of RFC 8439 section 2.3 (June 2018), SHA-256 as
// specified in RFC 6234 (May 2011), and HMAC from RFC 2104 (February 1997).
// This header is also used by the standalone persistent-seed bootstrap tool.
namespace pedigree_random {
inline void erase(void* memory, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
  while (length--)
    *bytes++ = 0;
}

namespace detail {
inline uint32_t little32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

inline uint32_t rotateLeft(uint32_t word, unsigned bits) {
  return (word << bits) | (word >> (32 - bits));
}

inline void quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
  a += b;
  d = rotateLeft(d ^ a, 16);
  c += d;
  b = rotateLeft(b ^ c, 12);
  a += b;
  d = rotateLeft(d ^ a, 8);
  c += d;
  b = rotateLeft(b ^ c, 7);
}

class Sha256 {
 public:
  void update(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    m_Length += length;
    while (length--) {
      m_Block[m_Used++] = *bytes++;
      if (m_Used == sizeof(m_Block)) {
        transform();
        m_Used = 0;
      }
    }
  }

  void finish(uint8_t output[32]) {
    const uint64_t bits = m_Length * 8;
    uint8_t padding = 0x80;
    update(&padding, 1);
    padding = 0;
    while (m_Used != 56)
      update(&padding, 1);
    uint8_t length[8];
    for (size_t i = 0; i < sizeof(length); ++i)
      length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(length, sizeof(length));
    for (size_t i = 0; i < 8; ++i)
      for (size_t j = 0; j < 4; ++j)
        output[4 * i + j] = static_cast<uint8_t>(m_State[i] >> (24 - 8 * j));
    erase(length, sizeof(length));
    erase(this, sizeof(*this));
  }

 private:
  void transform() {
    static const uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t schedule[64];
    for (size_t i = 0; i < 16; ++i) {
      schedule[i] = 0;
      for (size_t j = 0; j < 4; ++j)
        schedule[i] = (schedule[i] << 8) | m_Block[i * 4 + j];
    }
    for (size_t i = 16; i < 64; ++i) {
      const uint32_t a = schedule[i - 15], b = schedule[i - 2];
      const uint32_t s0 = rotateLeft(a, 25) ^ rotateLeft(a, 14) ^ (a >> 3);
      const uint32_t s1 = rotateLeft(b, 15) ^ rotateLeft(b, 13) ^ (b >> 10);
      schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    uint32_t work[8];
    for (size_t i = 0; i < 8; ++i)
      work[i] = m_State[i];
    for (size_t i = 0; i < 64; ++i) {
      const uint32_t s1 =
          rotateLeft(work[4], 26) ^ rotateLeft(work[4], 21) ^ rotateLeft(work[4], 7);
      const uint32_t choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
      const uint32_t t1 = work[7] + s1 + choose + constants[i] + schedule[i];
      const uint32_t s0 =
          rotateLeft(work[0], 30) ^ rotateLeft(work[0], 19) ^ rotateLeft(work[0], 10);
      const uint32_t majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
      for (size_t j = 7; j > 0; --j)
        work[j] = work[j - 1];
      work[4] += t1;
      work[0] = t1 + s0 + majority;
    }
    for (size_t i = 0; i < 8; ++i)
      m_State[i] += work[i];
    erase(schedule, sizeof(schedule));
    erase(work, sizeof(work));
  }

  uint32_t m_State[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t m_Block[64] = {};
  size_t m_Used = 0;
  uint64_t m_Length = 0;
};
}  // namespace detail

inline void chacha20_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                           uint8_t output[64]) {
  uint32_t initial[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (size_t i = 0; i < 8; ++i)
    initial[4 + i] = detail::little32(key + i * 4);
  initial[12] = counter;
  for (size_t i = 0; i < 3; ++i)
    initial[13 + i] = detail::little32(nonce + i * 4);
  uint32_t work[16];
  for (size_t i = 0; i < 16; ++i)
    work[i] = initial[i];
  for (size_t round = 0; round < 10; ++round) {
    detail::quarter(work[0], work[4], work[8], work[12]);
    detail::quarter(work[1], work[5], work[9], work[13]);
    detail::quarter(work[2], work[6], work[10], work[14]);
    detail::quarter(work[3], work[7], work[11], work[15]);
    detail::quarter(work[0], work[5], work[10], work[15]);
    detail::quarter(work[1], work[6], work[11], work[12]);
    detail::quarter(work[2], work[7], work[8], work[13]);
    detail::quarter(work[3], work[4], work[9], work[14]);
  }
  for (size_t i = 0; i < 16; ++i) {
    work[i] += initial[i];
    for (size_t j = 0; j < 4; ++j)
      output[4 * i + j] = static_cast<uint8_t>(work[i] >> (8 * j));
  }
  erase(work, sizeof(work));
  erase(initial, sizeof(initial));
}

// Fixed 256-bit keys match the entropy and persistent-seed interfaces.
inline void hmac_sha256(const uint8_t key[32], const void* data, size_t length,
                        uint8_t output[32]) {
  uint8_t pad[64], digest[32];
  for (size_t i = 0; i < sizeof(pad); ++i)
    pad[i] = (i < 32 ? key[i] : 0) ^ 0x36;
  detail::Sha256 inner;
  inner.update(pad, sizeof(pad));
  inner.update(data, length);
  inner.finish(digest);
  for (size_t i = 0; i < sizeof(pad); ++i)
    pad[i] ^= 0x36 ^ 0x5c;
  detail::Sha256 outer;
  outer.update(pad, sizeof(pad));
  outer.update(digest, sizeof(digest));
  outer.finish(output);
  erase(digest, sizeof(digest));
  erase(pad, sizeof(pad));
}
}  // namespace pedigree_random

#endif

/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/utilities/SecureRandom.h"
#include "pedigree/kernel/utilities/lib.h"

namespace {
Spinlock randomLock{false, true};
uint8_t randomKey[32] = {};
bool randomReady = false;

bool initialiseLocked() {
  if (randomReady)
    return true;
  uint8_t seed[32] = {};
  if (hardware_random_bytes(seed, sizeof(seed)) == sizeof(seed)) {
    for (size_t i = 0; i < sizeof(seed); ++i)
      randomKey[i] = seed[i];
    randomReady = true;
  }
  pedigree_random::erase(seed, sizeof(seed));
  return randomReady;
}
}  // namespace

extern "C" int secure_random_seed(const void* buffer, size_t length) {
  if (!buffer || length != sizeof(randomKey))
    return 0;
  LockGuard<Spinlock> guard(randomLock);
  const auto* seed = static_cast<const uint8_t*>(buffer);
  uint8_t next[32];
  if (initialiseLocked()) {
    // Preserve existing secret entropy even if a later trusted source degrades.
    pedigree_random::hmac_sha256(randomKey, seed, length, next);
  } else {
    for (size_t i = 0; i < sizeof(next); ++i)
      next[i] = seed[i];
  }
  for (size_t i = 0; i < sizeof(next); ++i)
    randomKey[i] = next[i];
  randomReady = true;
  pedigree_random::erase(next, sizeof(next));
  return 1;
}

extern "C" size_t secure_random_bytes(void* buffer, size_t length) {
  if (!buffer || !length)
    return 0;
  auto* output = static_cast<uint8_t*>(buffer);
  size_t produced = 0;
  while (produced < length) {
    // One block per critical section bounds IRQ latency for large device reads.
    LockGuard<Spinlock> guard(randomLock);
    if (!initialiseLocked())
      return 0;
    const uint8_t nonce[12] = {};
    uint8_t block[64];
    pedigree_random::chacha20_block(randomKey, nonce, 0, block);
    for (size_t i = 0; i < sizeof(randomKey); ++i)
      randomKey[i] = block[i];
    const size_t count = length - produced < 32 ? length - produced : 32;
    for (size_t i = 0; i < count; ++i)
      output[produced + i] = block[32 + i];
    produced += count;
    // No returned bytes are retained, including the unused tail of a short read.
    pedigree_random::erase(block, sizeof(block));
  }
  return produced;
}

/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/utilities/SecureRandom.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace {
[[noreturn]] void fail(const char* check) {
  std::fprintf(stderr, "SecureRandom fuzz mismatch: %s\n", check);
  std::abort();
}

void compareSha256(const uint8_t* data, size_t size, size_t split) {
  uint8_t actual[32];
  uint8_t expected[32];
  pedigree_random::detail::Sha256 sha;
  sha.update(data, split);
  sha.update(data + split, size - split);
  sha.finish(actual);

  unsigned digestSize = 0;
  if (EVP_Digest(data, size, expected, &digestSize, EVP_sha256(), nullptr) != 1 ||
      digestSize != sizeof(expected) || std::memcmp(actual, expected, sizeof(actual))) {
    fail("SHA-256");
  }
}

void compareHmac(const uint8_t key[32], const uint8_t* data, size_t size) {
  uint8_t actual[32];
  uint8_t expected[EVP_MAX_MD_SIZE];
  unsigned digestSize = 0;
  pedigree_random::hmac_sha256(key, data, size, actual);
  if (!HMAC(EVP_sha256(), key, 32, data, size, expected, &digestSize) ||
      digestSize != sizeof(actual) || std::memcmp(actual, expected, sizeof(actual))) {
    fail("HMAC-SHA-256");
  }
}

void compareChacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
  uint8_t iv[16];
  for (size_t i = 0; i < 4; ++i) {
    iv[i] = static_cast<uint8_t>(counter >> (8 * i));
  }
  std::memcpy(iv + 4, nonce, 12);

  uint8_t actual[64];
  uint8_t expected[64];
  uint8_t zeros[64] = {};
  pedigree_random::chacha20_block(key, nonce, counter, actual);

  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (!context) {
    fail("OpenSSL context");
  }
  int written = 0;
  const bool ok = EVP_EncryptInit_ex(context, EVP_chacha20(), nullptr, key, iv) == 1 &&
                  EVP_EncryptUpdate(context, expected, &written, zeros, sizeof(zeros)) == 1;
  EVP_CIPHER_CTX_free(context);
  if (!ok || written != sizeof(expected) || std::memcmp(actual, expected, sizeof(actual))) {
    fail("ChaCha20 block");
  }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 4096) {
    return 0;
  }

  uint8_t key[32] = {};
  uint8_t nonce[12] = {};
  uint32_t counter = 0;
  const size_t keySize = size < sizeof(key) ? size : sizeof(key);
  std::memcpy(key, data, keySize);
  if (size > sizeof(key)) {
    const size_t nonceSize =
        size - sizeof(key) < sizeof(nonce) ? size - sizeof(key) : sizeof(nonce);
    std::memcpy(nonce, data + sizeof(key), nonceSize);
  }
  for (size_t i = 0; i < 4 && size > sizeof(key) + sizeof(nonce) + i; ++i) {
    counter |= static_cast<uint32_t>(data[sizeof(key) + sizeof(nonce) + i]) << (8 * i);
  }

  const size_t prefix = sizeof(key) + sizeof(nonce) + sizeof(counter);
  const uint8_t* message = size > prefix ? data + prefix : data;
  const size_t messageSize = size > prefix ? size - prefix : size;
  compareSha256(message, messageSize, messageSize / 2);
  compareHmac(key, message, messageSize);
  compareChacha20(key, nonce, counter);
  return 0;
}

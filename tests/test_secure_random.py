"""Exercise the real RNG core with native locking and independent crypto vectors."""

import hashlib
import hmac
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


LOCK_SHIM = r"""
#pragma once
#include <atomic>
#include <cassert>
#include <mutex>
static thread_local bool testInterrupts = true;
class Spinlock {
 public:
  Spinlock(bool locked, bool avoidTracking) { assert(!locked && avoidTracking); }
  void acquire() {
    bool previous = testInterrupts;
    testInterrupts = false;
    mutex.lock();
    priorInterrupts = previous;
  }
  void release() {
    bool previous = priorInterrupts;
    assert(!testInterrupts);
    mutex.unlock();
    testInterrupts = previous;
  }
 private:
  std::mutex mutex;
  bool priorInterrupts = false;
};
"""

HARNESS = r"""
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "SecureRandom.cc"
#include "expected.h"
template class LockGuard<Spinlock>;

static std::atomic<size_t> hardwareCalls{0};
static size_t hardwareLength = 0;
extern "C" size_t hardware_random_bytes(void* buffer, size_t length) {
  assert(!testInterrupts);
  ++hardwareCalls;
  size_t count = std::min(length, hardwareLength);
  for (size_t i = 0; i < count; ++i)
    static_cast<uint8_t*>(buffer)[i] = static_cast<uint8_t>(i);
  return count;
}

static std::string hex(const void* data, size_t length) {
  static const char digits[] = "0123456789abcdef";
  std::string result;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 15];
  }
  return result;
}

static void reset(size_t available = 0) {
  pedigree_random::erase(randomKey, sizeof(randomKey));
  randomReady = false;
  hardwareCalls = 0;
  hardwareLength = available;
  testInterrupts = true;
}

static void primitives() {
  uint8_t key[32], output[64], nonce[12] = {0,0,0,9,0,0,0,0x4a,0,0,0,0};
  for (size_t i = 0; i < 32; ++i)
    key[i] = static_cast<uint8_t>(i);
  pedigree_random::chacha20_block(key, nonce, 1, output);
  assert(hex(output, 64) ==
      "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
      "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");
  uint8_t unalignedKey[33], unalignedNonce[13], guarded[66];
  memcpy(unalignedKey + 1, key, sizeof(key));
  memcpy(unalignedNonce + 1, nonce, sizeof(nonce));
  memset(guarded, 0xa5, sizeof(guarded));
  pedigree_random::chacha20_block(unalignedKey + 1, unalignedNonce + 1, 1, guarded + 1);
  assert(!memcmp(guarded + 1, output, sizeof(output)));
  assert(guarded[0] == 0xa5 && guarded[65] == 0xa5);
  memset(key, 0, sizeof(key));
  memset(nonce, 0, sizeof(nonce));
  pedigree_random::chacha20_block(key, nonce, 0, output);
  assert(hex(output, 64) ==
      "76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
      "da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586");

  // RFC 4231 case 1: appending zero key bytes leaves HMAC's padded key unchanged.
  memset(key, 0x0b, 20);
  pedigree_random::hmac_sha256(key, "Hi There", 8, output);
  assert(hex(output, 32) ==
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  memset(guarded, 0xa5, sizeof(guarded));
  memcpy(unalignedKey + 1, key, sizeof(key));
  pedigree_random::hmac_sha256(unalignedKey + 1, "Hi There", 8, guarded + 1);
  assert(!memcmp(guarded + 1, output, 32));
  assert(guarded[0] == 0xa5 && guarded[33] == 0xa5);
  for (size_t i = 0; i < 32; ++i)
    key[i] = static_cast<uint8_t>(i);
  for (size_t n = 0; n < sizeof(hmacLengths)/sizeof(hmacLengths[0]); ++n) {
    std::vector<uint8_t> message(hmacLengths[n]);
    for (size_t i = 0; i < message.size(); ++i)
      message[i] = static_cast<uint8_t>(i * 73 + 41);
    pedigree_random::hmac_sha256(key, message.data(), message.size(), output);
    assert(hex(output, 32) == hmacExpected[n]);
  }
  static const char kernelDomain[] = "Pedigree kernel seed v1";
  pedigree_random::hmac_sha256(key, kernelDomain, sizeof(kernelDomain) - 1, output);
  assert(hex(output, 32) == kernelSeedExpected);
  static const char savedDomain[] = "Pedigree saved seed v1";
  pedigree_random::hmac_sha256(key, savedDomain, sizeof(savedDomain) - 1, output);
  assert(hex(output, 32) == savedSeedExpected);
  pedigree_random::erase(output, sizeof(output));
  for (uint8_t byte : output)
    assert(byte == 0);
}

static void state() {
  uint8_t storage[99], seed[32];
  for (size_t i = 0; i < sizeof(seed); ++i)
    seed[i] = static_cast<uint8_t>(i);
  reset();
  memset(storage, 0xa5, sizeof(storage));
  assert(secure_random_bytes(nullptr, 32) == 0);
  assert(secure_random_bytes(storage, 0) == 0 && hardwareCalls == 0);
  assert(secure_random_bytes(storage + 1, 97) == 0);
  assert(!randomReady && hardwareCalls == 1 && testInterrupts);
  for (uint8_t byte : storage)
    assert(byte == 0xa5);
  hardwareLength = 16;
  assert(secure_random_bytes(storage, 32) == 0 && !randomReady);
  for (uint8_t byte : storage)
    assert(byte == 0xa5);
  for (uint8_t byte : randomKey)
    assert(byte == 0);
  assert(!secure_random_seed(nullptr, 32));
  assert(!secure_random_seed(seed, 0));
  assert(!secure_random_seed(seed, 31));
  assert(!secure_random_seed(seed, 33));
  hardwareLength = 0;
  assert(secure_random_seed(seed, 32) == 1 && randomReady && testInterrupts);
  size_t calls = hardwareCalls;
  assert(secure_random_bytes(storage + 1, 97) == 97);
  assert(storage[0] == 0xa5 && storage[98] == 0xa5);
  assert(hex(storage + 1, 97) == streamExpected);
  assert(hex(randomKey, 32) == afterFourExpected);
  assert(hardwareCalls == calls && testInterrupts);
  std::array<uint8_t, 32> keyBeforeInvalid;
  memcpy(keyBeforeInvalid.data(), randomKey, 32);
  assert(!secure_random_seed(seed, 31));
  assert(!memcmp(keyBeforeInvalid.data(), randomKey, 32));

  reset();
  assert(secure_random_seed(seed, 32));
  assert(secure_random_bytes(storage + 1, 1) == 1);
  assert(hex(randomKey, 32) == afterOneExpected);
  assert(secure_random_bytes(storage + 2, 31) == 31);
  assert(hex(storage + 1, 32) == partialExpected);
  assert(hex(randomKey, 32) == afterTwoExpected);
  memset(seed, 0, sizeof(seed));
  assert(secure_random_seed(seed, 32));
  assert(hex(randomKey, 32) == reseedExpected);
  assert(secure_random_bytes(storage, 32) == 32);
  assert(hex(storage, 32) == afterReseedOutput);

  reset(32);
  assert(secure_random_bytes(storage, 32) == 32 && hardwareCalls == 1);
  assert(hex(storage, 32) == hardwareExpected);
  assert(secure_random_bytes(storage, 32) == 32 && hardwareCalls == 1);
  reset(32);
  assert(secure_random_seed(seed, 32) && hardwareCalls == 1);
  assert(hex(randomKey, 32) == mixedHardwareExpected);
  testInterrupts = false;
  assert(secure_random_bytes(storage, 32) == 32 && !testInterrupts);
}

static void concurrent() {
  constexpr size_t threads = 8, perThread = 128, total = threads * perThread;
  std::vector<std::array<uint8_t, 32>> sequential(total), parallel(total);
  reset(32);
  for (auto& bytes : sequential)
    assert(secure_random_bytes(bytes.data(), bytes.size()) == bytes.size());
  reset(32);
  std::vector<std::thread> workers;
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      for (size_t i = 0; i < perThread; ++i) {
        auto& bytes = parallel[t * perThread + i];
        assert(secure_random_bytes(bytes.data(), bytes.size()) == bytes.size());
        assert(testInterrupts);
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  assert(hardwareCalls == 1);
  std::sort(sequential.begin(), sequential.end());
  std::sort(parallel.begin(), parallel.end());
  assert(sequential == parallel);
  assert(std::adjacent_find(parallel.begin(), parallel.end()) == parallel.end());

  workers.clear();
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::array<uint8_t, 32> seed = {};
      seed[0] = static_cast<uint8_t>(t);
      for (size_t i = 0; i < perThread; ++i) {
        auto& bytes = parallel[t * perThread + i];
        assert(secure_random_seed(seed.data(), seed.size()));
        assert(secure_random_bytes(bytes.data(), bytes.size()) == bytes.size());
        assert(testInterrupts);
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  std::sort(parallel.begin(), parallel.end());
  assert(std::adjacent_find(parallel.begin(), parallel.end()) == parallel.end());
  assert(hardwareCalls == 1);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  std::string test = argv[1];
  if (test == "primitives") primitives();
  else if (test == "state") state();
  else if (test == "concurrent") concurrent();
  else return 2;
  puts("Secure random contracts passed");
}
"""


def chacha_block(key):
    """Independent word-oriented reference, anchored by published block vectors."""
    initial = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    initial += [int.from_bytes(key[i : i + 4], "little") for i in range(0, 32, 4)]
    initial += [0, 0, 0, 0]
    state = initial.copy()

    def quarter(a, b, c, d):
        for x, y, z, rotation in ((a, b, d, 16), (c, d, b, 12), (a, b, d, 8), (c, d, b, 7)):
            state[x] = (state[x] + state[y]) & 0xFFFFFFFF
            word = state[z] ^ state[x]
            state[z] = ((word << rotation) | (word >> (32 - rotation))) & 0xFFFFFFFF

    for _ in range(10):
        for column in range(4):
            quarter(column, column + 4, column + 8, column + 12)
        for diagonal in range(4):
            quarter(diagonal, 4 + (diagonal + 1) % 4, 8 + (diagonal + 2) % 4, 12 + (diagonal + 3) % 4)
    return b"".join(((a + b) & 0xFFFFFFFF).to_bytes(4, "little") for a, b in zip(state, initial))


def expected_header():
    key = bytes(range(32))
    blocks = []
    for _ in range(4):
        block = chacha_block(key)
        key = block[:32]
        blocks.append((key, block[32:]))
    reseeded = hmac.digest(blocks[1][0], bytes(32), "sha256")
    values = {
        "streamExpected": b"".join(output for _, output in blocks)[:97],
        "afterFourExpected": blocks[3][0],
        "afterOneExpected": blocks[0][0],
        "afterTwoExpected": blocks[1][0],
        "partialExpected": blocks[0][1][:1] + blocks[1][1][:31],
        "reseedExpected": reseeded,
        "afterReseedOutput": chacha_block(reseeded)[32:],
        "hardwareExpected": blocks[0][1],
        "mixedHardwareExpected": hmac.digest(bytes(range(32)), bytes(32), "sha256"),
        "kernelSeedExpected": hmac.digest(bytes(range(32)), b"Pedigree kernel seed v1", "sha256"),
        "savedSeedExpected": hmac.digest(bytes(range(32)), b"Pedigree saved seed v1", "sha256"),
    }
    declarations = [f'const char* {name} = "{value.hex()}";' for name, value in values.items()]
    lengths = [0, 1, 31, 32, 55, 56, 63, 64, 65, 127, 128, 129, 4096]
    declarations.append("const size_t hmacLengths[] = {" + ",".join(map(str, lengths)) + "};")
    results = []
    for length in lengths:
        message = bytes((i * 73 + 41) & 255 for i in range(length))
        results.append('"' + hmac.new(bytes(range(32)), message, hashlib.sha256).hexdigest() + '"')
    declarations.append("const char* hmacExpected[] = {" + ",".join(results) + "};")
    return "\n".join(declarations)


class SecureRandomTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="pedigree-secure-random-")
        temporary = Path(cls.directory.name)
        repository = Path(__file__).resolve().parents[1]
        kernel = temporary / "pedigree/kernel"
        (kernel / "utilities").mkdir(parents=True)
        (kernel / "Spinlock.h").write_text(LOCK_SHIM)
        (kernel / "utilities/lib.h").write_text(
            '#pragma once\n#include <stddef.h>\nextern "C" size_t hardware_random_bytes(void*, size_t);\n'
        )
        (temporary / "config.h").write_text("#define THREADS 0\n")
        (temporary / "expected.h").write_text(expected_header())
        harness = temporary / "contracts.cc"
        harness.write_text(HARNESS)
        cls.binary = temporary / "contracts"
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        subprocess.run(
            [
                *compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", f"-I{temporary}",
                f"-I{repository / 'src/system/include'}",
                f"-I{repository / 'src/system/kernel/core/lib'}",
                str(harness), "-o", str(cls.binary),
            ], check=True, text=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_contract(self, name):
        result = subprocess.run(
            [str(self.binary), name], capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "Secure random contracts passed")

    def test_published_and_independent_crypto_vectors(self):
        self.run_contract("primitives")

    def test_seeding_rekeying_short_reads_and_failure(self):
        self.run_contract("state")

    def test_concurrent_reads_and_reseeding(self):
        self.run_contract("concurrent")


if __name__ == "__main__":
    unittest.main()

/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/LazyEvaluate.h"
#include "pedigree/kernel/utilities/Pair.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/UniqueResource.h"
#include "pedigree/kernel/utilities/demangle.h"
#include "pedigree/kernel/utilities/md5/md5.h"
#include "pedigree/kernel/utilities/sha1/sha1.h"
#include "pedigree/kernel/utilities/smhasher/MurmurHash3.h"
#include "pedigree/kernel/utilities/spooky/SpookyV2.h"
#include "pedigree/kernel/utilities/utility.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

uint8_t inputByte(const uint8_t* data, size_t size, size_t index) {
  return index < size ? data[index] : static_cast<uint8_t>(index * 29 + 11);
}

struct Resource {
  size_t* released;
};

struct Releaser {
  static void release(Resource* resource) {
    ++*resource->released;
  }
};

using Owner = UniqueResource<Resource, Releaser>;

struct Tracked {
  Tracked(size_t* destroyed, uint8_t id) : destroyed(destroyed), id(id) {}
  ~Tracked() {
    ++destroyed[id];
  }

  size_t* destroyed;
  uint8_t id;
};

size_t lazyCreated;
size_t lazyDestroyed;

int* createInt(const int& metadata) {
  ++lazyCreated;
  return new int(metadata);
}

void destroyInt(int* value) {
  ++lazyDestroyed;
  delete value;
}

void checkValues(const uint8_t* data, size_t size) {
  const int left = inputByte(data, size, 0);
  const int right = inputByte(data, size, 1);
  const int other = inputByte(data, size, 2);

  const auto value = Result<int, int>::withValue(left);
  const auto error = Result<int, int>::withError(right);
  require(value.hasValue() && !value.hasError() && value.value() == left);
  require(error.hasError() && !error.hasValue() && error.error() == right);

  const Pair<int, int> a(left, right);
  const Pair<int, int> b(left, other);
  const auto expectedA = std::pair(left, right);
  const auto expectedB = std::pair(left, other);
  require((a == b) == (expectedA == expectedB));
  require((a != b) == (expectedA != expectedB));
  require((a < b) == (expectedA < expectedB));
  require((a <= b) == (expectedA <= expectedB));
  require((a > b) == (expectedA > expectedB));
  require((a >= b) == (expectedA >= expectedB));

  const uint32_t word = uint32_t(left) << 24 | uint32_t(right) << 16 | uint32_t(other) << 8 |
                        inputByte(data, size, 3);
  require(BS32(BS32(word)) == word);
  require(HOST_TO_BIG32(BIG_TO_HOST32(word)) == word);
  require(HOST_TO_LITTLE32(LITTLE_TO_HOST32(word)) == word);
}

void checkOwners(const uint8_t* data, size_t size) {
  std::array<size_t, 2> released{};
  Resource first{&released[0]};
  Resource second{&released[1]};
  {
    Owner owner = Owner::adopt(&first);
    Owner other = Owner::adopt(&second);
    if (inputByte(data, size, 4) & 1) {
      owner = std::move(other);
      require(released[0] == 1 && released[1] == 0);
    } else {
      Resource* transferred = owner.release();
      require(transferred == &first && !owner);
      owner.reset(transferred);
    }
    owner.reset(owner.get());
  }
  require(released[0] == 1 && released[1] == 1);

  size_t destroyed[2]{};
  {
    auto pointer = UniquePointer<Tracked>::adopt(new Tracked(destroyed, 0));
    auto moved = std::move(pointer);
    require(!pointer.get() && moved.get());
    Tracked* raw = moved.releaseOwnership();
    require(!moved.get() && destroyed[0] == 0);
    auto adopted = UniquePointer<Tracked>::adopt(raw);
    adopted.reset();
    require(destroyed[0] == 1);

    auto array = UniqueArray<uint8_t>::allocate(32);
    array.get()[inputByte(data, size, 5) % 32] = inputByte(data, size, 6);
    auto movedArray = std::move(array);
    require(!array.get() && movedArray.get());

    Tracked* guarded = new Tracked(destroyed, 1);
    {
      PointerGuard<Tracked> guard(&guarded);
      require(guarded != nullptr);
    }
    require(!guarded && destroyed[1] == 1);
  }

  std::array<SharedPointer<Tracked>, 4> pointers;
  std::array<size_t, 32> sharedDestroyed{};
  size_t created = 0;
  for (size_t step = 0; step < 32; ++step) {
    const uint8_t command = inputByte(data, size, 7 + step * 3);
    const size_t target = inputByte(data, size, 8 + step * 3) % pointers.size();
    const size_t source = inputByte(data, size, 9 + step * 3) % pointers.size();
    switch (command & 3) {
      case 0:
        pointers[target] = SharedPointer<Tracked>::tryAdopt(
            new Tracked(sharedDestroyed.data(), static_cast<uint8_t>(created++)));
        break;
      case 1:
        pointers[target] = pointers[source];
        break;
      case 2:
        pointers[target] = std::move(pointers[source]);
        break;
      case 3:
        pointers[target].reset();
        break;
    }

    for (const auto& pointer : pointers) {
      if (!pointer) {
        require(pointer.refcount() == 0);
        continue;
      }
      size_t references = 0;
      for (const auto& other : pointers) {
        references += other.get() == pointer.get();
      }
      require(pointer.refcount() == references);
      require(pointer.unique() == (references == 1));
    }
  }
  for (auto& pointer : pointers) {
    pointer.reset();
  }
  for (size_t index = 0; index < created; ++index) {
    require(sharedDestroyed[index] == 1);
  }

  lazyCreated = lazyDestroyed = 0;
  {
    const int metadata = inputByte(data, size, 102);
    LazyEvaluate<int, int, createInt, destroyInt> value{metadata};
    if (!value) {
      std::abort();
    }
    require(!value.active());
    for (size_t step = 0; step < 16; ++step) {
      if (inputByte(data, size, 103 + step) & 1) {
        require(value.get() && *value.get() == metadata && value.active());
      } else {
        value.reset();
        require(!value.active());
      }
      require(lazyCreated - lazyDestroyed == size_t(value.active()));
    }
  }
  require(lazyCreated == lazyDestroyed);
}

void checkHashes(const uint8_t* data, size_t size) {
  const size_t split = size ? inputByte(data, size, 0) % (size + 1) : 0;
  const uint32_t seed = uint32_t(inputByte(data, size, 1)) << 24 |
                        uint32_t(inputByte(data, size, 2)) << 16 |
                        uint32_t(inputByte(data, size, 3)) << 8 | inputByte(data, size, 4);

  SHA1 wholeSha;
  SHA1 splitSha;
  wholeSha.Input(data, static_cast<unsigned>(size));
  splitSha.Input(data, static_cast<unsigned>(split));
  splitSha.Input(data + split, static_cast<unsigned>(size - split));
  unsigned wholeDigest[5]{};
  unsigned splitDigest[5]{};
  require(wholeSha.Result(wholeDigest) && splitSha.Result(splitDigest));
  require(std::memcmp(wholeDigest, splitDigest, sizeof(wholeDigest)) == 0);

  MD5 wholeMd5;
  MD5 splitMd5;
  wholeMd5.Input(const_cast<uint8_t*>(data), static_cast<unsigned>(size));
  splitMd5.Input(const_cast<uint8_t*>(data), static_cast<unsigned>(split));
  splitMd5.Input(const_cast<uint8_t*>(data + split), static_cast<unsigned>(size - split));
  uint8_t wholeMd5Digest[16]{};
  uint8_t splitMd5Digest[16]{};
  wholeMd5.Result(wholeMd5Digest);
  splitMd5.Result(splitMd5Digest);
  require(std::memcmp(wholeMd5Digest, splitMd5Digest, sizeof(wholeMd5Digest)) == 0);

  uint64_t wholeH1 = seed, wholeH2 = ~uint64_t(seed);
  uint64_t splitH1 = wholeH1, splitH2 = wholeH2;
  SpookyHash::Hash128(data, size, &wholeH1, &wholeH2);
  SpookyHash streaming;
  streaming.Init(splitH1, splitH2);
  streaming.Update(data, split);
  streaming.Update(data + split, size - split);
  streaming.Final(&splitH1, &splitH2);
  require(wholeH1 == splitH1 && wholeH2 == splitH2);

  uint32_t murmur32[2]{};
  std::array<uint8_t, 16> murmurX86A{}, murmurX86B{}, murmurX64A{}, murmurX64B{};
  MurmurHash3_x86_32(data, static_cast<int>(size), seed, &murmur32[0]);
  MurmurHash3_x86_32(data, static_cast<int>(size), seed, &murmur32[1]);
  require(murmur32[0] == murmur32[1]);
  MurmurHash3_x86_128(data, static_cast<int>(size), seed, murmurX86A.data());
  MurmurHash3_x86_128(data, static_cast<int>(size), seed, murmurX86B.data());
  MurmurHash3_x64_128(data, static_cast<int>(size), seed, murmurX64A.data());
  MurmurHash3_x64_128(data, static_cast<int>(size), seed, murmurX64B.data());
  require(murmurX86A == murmurX86B && murmurX64A == murmurX64B);
}

void checkDemangle(const uint8_t* data, size_t size) {
  constexpr char Alphabet[] = "_ZNIEPKStvij0123456789abcdefghijklmnopqrstuvwxyz";
  char encoded[64] = "_Z";
  const size_t length = std::min(size, sizeof(encoded) - 3);
  for (size_t index = 0; index < length; ++index) {
    encoded[index + 2] = Alphabet[data[index] % (sizeof(Alphabet) - 1)];
  }
  encoded[length + 2] = '\0';

  LargeStaticString source(encoded);
  LargeStaticString first;
  LargeStaticString second;
  demangle_full(source, first);
  demangle_full(source, second);
  require(first == second);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 512) {
    return 0;
  }
  checkValues(data, size);
  checkOwners(data, size);
  checkHashes(data, size);
  checkDemangle(data, size);
  return 0;
}

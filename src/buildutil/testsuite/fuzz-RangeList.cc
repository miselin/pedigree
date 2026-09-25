/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/RangeList.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace {
constexpr size_t Domain = 128;
constexpr size_t MaxSpan = 32;
constexpr size_t MinOperations = 32;
constexpr size_t MaxOperations = 128;

using FreeBytes = std::array<uint8_t, Domain>;
using Ranges = RangeList<uint64_t>;

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

uint8_t inputByte(const uint8_t* data, size_t size, size_t index, uint8_t fallback) {
  return index < size ? data[index] : fallback;
}

bool allFree(const FreeBytes& freeBytes, size_t address, size_t length) {
  for (size_t i = address; i < address + length; ++i) {
    if (!freeBytes[i]) {
      return false;
    }
  }
  return true;
}

bool hasFreeRun(const FreeBytes& freeBytes, size_t length) {
  for (size_t address = 0; address + length <= Domain; ++address) {
    if (allFree(freeBytes, address, length)) {
      return true;
    }
  }
  return false;
}

size_t findByte(const FreeBytes& freeBytes, size_t start, bool wantedFree) {
  for (size_t offset = 0; offset < Domain; ++offset) {
    const size_t address = (start + offset) % Domain;
    if (static_cast<bool>(freeBytes[address]) == wantedFree) {
      return address;
    }
  }
  return Domain;
}

void setBytes(FreeBytes& freeBytes, size_t address, size_t length, uint8_t value) {
  std::fill(freeBytes.begin() + address, freeBytes.begin() + address + length, value);
}

void checkRanges(const Ranges& ranges, const FreeBytes& freeBytes) {
  FreeBytes actual{};
  for (size_t index = 0; index < ranges.size(); ++index) {
    Ranges::Range range;
    require(ranges.getRange(index, range));
    require(range.address < Domain && range.length && range.length <= Domain - range.address);
    for (size_t address = range.address; address < range.address + range.length; ++address) {
      require(!actual[address]);
      actual[address] = 1;
    }
  }
  require(actual == freeBytes);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::array<Ranges, 2> ranges{Ranges(false), Ranges(true)};
  std::array<FreeBytes, 2> freeBytes{};
  for (size_t i = 0; i < ranges.size(); ++i) {
    ranges[i].free(0, Domain);
    freeBytes[i].fill(1);
  }

  const size_t inputOperations = size / 4 + (size % 4 != 0);
  const size_t operations = std::max(MinOperations, std::min(inputOperations, MaxOperations));
  for (size_t step = 0; step < operations; ++step) {
    const size_t offset = step * 4;
    const uint8_t operation = inputByte(data, size, offset, step) & 7;
    const uint8_t selector = inputByte(data, size, offset + 1, step / 8);
    const uint8_t position = inputByte(data, size, offset + 2, step * 29 + 17);
    const uint8_t amount = inputByte(data, size, offset + 3, step * 17 + 5);
    const size_t target = selector & 1;
    const size_t other = target ^ 1;
    const size_t start = position % Domain;
    size_t length = 1 + amount % MaxSpan;
    length = std::min(length, Domain - start);

    switch (operation) {
      case 0:
      case 1: {
        const bool expected = hasFreeRun(freeBytes[target], length);
        uint64_t address = UINT64_MAX;
        const bool allocated = ranges[target].allocate(length, address);
        require(allocated == expected);
        if (allocated) {
          require(address < Domain && length <= Domain - address);
          require(allFree(freeBytes[target], address, length));
          setBytes(freeBytes[target], address, length, 0);
        }
        break;
      }
      case 2: {
        const bool expected = allFree(freeBytes[target], start, length);
        require(ranges[target].allocateSpecific(start, length) == expected);
        if (expected) {
          setBytes(freeBytes[target], start, length, 0);
        }
        break;
      }
      case 3: {
        const size_t address = findByte(freeBytes[target], start, false);
        if (address == Domain) {
          break;
        }
        size_t available = 0;
        while (address + available < Domain && available < MaxSpan &&
               !freeBytes[target][address + available]) {
          ++available;
        }
        const size_t released = 1 + amount % available;
        ranges[target].free(address, released, selector & 2);
        setBytes(freeBytes[target], address, released, 1);
        break;
      }
      case 4: {
        Ranges copy(ranges[target]);
        FreeBytes copiedBytes = freeBytes[target];
        checkRanges(copy, copiedBytes);
        const size_t address = findByte(copiedBytes, start, true);
        if (address != Domain) {
          require(copy.allocateSpecific(address, 1));
          copiedBytes[address] = 0;
        }
        checkRanges(copy, copiedBytes);
        checkRanges(ranges[target], freeBytes[target]);
        ranges[other] = copy;
        freeBytes[other] = copiedBytes;
        break;
      }
      case 5:
        ranges[0].swap(ranges[1]);
        std::swap(freeBytes[0], freeBytes[1]);
        break;
      case 6:
        ranges[target].clear();
        freeBytes[target].fill(0);
        break;
      case 7:
        ranges[target].sweep();
        break;
    }

    checkRanges(ranges[0], freeBytes[0]);
    checkRanges(ranges[1], freeBytes[1]);
  }

  return 0;
}

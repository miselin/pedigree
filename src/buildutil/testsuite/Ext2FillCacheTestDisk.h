/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef EXT2_FILL_CACHE_TEST_DISK_H
#define EXT2_FILL_CACHE_TEST_DISK_H

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

class FillCacheDisk final : public Disk {
 public:
  static constexpr size_t kPageSize = TargetInfo::getPageSize();

  FillCacheDisk()
      : storage(512 * 1024, 0),
        persisted(storage.size(), 0),
        pageReferences(storage.size() / kPageSize, 0) {}

  BufferView read(uint64_t location) override {
    if (location >= storage.size()) {
      outOfRange = true;
      return BufferView();
    }

    ++pageReferences[location / kPageSize];
    reads.push_back(location);
    operations.push_back('R');
    const size_t pageOffset = location % kPageSize;
    return BufferView(storage.data() + location, kPageSize - pageOffset);
  }

  void write(uint64_t location) override {
    writes.push_back(location);
    writePins.push_back(location < storage.size() && pageReferences[location / kPageSize]);
    operations.push_back('W');

    const uint64_t page = location & ~(static_cast<uint64_t>(kPageSize) - 1);
    if (std::find(scheduledPages.begin(), scheduledPages.end(), page) == scheduledPages.end()) {
      scheduledPages.push_back(page);
      std::copy(storage.begin() + page, storage.begin() + page + kPageSize,
                persisted.begin() + page);
    }
  }

  void align(uint64_t) override {}

  void flush(uint64_t location) override {
    flushes.push_back(location);
    operations.push_back('F');
    const uint64_t page = location & ~(static_cast<uint64_t>(kPageSize) - 1);
    std::copy(storage.begin() + page, storage.begin() + page + kPageSize, persisted.begin() + page);
  }

  size_t getSize() const override {
    return storage.size();
  }

  size_t getBlockSize() const override {
    return 512;
  }

  bool pin(uint64_t location) override {
    if (location >= storage.size()) {
      outOfRange = true;
      return false;
    }
    ++pageReferences[location / kPageSize];
    return true;
  }

  void unpin(uint64_t location) override {
    if (location >= storage.size() || !pageReferences[location / kPageSize]) {
      unbalancedUnpin = true;
      return;
    }
    --pageReferences[location / kPageSize];
    operations.push_back('U');
  }

  void fill(uint64_t location, size_t length, uint8_t value) {
    if (location + length > storage.size()) {
      outOfRange = true;
      return;
    }
    std::fill(storage.begin() + location, storage.begin() + location + length, value);
    std::fill(persisted.begin() + location, persisted.begin() + location + length, value);
  }

  void store32(uint64_t location, uint32_t value) {
    if (location + sizeof(value) > storage.size()) {
      outOfRange = true;
      return;
    }
    std::memcpy(storage.data() + location, &value, sizeof(value));
    std::memcpy(persisted.data() + location, &value, sizeof(value));
  }

  bool equals(uint64_t location, const uint8_t* expected, size_t length) const {
    if (location + length > storage.size()) {
      return false;
    }
    return std::equal(expected, expected + length, storage.begin() + location);
  }

  bool persistedEquals(uint64_t location, const uint8_t* expected, size_t length) const {
    if (location + length > persisted.size()) {
      return false;
    }
    return std::equal(expected, expected + length, persisted.begin() + location);
  }

  bool hasNoPins() const {
    return std::all_of(pageReferences.begin(), pageReferences.end(),
                       [](size_t count) { return count == 0; });
  }

  void clearActivity() {
    reads.clear();
    writes.clear();
    flushes.clear();
    writePins.clear();
    operations.clear();
    scheduledPages.clear();
  }

  std::vector<uint8_t> storage;
  std::vector<uint8_t> persisted;
  std::vector<size_t> pageReferences;
  std::vector<uint64_t> reads;
  std::vector<uint64_t> writes;
  std::vector<uint64_t> flushes;
  std::vector<bool> writePins;
  std::vector<char> operations;
  std::vector<uint64_t> scheduledPages;
  bool outOfRange = false;
  bool unbalancedUnpin = false;
};

#endif

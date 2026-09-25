/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/utilities/RadixTree.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace {
using Model = std::map<std::string, int>;

[[noreturn]] void fail(size_t operation, const char* check) {
  std::fprintf(stderr, "RadixTree fuzz mismatch at operation %zu: %s\n", operation, check);
  std::abort();
}

void verify(const RadixTree<int>& tree, const Model& model, const std::string& key,
            size_t operation) {
  if (tree.count() != model.size()) {
    fail(operation, "count");
  }

  int value = -1;
  const auto expected = model.find(key);
  const bool found = tree.lookup(String(key.c_str()), value);
  if (found != (expected != model.end()) ||
      value != (expected == model.end() ? 0 : expected->second)) {
    fail(operation, "operation key");
  }

  std::multiset<int> expectedValues;
  for (const auto& entry : model) {
    value = -1;
    if (!tree.lookup(String(entry.first.c_str()), value) || value != entry.second) {
      fail(operation, "stored key");
    }
    expectedValues.insert(entry.second);
  }

  std::multiset<int> actualValues;
  for (auto it = tree.begin(); it != tree.end(); ++it) {
    if (actualValues.size() >= model.size()) {
      fail(operation, "iteration length");
    }
    actualValues.insert(*it);
  }
  if (actualValues != expectedValues) {
    fail(operation, "iteration values");
  }
}

std::string makeKey(uint8_t base, uint8_t suffix) {
  constexpr const char* Keys[] = {"",       "a",     "ab",  "abc",    "abcd", "abd",
                                  "b",      "ba",    "baa", "bb",     "foo",  "foobar",
                                  "foobaz", "fooba", "bar", "barfoo", "x",    "xyz"};
  std::string key(Keys[base % (sizeof(Keys) / sizeof(Keys[0]))]);
  if (suffix & 0x10) {
    key += static_cast<char>('a' + (suffix & 7));
  }
  if (suffix & 0x20) {
    key += static_cast<char>('a' + ((suffix >> 3) & 7));
  }
  return key;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 1024) {
    return 0;
  }

  RadixTree<int> tree;
  Model model;
  for (size_t offset = 0; offset + 3 < size; offset += 4) {
    const std::string key = makeKey(data[offset + 1], data[offset + 2]);
    const int value = data[offset + 3];
    const size_t operation = offset / 4;
    switch (data[offset] & 15) {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
        tree.insert(String(key.c_str()), value);
        model[key] = value;
        break;
      case 8:
      case 9:
      case 10:
        tree.remove(String(key.c_str()));
        model.erase(key);
        break;
      case 11:
        break;
      case 12: {
        RadixTree<int> copy(tree);
        verify(copy, model, key, operation);
        copy.insert(String(key.c_str()), value);
        break;
      }
      case 13: {
        RadixTree<int> copy;
        copy.insert(String("stale"), 123);
        copy = tree;
        verify(copy, model, key, operation);
        break;
      }
      case 14: {
        const RadixTree<int>& same = tree;
        tree = same;
        break;
      }
      case 15:
        tree.clear();
        model.clear();
        break;
    }
    verify(tree, model, key, operation);
  }
  return 0;
}

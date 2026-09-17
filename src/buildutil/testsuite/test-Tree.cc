/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/Tree.h"

#include <gtest/gtest.h>

TEST(PedigreeTree, Construction) {
  Tree<int, int> x;
  EXPECT_EQ(x.count(), (size_t)0);
  EXPECT_EQ(x.begin(), x.end());
}

TEST(PedigreeTree, CopyConstruction) {
  Tree<int, int> x;
  x.insert(1, 1);

  Tree<int, int> y(x);
  EXPECT_EQ(x.count(), y.count());

  EXPECT_EQ(x.lookup(1), y.lookup(1));
}

TEST(PedigreeTree, Insertion) {
  Tree<int, int> x;
  for (int i = 1; i < 9; ++i) {
    x.insert(i, i);
  }

  for (int i = 1; i < 9; ++i) {
    EXPECT_EQ(x.lookup(i), i);
  }
}

TEST(PedigreeTree, Clear) {
  Tree<int, int> x;
  // try and rebalance the tree a bit
  for (int i = 0; i < 256; ++i) {
    x.insert(i, 1234);
  }
  for (int i = 512; i < 768; ++i) {
    x.insert(i, 1234);
  }
  for (int i = 256; i < 512; ++i) {
    x.insert(i, 1234);
  }
  for (int i = 768; i < 1024; ++i) {
    x.insert(i, 1234);
  }
  EXPECT_EQ(x.count(), (size_t)1024);
  x.clear();
  EXPECT_EQ(x.count(), (size_t)0);
}

TEST(PedigreeTree, Removal) {
  Tree<int, int> x;
  x.insert(1, 1);
  x.insert(2, 2);
  x.remove(1);
  EXPECT_EQ(x.count(), (size_t)1);

  EXPECT_EQ(x.lookup(2), 2);
}

TEST(PedigreeTree, RootRemoval) {
  Tree<int, int> x;
  x.insert(1, 1);
  x.insert(2, 2);  // should become the root of the tree
  x.insert(3, 3);
  x.remove(2);
  EXPECT_EQ(x.count(), (size_t)2);

  EXPECT_EQ(x.lookup(1), 1);
  EXPECT_EQ(x.lookup(2), 0);
  EXPECT_EQ(x.lookup(3), 3);
}

TEST(PedigreeTree, LeftRemoval) {
  Tree<int, int> x;
  x.insert(1, 1);
  x.insert(2, 2);  // should become the root of the tree
  x.insert(3, 3);
  x.remove(1);
  EXPECT_EQ(x.count(), (size_t)2);

  EXPECT_EQ(x.lookup(1), 0);
  EXPECT_EQ(x.lookup(2), 2);
  EXPECT_EQ(x.lookup(3), 3);
}

TEST(PedigreeTree, RightRemoval) {
  Tree<int, int> x;
  x.insert(1, 1);
  x.insert(2, 2);  // should become the root of the tree
  x.insert(3, 3);
  x.remove(3);
  EXPECT_EQ(x.count(), (size_t)2);

  EXPECT_EQ(x.lookup(1), 1);
  EXPECT_EQ(x.lookup(2), 2);
  EXPECT_EQ(x.lookup(3), 0);
}

TEST(PedigreeTree, DoubleInsertionValueChanged) {
  Tree<int, int> x;
  x.insert(1, 1);
  x.insert(1, 2);
  EXPECT_EQ(x.lookup(1), 2);
  EXPECT_EQ(x.count(), 1U);
}

TEST(PedigreeTree, SelfAssignmentPreservesItems) {
  Tree<int, int> x;
  x.insert(1, 10);
  x.insert(2, 20);

  const Tree<int, int>& same = x;
  x = same;
  EXPECT_EQ(x.count(), 2U);
  EXPECT_EQ(x.lookup(1), 10);
  EXPECT_EQ(x.lookup(2), 20);
}

TEST(PedigreeTree, SortedInsertion) {
  Tree<int, int> x;
  size_t insertions = 0;
  for (int i = 1; i < 1024; ++i) {
    x.insert(i, i);
    ++insertions;
  }

  EXPECT_EQ(x.count(), insertions);
  for (int i = 1; i < 1024; ++i) {
    EXPECT_EQ(x.lookup(i), i);
  }
}

TEST(PedigreeTree, ReverseSortedInsertion) {
  Tree<int, int> x;
  size_t insertions = 0;
  for (int i = 1023; i > 0; --i) {
    x.insert(i, i);
    ++insertions;
  }

  EXPECT_EQ(x.count(), insertions);
  for (int i = 1; i < 1024; ++i) {
    EXPECT_EQ(x.lookup(i), i);
  }
}

TEST(PedigreeTree, Iteration) {
  Tree<int, int> x;
  x.insert(1, 1);
  x.insert(2, 2);
  x.insert(3, 3);
  x.insert(4, 4);
  x.insert(5, 5);

  auto it = x.begin();
  EXPECT_EQ(it.value(), 1);
  ++it;
  EXPECT_EQ(it.value(), 2);
  ++it;
  EXPECT_EQ(it.value(), 3);
  ++it;
  EXPECT_EQ(it.value(), 4);
  ++it;
  EXPECT_EQ(it.value(), 5);
  ++it;
  EXPECT_EQ(it, x.end());
}

TEST(PedigreeTree, ConstIteration) {
  Tree<int, int> x;
  x.insert(1, 10);
  x.insert(2, 20);

  const Tree<int, int>& readOnly = x;
  auto it = readOnly.begin();
  EXPECT_EQ(it.key(), 1);
  EXPECT_EQ(it.value(), 10);
  ++it;
  EXPECT_EQ(it.key(), 2);
  EXPECT_EQ(it.value(), 20);
  ++it;
  EXPECT_EQ(it, readOnly.end());
}

TEST(PedigreeTree, LowerBoundEmptyPreservesOutputs) {
  const Tree<int, int> x;
  int key = 91;
  int value = 92;
  EXPECT_FALSE(x.lowerBound(0, key, value));
  EXPECT_EQ(key, 91);
  EXPECT_EQ(value, 92);
}

TEST(PedigreeTree, LowerBoundOrdering) {
  Tree<int, int> x;
  const int inserted[] = {40, 10, 70, 20, 60, 30, 50};
  for (int key : inserted) {
    x.insert(key, key == 40 ? 0 : key + 100);
  }
  const Tree<int, int>& readOnly = x;
  const int queries[] = {-1, 10, 11, 20, 35, 40, 69, 70};
  const int expected[] = {10, 10, 20, 20, 40, 40, 70, 70};
  for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
    int key = -1;
    int value = -1;
    ASSERT_TRUE(readOnly.lowerBound(queries[i], key, value));
    EXPECT_EQ(key, expected[i]);
    EXPECT_EQ(value, expected[i] == 40 ? 0 : expected[i] + 100);
  }

  int key = 91;
  int value = 92;
  EXPECT_FALSE(readOnly.lowerBound(71, key, value));
  EXPECT_EQ(key, 91);
  EXPECT_EQ(value, 92);
}

TEST(PedigreeTree, LowerBoundPreservesKeyRequirements) {
  struct Key {
    int number;
    bool operator==(const Key& other) const {
      return number == other.number;
    }
    bool operator>(const Key& other) const {
      return number > other.number;
    }
  };
  Tree<Key, int> x;
  x.insert(Key{3}, 30);
  x.insert(Key{1}, 10);
  x.insert(Key{5}, 50);
  Key key{91};
  int value = 92;
  ASSERT_TRUE(x.lowerBound(Key{2}, key, value));
  EXPECT_EQ(key.number, 3);
  EXPECT_EQ(value, 30);
}

TEST(PedigreeTree, LowerBoundCursorSurvivesRemoval) {
  Tree<int, int> x;
  for (int i = 0; i < 64; ++i) {
    const int index = i * 37 % 64;
    x.insert(index * 4096, index + 100);
  }

  int cursor = 0;
  int key = -1;
  int value = -1;
  int visited = 0;
  size_t retained = 0;
  while (x.lowerBound(cursor, key, value)) {
    ASSERT_LT(visited, 64);
    ASSERT_EQ(key, visited * 4096);
    EXPECT_EQ(value, visited + 100);
    cursor = key + 1;
    if (visited < 16 || visited % 3 == 0) {
      ++retained;
    } else {
      x.remove(key);
    }
    ++visited;
  }
  EXPECT_EQ(visited, 64);
  EXPECT_EQ(x.count(), retained);
  for (int i = 0; i < 64; ++i) {
    EXPECT_EQ(x.contains(i * 4096), i < 16 || i % 3 == 0);
  }

  cursor = 0;
  size_t removed = 0;
  while (x.lowerBound(cursor, key, value)) {
    ASSERT_LT(removed, retained);
    cursor = key + 1;
    x.remove(key);
    EXPECT_EQ(value, key / 4096 + 100);
    ++removed;
  }
  EXPECT_EQ(removed, retained);
  EXPECT_EQ(x.count(), 0U);
}

TEST(PedigreeTree, FloorBoundOrderingAndAbsentOutputs) {
  Tree<int, int> tree;
  int key = 91, value = 92;
  EXPECT_FALSE(tree.floorBound(0, key, value));
  EXPECT_EQ(key, 91);
  EXPECT_EQ(value, 92);
  for (int inserted : {40, 10, 70, 20, 60, 30, 50})
    tree.insert(inserted, inserted == 40 ? 0 : inserted + 100);
  const int queries[] = {10, 11, 20, 35, 40, 69, 70, 100};
  const int expected[] = {10, 10, 20, 30, 40, 60, 70, 70};
  for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
    ASSERT_TRUE(tree.floorBound(queries[i], key, value));
    EXPECT_EQ(key, expected[i]);
    EXPECT_EQ(value, expected[i] == 40 ? 0 : expected[i] + 100);
  }
  key = 91;
  value = 92;
  EXPECT_FALSE(tree.floorBound(9, key, value));
  EXPECT_EQ(key, 91);
  EXPECT_EQ(value, 92);
  for (int expectedKey = 70; expectedKey >= 10; expectedKey -= 10) {
    ASSERT_TRUE(tree.floorBound(100, key, value));
    EXPECT_EQ(key, expectedKey);
    tree.remove(key);
  }
  EXPECT_FALSE(tree.floorBound(100, key, value));
}

TEST(PedigreeTree, FloorBoundComparisonCost) {
  struct Key {
    int number;
    size_t* comparisons;
    bool operator==(const Key& other) const {
      ++*comparisons;
      return number == other.number;
    }
    bool operator>(const Key& other) const {
      ++*comparisons;
      return number > other.number;
    }
  };
  size_t comparisons = 0;
  Tree<Key, int> tree;
  for (int i = 0; i < 4096; ++i)
    tree.insert(Key{i * 2, &comparisons}, i);
  for (int i : {0, 1, 511, 4095}) {
    Key key{-1, &comparisons};
    int value = -1;
    comparisons = 0;
    ASSERT_TRUE(tree.floorBound(Key{i * 2 + 1, &comparisons}, key, value));
    EXPECT_EQ(key.number, i * 2);
    EXPECT_EQ(value, i);
    EXPECT_LE(comparisons, 52U);
  }
}

TEST(PedigreeTree, InsertMove) {
  Tree<int, SharedPointer<int>> x;
  auto y = SharedPointer<int>::allocate();
  auto ptr = y.get();

  x.insert(1, pedigree_std::move(y));

  EXPECT_EQ(x.lookup(1).get(), ptr);
  EXPECT_EQ(x.lookupRef(1).get(), ptr);
  EXPECT_EQ(y.get(), nullptr);
}

TEST(PedigreeTree, FallibleInsertionBalancesAndReplaces) {
  Tree<int, int> tree;
  // Exercise both rotations while using the same path as staged VM metadata.
  for (int i = 0; i < 128; ++i)
    ASSERT_TRUE(tree.tryInsert(i * 37 % 128, i));
  ASSERT_EQ(tree.count(), 128U);
  for (int i = 0; i < 128; ++i)
    ASSERT_TRUE(tree.tryInsert(i, i + 1000));
  ASSERT_EQ(tree.count(), 128U);
  int expected = 0;
  for (auto it = tree.begin(); it != tree.end(); ++it) {
    EXPECT_EQ(it.key(), expected);
    EXPECT_EQ(it.value(), expected + 1000);
    ++expected;
  }
  EXPECT_EQ(expected, 128);
  for (int i = 0; i < 128; i += 2)
    tree.remove(i);
  for (int i = 0; i < 128; ++i)
    EXPECT_EQ(tree.contains(i), (i & 1) != 0);
}

TEST(PedigreeTree, FallibleInsertionMovesOwnership) {
  Tree<int, SharedPointer<int>> tree;
  auto first = SharedPointer<int>::allocate(7);
  auto second = SharedPointer<int>::allocate(9);
  int* retained = second.get();
  ASSERT_TRUE(tree.tryInsert(1, pedigree_std::move(first)));
  ASSERT_TRUE(tree.tryInsert(1, pedigree_std::move(second)));
  EXPECT_EQ(first.get(), nullptr);
  EXPECT_EQ(second.get(), nullptr);
  EXPECT_EQ(tree.count(), 1U);
  EXPECT_EQ(tree.lookup(1).get(), retained);
  tree.clear();
  EXPECT_EQ(tree.count(), 0U);
}

TEST(PedigreeTree, MutableValueSurvivesRotationsAndUnrelatedRemoval) {
  struct Value {
    int content = 0;
  };
  Tree<int, Value> tree;
  EXPECT_EQ(tree.find(32), nullptr);
  ASSERT_TRUE(tree.tryInsert(32, Value{123}));
  Value* retained = tree.find(32);
  ASSERT_NE(retained, nullptr);
  for (int i = 0; i < 128; ++i)
    if (i != 32)
      ASSERT_TRUE(tree.tryInsert(i, Value{i}));
  EXPECT_EQ(tree.find(32), retained);
  retained->content = 456;
  for (int i = 0; i < 128; i += 2)
    if (i != 32)
      tree.remove(i);
  ASSERT_EQ(tree.find(32), retained);
  EXPECT_EQ(tree.find(32)->content, 456);
  EXPECT_EQ(tree.find(30), nullptr);
  EXPECT_EQ(tree.find(31)->content, 31);
  tree.remove(32);
  EXPECT_EQ(tree.find(32), nullptr);
  tree.clear();
  EXPECT_EQ(tree.find(31), nullptr);
}

TEST(PedigreeTree, EmptyIteratorEqualityIsSymmetric) {
  Tree<int, int> tree;
  Tree<int, int>::Iterator initial;
  Tree<int, int>::ConstIterator constInitial;
  const auto end = tree.end();
  EXPECT_EQ(initial, initial);
  EXPECT_EQ(end, end);
  EXPECT_EQ(initial, end);
  EXPECT_EQ(end, initial);
  EXPECT_EQ(initial, constInitial);
  EXPECT_EQ(constInitial, initial);

  auto empty = tree.begin();
  EXPECT_EQ(empty, end);
  EXPECT_EQ(end, empty);
  EXPECT_EQ(empty, constInitial);
  EXPECT_EQ(constInitial, empty);
  const Tree<int, int>& readOnly = tree;
  EXPECT_EQ(readOnly.begin(), end);
  EXPECT_EQ(end, readOnly.begin());

  // Prepared mapping code deliberately avoids allocating an empty cursor.
  auto cursor = tree.count() ? tree.begin() : tree.end();
  ASSERT_EQ(cursor, tree.end());
  size_t visited = 0;
  for (; cursor != tree.end(); ++cursor)
    ++visited;
  EXPECT_EQ(visited, 0U);
}

TEST(PedigreeTree, LiveAndExhaustedIteratorEqualityIsSymmetric) {
  Tree<int, int> tree;
  ASSERT_TRUE(tree.tryInsert(2, 20));
  ASSERT_TRUE(tree.tryInsert(1, 10));
  ASSERT_TRUE(tree.tryInsert(3, 30));
  Tree<int, int>::Iterator initial;
  const auto end = tree.end();
  auto cursor = tree.begin();
  Tree<int, int>::ConstIterator readOnly = cursor;
  EXPECT_EQ(cursor, readOnly);
  EXPECT_EQ(readOnly, cursor);
  EXPECT_NE(cursor, end);
  EXPECT_NE(end, cursor);
  EXPECT_NE(initial, cursor);
  EXPECT_NE(cursor, initial);
  EXPECT_NE(readOnly, end);
  EXPECT_NE(end, readOnly);

  Tree<int, int> other;
  other.insert(1, 10);
  EXPECT_NE(cursor, other.begin());
  EXPECT_NE(other.begin(), cursor);

  size_t visited = 0;
  for (; cursor != end; ++cursor) {
    ASSERT_LT(visited, 3U);
    EXPECT_EQ(cursor.key(), static_cast<int>(visited + 1));
    ++visited;
  }
  EXPECT_EQ(visited, 3U);
  EXPECT_EQ(cursor, end);
  EXPECT_EQ(end, cursor);
  EXPECT_EQ(cursor, initial);
  EXPECT_EQ(initial, cursor);
  EXPECT_EQ(readOnly, end);
  EXPECT_EQ(end, readOnly);
}

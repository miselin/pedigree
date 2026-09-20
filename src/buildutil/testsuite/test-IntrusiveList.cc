/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/IntrusiveList.h"

#include <gtest/gtest.h>

struct IntrusiveListItem {
  explicit IntrusiveListItem(int value) : value(value) {}

  int value;
  IntrusiveListNode<IntrusiveListItem> node;
};

using ItemList = IntrusiveList<IntrusiveListItem, &IntrusiveListItem::node>;

TEST(IntrusiveList, StartsEmpty) {
  ItemList list;

  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.count(), 0U);
  EXPECT_EQ(list.begin(), list.end());
  EXPECT_EQ(list.rbegin(), list.rend());
  EXPECT_EQ(list.popFront(), nullptr);
  EXPECT_EQ(list.popBack(), nullptr);
}

TEST(IntrusiveList, PushAndPopBothEnds) {
  IntrusiveListItem one(1), two(2), three(3);
  ItemList list;

  list.pushBack(two);
  list.pushFront(one);
  list.pushBack(three);

  EXPECT_EQ(list.count(), 3U);
  EXPECT_EQ(list.popFront(), &one);
  EXPECT_EQ(list.popBack(), &three);
  EXPECT_EQ(list.popBack(), &two);
  EXPECT_TRUE(list.empty());
}

TEST(IntrusiveList, IteratesInBothDirections) {
  IntrusiveListItem one(1), two(2), three(3);
  ItemList list;
  list.pushBack(one);
  list.pushBack(two);
  list.pushBack(three);

  auto forward = list.begin();
  EXPECT_EQ((forward++)->value, 1);
  EXPECT_EQ((forward++)->value, 2);
  EXPECT_EQ((forward++)->value, 3);
  EXPECT_EQ(forward, list.end());

  const ItemList& constList = list;
  auto reverse = constList.rbegin();
  EXPECT_EQ((reverse++)->value, 3);
  EXPECT_EQ((reverse++)->value, 2);
  EXPECT_EQ((reverse++)->value, 1);
  EXPECT_EQ(reverse, constList.rend());
}

TEST(IntrusiveList, EraseMaintainsBoundaries) {
  IntrusiveListItem one(1), two(2), three(3);
  ItemList list;
  list.pushBack(one);
  list.pushBack(two);
  list.pushBack(three);

  auto middle = list.begin();
  ++middle;
  middle = list.erase(middle);
  ASSERT_NE(middle, list.end());
  EXPECT_EQ(middle->value, 3);

  auto last = list.rbegin();
  last = list.erase(last);
  ASSERT_NE(last, list.rend());
  EXPECT_EQ(last->value, 1);
  EXPECT_EQ(list.count(), 1U);
  EXPECT_EQ(list.popFront(), &one);
}

TEST(IntrusiveList, ClearAllowsNodeReuse) {
  IntrusiveListItem one(1), two(2);
  ItemList list;
  list.pushBack(one);
  list.pushBack(two);

  list.clear();
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.begin(), list.end());

  list.pushFront(two);
  list.pushFront(one);
  EXPECT_EQ(list.popFront(), &one);
  EXPECT_EQ(list.popFront(), &two);
}

TEST(IntrusiveList, ContainsAndUnlinkAreListSpecific) {
  IntrusiveListItem one(1), two(2), three(3), other(4);
  ItemList list, otherList;
  list.pushBack(one);
  list.pushBack(two);
  list.pushBack(three);
  otherList.pushBack(other);

  EXPECT_TRUE(list.contains(one));
  EXPECT_TRUE(list.contains(two));
  EXPECT_FALSE(list.contains(other));
  EXPECT_TRUE(otherList.contains(other));
  EXPECT_FALSE(otherList.contains(two));

  EXPECT_TRUE(list.unlink(two));
  EXPECT_FALSE(list.contains(two));
  EXPECT_FALSE(list.unlink(two));
  EXPECT_FALSE(list.unlink(other));
  EXPECT_TRUE(otherList.contains(other));
  ASSERT_EQ(list.count(), 2U);

  auto it = list.begin();
  EXPECT_EQ((it++)->value, 1);
  EXPECT_EQ((it++)->value, 3);
  EXPECT_EQ(it, list.end());

  list.pushFront(two);
  EXPECT_TRUE(list.contains(two));
  EXPECT_EQ(list.popFront(), &two);
}

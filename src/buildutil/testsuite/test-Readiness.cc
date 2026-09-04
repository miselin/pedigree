/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <gtest/gtest.h>

#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/utilities/Vector.h"

namespace {
class TestReadinessObserver final : public ReadinessObserver {
 public:
  void readinessChanged(ReadyMask mask) override {
    masks.pushBack(mask);
  }

  Vector<ReadyMask> masks;
};

class TestReadinessSource final : public ReadinessSource {
 public:
  using ReadinessSource::closeReadiness;
  using ReadinessSource::notifyReadiness;
};
}  // namespace

TEST(Readiness, FiltersAndRemovesExactSubscriptions) {
  TestReadinessSource source;
  auto *readObserver = new TestReadinessObserver;
  auto *writeObserver = new TestReadinessObserver;
  SharedPointer<ReadinessObserver> readBase(readObserver);
  SharedPointer<ReadinessObserver> writeBase(writeObserver);
  ReadinessSubscription readSubscription;
  ReadinessSubscription writeSubscription;

  ASSERT_TRUE(source.subscribeReadiness(ReadyRead, readBase, readSubscription));
  ASSERT_TRUE(source.subscribeReadiness(ReadyWrite, writeBase, writeSubscription));

  source.notifyReadiness(ReadyRead);
  ASSERT_EQ(readObserver->masks.count(), 1U);
  EXPECT_EQ(readObserver->masks[0], ReadyRead);
  EXPECT_EQ(writeObserver->masks.count(), 0U);

  readSubscription.reset();
  source.notifyReadiness(ReadyRead | ReadyWrite);
  EXPECT_EQ(readObserver->masks.count(), 1U);
  ASSERT_EQ(writeObserver->masks.count(), 1U);
  EXPECT_EQ(writeObserver->masks[0], ReadyRead | ReadyWrite);
}

TEST(Readiness, TerminalStateIsAlwaysReported) {
  TestReadinessSource source;
  auto *observer = new TestReadinessObserver;
  SharedPointer<ReadinessObserver> base(observer);
  ReadinessSubscription subscription;

  ASSERT_TRUE(source.subscribeReadiness(ReadyRead, base, subscription));
  source.closeReadiness(ReadyHangup | ReadyInvalid);

  ASSERT_EQ(observer->masks.count(), 1U);
  EXPECT_EQ(observer->masks[0], ReadyHangup | ReadyInvalid);

  ReadinessSubscription rejected;
  EXPECT_FALSE(source.subscribeReadiness(ReadyRead, base, rejected));
  EXPECT_FALSE(static_cast<bool>(rejected));
}

TEST(Readiness, MoveTransfersRegistrationOwnership) {
  TestReadinessSource source;
  auto *observer = new TestReadinessObserver;
  SharedPointer<ReadinessObserver> base(observer);
  ReadinessSubscription first;

  ASSERT_TRUE(source.subscribeReadiness(ReadyRead, base, first));
  ReadinessSubscription second(pedigree_std::move(first));
  EXPECT_FALSE(static_cast<bool>(first));
  EXPECT_TRUE(static_cast<bool>(second));

  source.notifyReadiness(ReadyRead);
  EXPECT_EQ(observer->masks.count(), 1U);
}

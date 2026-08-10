/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "system/kernel/machine/mach_pc/RtcAlarmQueue.h"
#include <gtest/gtest.h>

namespace {
void* pointer(size_t value) {
  return reinterpret_cast<void*>(value);
}
}  // namespace

TEST(RtcAlarmQueue, RemoteRemovalDrainsCommittedDispatch) {
  RtcAlarmQueue queue;
  RtcAlarmQueue::Record alarm;
  void* event = pointer(0x1000);
  void* target = pointer(0x2000);
  void* dispatchOwner = pointer(0x3000);
  void* removalOwner = pointer(0x4000);

  alarm.prepare(event, 10, target);
  queue.add(&alarm);
  EXPECT_EQ(queue.claimDue(10, dispatchOwner), &alarm);

  const RtcAlarmQueue::Removal removal = queue.removeFirst(event, removalOwner);
  EXPECT_EQ(removal.disposition, RtcAlarmQueue::RemovalDisposition::RemoteInFlight);
  EXPECT_TRUE(queue.hasRemoteInFlight(event, removalOwner));
  EXPECT_EQ(queue.activeCount(), 1U);

  queue.completeDispatch(&alarm);
  EXPECT_FALSE(queue.hasRemoteInFlight(event, removalOwner));
  EXPECT_EQ(queue.activeCount(), 0U);
  EXPECT_EQ(queue.freeCount(), 1U);
}

TEST(RtcAlarmQueue, SameContextRemovalDefersWithoutSelfDrain) {
  RtcAlarmQueue queue;
  RtcAlarmQueue::Record alarm;
  void* event = pointer(0x1000);
  void* owner = pointer(0x3000);

  alarm.prepare(event, 10, pointer(0x2000));
  queue.add(&alarm);
  EXPECT_EQ(queue.claimDue(10, owner), &alarm);

  const RtcAlarmQueue::Removal removal = queue.removeFirst(event, owner);
  EXPECT_EQ(removal.disposition, RtcAlarmQueue::RemovalDisposition::SelfDeferred);
  EXPECT_TRUE(alarm.deferredRemoval());
  EXPECT_FALSE(queue.hasRemoteInFlight(event, owner));

  queue.completeDispatch(&alarm);
  EXPECT_EQ(queue.activeCount(), 0U);
  EXPECT_EQ(queue.freeCount(), 1U);
}

TEST(RtcAlarmQueue, ClaimsEveryDueAlarmAndLeavesFutureAlarmQueued) {
  RtcAlarmQueue queue;
  RtcAlarmQueue::Record first;
  RtcAlarmQueue::Record second;
  RtcAlarmQueue::Record future;
  void* owner = pointer(0x3000);

  first.prepare(pointer(0x1000), 10, pointer(0x2000));
  second.prepare(pointer(0x1001), 5, pointer(0x2001));
  future.prepare(pointer(0x1002), 20, pointer(0x2002));
  queue.add(&first);
  queue.add(&second);
  queue.add(&future);

  EXPECT_EQ(queue.claimDue(10, owner), &first);
  EXPECT_EQ(queue.claimDue(10, owner), &second);
  EXPECT_EQ(queue.claimDue(10, owner), nullptr);

  queue.completeDispatch(&first);
  queue.completeDispatch(&second);
  EXPECT_EQ(queue.activeCount(), 1U);
  EXPECT_EQ(queue.freeCount(), 2U);
  EXPECT_EQ(queue.claimDue(20, owner), &future);
  queue.completeDispatch(&future);
  EXPECT_EQ(queue.activeCount(), 0U);
  EXPECT_EQ(queue.freeCount(), 3U);
}

TEST(RtcAlarmQueue, CancellationRecyclesRecordsForLaterAlarms) {
  RtcAlarmQueue queue;
  RtcAlarmQueue::Record first;
  RtcAlarmQueue::Record second;
  void* event = pointer(0x1000);
  void* owner = pointer(0x3000);

  first.prepare(event, 10, pointer(0x2000));
  second.prepare(event, 20, pointer(0x2000));
  queue.add(&first);
  queue.add(&second);

  bool remoteInFlight = false;
  bool selfDeferred = false;
  RtcAlarmQueue::Record* removed =
      queue.removeAllQueued(event, owner, remoteInFlight, selfDeferred);
  EXPECT_FALSE(remoteInFlight);
  EXPECT_FALSE(selfDeferred);
  EXPECT_EQ(queue.activeCount(), 0U);

  queue.recycleList(removed);
  EXPECT_EQ(queue.freeCount(), 2U);
  RtcAlarmQueue::Record* reusedFirst = queue.takeReusable();
  RtcAlarmQueue::Record* reusedSecond = queue.takeReusable();
  EXPECT_NE(reusedFirst, nullptr);
  EXPECT_NE(reusedSecond, nullptr);
  EXPECT_NE(reusedFirst, reusedSecond);
  EXPECT_EQ(queue.takeReusable(), nullptr);

  reusedFirst->prepare(pointer(0x1001), 30, pointer(0x2001));
  queue.add(reusedFirst);
  const RtcAlarmQueue::Removal removal = queue.removeFirst(pointer(0x1001), owner);
  EXPECT_EQ(removal.disposition, RtcAlarmQueue::RemovalDisposition::Removed);
  EXPECT_EQ(removal.record, reusedFirst);
  queue.recycleList(removal.record);
  queue.recycleList(reusedSecond);
  EXPECT_EQ(queue.freeCount(), 2U);
}

TEST(RtcAlarmQueue, SelfRemovalStillCancelsQueuedDuplicates) {
  RtcAlarmQueue queue;
  RtcAlarmQueue::Record dispatching;
  RtcAlarmQueue::Record queued;
  void* event = pointer(0x1000);
  void* owner = pointer(0x3000);

  dispatching.prepare(event, 10, pointer(0x2000));
  queued.prepare(event, 20, pointer(0x2000));
  queue.add(&dispatching);
  queue.add(&queued);
  EXPECT_EQ(queue.claimDue(10, owner), &dispatching);

  bool remoteInFlight = false;
  bool selfDeferred = false;
  RtcAlarmQueue::Record* removed =
      queue.removeAllQueued(event, owner, remoteInFlight, selfDeferred);
  EXPECT_FALSE(remoteInFlight);
  EXPECT_TRUE(selfDeferred);
  EXPECT_TRUE(dispatching.deferredRemoval());
  EXPECT_EQ(removed, &queued);
  EXPECT_EQ(queue.activeCount(), 1U);

  queue.recycleList(removed);
  queue.completeDispatch(&dispatching);
  EXPECT_EQ(queue.activeCount(), 0U);
  EXPECT_EQ(queue.freeCount(), 2U);
}

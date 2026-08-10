/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "system/kernel/machine/mach_pc/LocalApicTimerHandlerSlots.h"
#include <gtest/gtest.h>

namespace {
struct HandlerToken {};

SchedulerTimerHandler* handlerFor(HandlerToken& token) {
  return reinterpret_cast<SchedulerTimerHandler*>(&token);
}
}  // namespace

TEST(LocalApicTimerHandlerSlots, KeepsProcessorsIsolated) {
  LocalApicTimerHandlerSlots slots;
  HandlerToken firstToken;
  HandlerToken lastToken;
  SchedulerTimerHandler* first = handlerFor(firstToken);
  SchedulerTimerHandler* last = handlerFor(lastToken);

  EXPECT_TRUE(slots.registerHandler(0, first));
  EXPECT_TRUE(slots.registerHandler(255, last));

  EXPECT_TRUE(slots.isPublished(0, first));
  EXPECT_FALSE(slots.isPublished(1, first));
  EXPECT_FALSE(slots.isPublished(254, first));
  EXPECT_TRUE(slots.isPublished(255, last));

  SchedulerTimerHandlerSlot::DispatchGuard firstDispatch;
  SchedulerTimerHandlerSlot::DispatchGuard lastDispatch;
  EXPECT_TRUE(slots.beginDispatch(0, firstDispatch));
  EXPECT_EQ(firstDispatch.handler(), first);
  EXPECT_TRUE(slots.beginDispatch(255, lastDispatch));
  EXPECT_EQ(lastDispatch.handler(), last);
}

TEST(LocalApicTimerHandlerSlots, RejectsDuplicateAndConflictingRegistration) {
  LocalApicTimerHandlerSlots slots;
  HandlerToken firstToken;
  HandlerToken secondToken;
  SchedulerTimerHandler* first = handlerFor(firstToken);
  SchedulerTimerHandler* second = handlerFor(secondToken);

  EXPECT_TRUE(slots.registerHandler(17, first));
  EXPECT_FALSE(slots.registerHandler(17, first));
  EXPECT_FALSE(slots.registerHandler(17, second));
  EXPECT_TRUE(slots.isPublished(17, first));
}

TEST(LocalApicTimerHandlerSlots, RemovesOnlyTheExactHandler) {
  LocalApicTimerHandlerSlots slots;
  HandlerToken firstToken;
  HandlerToken secondToken;
  SchedulerTimerHandler* first = handlerFor(firstToken);
  SchedulerTimerHandler* second = handlerFor(secondToken);

  ASSERT_TRUE(slots.registerHandler(42, first));
  EXPECT_FALSE(slots.removeHandler(42, nullptr));
  EXPECT_FALSE(slots.removeHandler(42, second));
  EXPECT_TRUE(slots.isPublished(42, first));

  EXPECT_TRUE(slots.removeHandler(42, first));
  EXPECT_FALSE(slots.isPublished(42, first));
  EXPECT_FALSE(slots.removeHandler(42, first));
}

TEST(LocalApicTimerHandlerSlots, RejectsRemoteRemovalAfterOwnerAdmission) {
  LocalApicTimerHandlerSlots slots;
  HandlerToken token;
  SchedulerTimerHandler* handler = handlerFor(token);

  ASSERT_TRUE(slots.registerHandler(42, handler));
  EXPECT_TRUE(slots.isPublished(42, handler));
  EXPECT_FALSE(slots.removeHandler(43, handler));
  EXPECT_TRUE(slots.isPublished(42, handler));
  EXPECT_TRUE(slots.removeHandler(42, handler));
  EXPECT_FALSE(slots.isPublished(42, handler));
}

TEST(LocalApicTimerHandlerSlots, DispatchGuardRetainsTheRawApicLifetime) {
  constexpr ProcessorId RawApicId = 93;
  LocalApicTimerHandlerSlots slots;
  HandlerToken token;
  SchedulerTimerHandler* handler = handlerFor(token);

  ASSERT_TRUE(slots.registerHandler(RawApicId, handler));
  {
    SchedulerTimerHandlerSlot::DispatchGuard dispatch;
    ASSERT_TRUE(slots.beginDispatch(RawApicId, dispatch));
    EXPECT_EQ(dispatch.handler(), handler);

    // The interrupt path may call ordinary scheduler code, but it cannot
    // let removal report a completed callback lifetime before return.
    EXPECT_FALSE(slots.removeHandler(RawApicId, handler));
    EXPECT_TRUE(slots.isPublished(RawApicId, handler));
  }
  EXPECT_TRUE(slots.removeHandler(RawApicId, handler));
}

TEST(LocalApicTimerHandlerSlots, RetainsRawApicKeyAcrossLogicalIdTransition) {
  constexpr ProcessorId EarlyLogicalId = 0;
  constexpr ProcessorId LateLogicalId = 37;
  constexpr ProcessorId RawApicId = 11;
  static_assert(RawApicId != EarlyLogicalId && RawApicId != LateLogicalId,
                "the transition model needs distinct logical and raw identities");

  LocalApicTimerHandlerSlots slots;
  HandlerToken token;
  SchedulerTimerHandler* handler = handlerFor(token);

  // The early BSP can still be logical processor zero when it registers a
  // timer handler. AP topology construction later assigns its logical ID;
  // the LAPIC's raw physical ID is the durable timer/IPI lookup key.
  ASSERT_TRUE(slots.registerHandler(RawApicId, handler));
  EXPECT_TRUE(slots.isPublished(RawApicId, handler));
  EXPECT_FALSE(slots.isPublished(EarlyLogicalId, handler));
  EXPECT_FALSE(slots.isPublished(LateLogicalId, handler));
  EXPECT_TRUE(slots.removeHandler(RawApicId, handler));
}

TEST(LocalApicTimerHandlerSlots, RejectsNullAndOutOfBoundsOperations) {
  LocalApicTimerHandlerSlots slots;
  HandlerToken token;
  SchedulerTimerHandler* handler = handlerFor(token);

  EXPECT_FALSE(slots.registerHandler(0, nullptr));
  EXPECT_FALSE(slots.registerHandler(256, handler));
  EXPECT_FALSE(slots.registerHandler(~static_cast<ProcessorId>(0), handler));
  SchedulerTimerHandlerSlot::DispatchGuard outOfBounds;
  EXPECT_FALSE(slots.beginDispatch(256, outOfBounds));
  EXPECT_FALSE(slots.beginDispatch(~static_cast<ProcessorId>(0), outOfBounds));
  EXPECT_FALSE(slots.removeHandler(256, handler));
  EXPECT_FALSE(slots.removeHandler(~static_cast<ProcessorId>(0), handler));

  EXPECT_TRUE(slots.registerHandler(255, handler));
  EXPECT_TRUE(slots.isPublished(255, handler));
  EXPECT_TRUE(slots.removeHandler(255, handler));
}

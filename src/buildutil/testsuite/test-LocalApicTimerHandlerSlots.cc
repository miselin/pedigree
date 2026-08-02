/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "system/kernel/machine/mach_pc/LocalApicTimerHandlerSlots.h"

namespace
{
struct HandlerToken
{
};

SchedulerTimerHandler *handlerFor(HandlerToken &token)
{
    return reinterpret_cast<SchedulerTimerHandler *>(&token);
}
}  // namespace

TEST(LocalApicTimerHandlerSlots, KeepsProcessorsIsolated)
{
    LocalApicTimerHandlerSlots slots;
    HandlerToken firstToken;
    HandlerToken lastToken;
    SchedulerTimerHandler *first = handlerFor(firstToken);
    SchedulerTimerHandler *last = handlerFor(lastToken);

    EXPECT_TRUE(slots.registerHandler(0, first));
    EXPECT_TRUE(slots.registerHandler(255, last));

    EXPECT_EQ(slots.load(0), first);
    EXPECT_EQ(slots.load(1), nullptr);
    EXPECT_EQ(slots.load(254), nullptr);
    EXPECT_EQ(slots.load(255), last);
}

TEST(LocalApicTimerHandlerSlots, RejectsDuplicateAndConflictingRegistration)
{
    LocalApicTimerHandlerSlots slots;
    HandlerToken firstToken;
    HandlerToken secondToken;
    SchedulerTimerHandler *first = handlerFor(firstToken);
    SchedulerTimerHandler *second = handlerFor(secondToken);

    EXPECT_TRUE(slots.registerHandler(17, first));
    EXPECT_FALSE(slots.registerHandler(17, first));
    EXPECT_FALSE(slots.registerHandler(17, second));
    EXPECT_EQ(slots.load(17), first);
}

TEST(LocalApicTimerHandlerSlots, RemovesOnlyTheExactHandler)
{
    LocalApicTimerHandlerSlots slots;
    HandlerToken firstToken;
    HandlerToken secondToken;
    SchedulerTimerHandler *first = handlerFor(firstToken);
    SchedulerTimerHandler *second = handlerFor(secondToken);

    ASSERT_TRUE(slots.registerHandler(42, first));
    EXPECT_FALSE(slots.removeHandler(42, nullptr));
    EXPECT_FALSE(slots.removeHandler(42, second));
    EXPECT_EQ(slots.load(42), first);

    EXPECT_TRUE(slots.removeHandler(42, first));
    EXPECT_EQ(slots.load(42), nullptr);
    EXPECT_FALSE(slots.removeHandler(42, first));
}

TEST(LocalApicTimerHandlerSlots, RejectsNullAndOutOfBoundsOperations)
{
    LocalApicTimerHandlerSlots slots;
    HandlerToken token;
    SchedulerTimerHandler *handler = handlerFor(token);

    EXPECT_FALSE(slots.registerHandler(0, nullptr));
    EXPECT_FALSE(slots.registerHandler(256, handler));
    EXPECT_FALSE(
        slots.registerHandler(~static_cast<ProcessorId>(0), handler));
    EXPECT_EQ(slots.load(256), nullptr);
    EXPECT_EQ(slots.load(~static_cast<ProcessorId>(0)), nullptr);
    EXPECT_FALSE(slots.removeHandler(256, handler));
    EXPECT_FALSE(slots.removeHandler(~static_cast<ProcessorId>(0), handler));

    EXPECT_TRUE(slots.registerHandler(255, handler));
    EXPECT_EQ(slots.load(255), handler);
    EXPECT_TRUE(slots.removeHandler(255, handler));
}

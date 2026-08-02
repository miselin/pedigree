/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "pedigree/kernel/machine/SchedulerTimerHandlerSlot.h"

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

TEST(SchedulerTimerHandlerSlot, RejectsDuplicateAndConflictingPublication)
{
    SchedulerTimerHandlerSlot slot;
    HandlerToken firstToken;
    HandlerToken secondToken;
    SchedulerTimerHandler *first = handlerFor(firstToken);
    SchedulerTimerHandler *second = handlerFor(secondToken);

    EXPECT_EQ(slot.load(), nullptr);
    EXPECT_FALSE(slot.publish(nullptr));
    EXPECT_TRUE(slot.publish(first));
    EXPECT_FALSE(slot.publish(first));
    EXPECT_FALSE(slot.publish(second));
    EXPECT_EQ(slot.load(), first);
}

TEST(SchedulerTimerHandlerSlot, UnpublishesOnlyTheExactOwner)
{
    SchedulerTimerHandlerSlot slot;
    HandlerToken firstToken;
    HandlerToken secondToken;
    SchedulerTimerHandler *first = handlerFor(firstToken);
    SchedulerTimerHandler *second = handlerFor(secondToken);

    ASSERT_TRUE(slot.publish(first));
    EXPECT_FALSE(slot.unpublish(nullptr));
    EXPECT_FALSE(slot.unpublish(second));
    EXPECT_EQ(slot.load(), first);
    EXPECT_TRUE(slot.unpublish(first));
    EXPECT_EQ(slot.load(), nullptr);
    EXPECT_FALSE(slot.unpublish(first));
    EXPECT_TRUE(slot.publish(second));
    EXPECT_EQ(slot.load(), second);
}

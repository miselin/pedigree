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
constexpr size_t Owner = 7;
constexpr size_t OtherProcessor = 11;

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

    EXPECT_FALSE(slot.isPublished(Owner, first));
    EXPECT_FALSE(slot.publish(Owner, nullptr));
    EXPECT_TRUE(slot.publish(Owner, first));
    EXPECT_FALSE(slot.publish(Owner, first));
    EXPECT_FALSE(slot.publish(Owner, second));
    EXPECT_TRUE(slot.isPublished(Owner, first));
}

TEST(SchedulerTimerHandlerSlot, UnpublishesOnlyTheExactOwner)
{
    SchedulerTimerHandlerSlot slot;
    HandlerToken firstToken;
    HandlerToken secondToken;
    SchedulerTimerHandler *first = handlerFor(firstToken);
    SchedulerTimerHandler *second = handlerFor(secondToken);

    ASSERT_TRUE(slot.publish(Owner, first));
    EXPECT_FALSE(slot.unpublish(Owner, nullptr));
    EXPECT_FALSE(slot.unpublish(Owner, second));
    EXPECT_TRUE(slot.isPublished(Owner, first));
    EXPECT_TRUE(slot.unpublish(Owner, first));
    EXPECT_FALSE(slot.isPublished(Owner, first));
    EXPECT_FALSE(slot.unpublish(Owner, first));
    EXPECT_TRUE(slot.publish(Owner, second));
    EXPECT_TRUE(slot.isPublished(Owner, second));
}

TEST(SchedulerTimerHandlerSlot, RejectsRemoteRemovalAfterOwnerAdmission)
{
    SchedulerTimerHandlerSlot slot;
    HandlerToken token;
    SchedulerTimerHandler *handler = handlerFor(token);

    ASSERT_TRUE(slot.publish(Owner, handler));

    // Models the decisive old interleaving: the timer core has admitted the
    // raw pointer when another CPU attempts teardown. The old slot allowed
    // that teardown; the owner-qualified slot rejects it.
    {
        SchedulerTimerHandlerSlot::DispatchGuard dispatch;
        ASSERT_TRUE(slot.beginDispatch(Owner, dispatch));
        EXPECT_FALSE(slot.unpublish(OtherProcessor, handler));
        EXPECT_EQ(dispatch.handler(), handler);
    }

    EXPECT_FALSE(slot.isPublished(OtherProcessor, handler));
    EXPECT_TRUE(slot.unpublish(Owner, handler));
    EXPECT_FALSE(slot.isPublished(Owner, handler));
}

TEST(SchedulerTimerHandlerSlot, SuspendedDispatchesRemainPreemptible)
{
    SchedulerTimerHandlerSlot slot;
    HandlerToken token;
    SchedulerTimerHandler *handler = handlerFor(token);

    ASSERT_TRUE(slot.publish(Owner, handler));
    {
        SchedulerTimerHandlerSlot::DispatchGuard firstDispatch;
        ASSERT_TRUE(slot.beginDispatch(Owner, firstDispatch));
        EXPECT_EQ(firstDispatch.handler(), handler);
        EXPECT_EQ(slot.activeDispatches(), 1U);

        // Models timer A switching to thread B without returning through A's
        // hard frame. B's next timer must still reach the scheduler callback.
        {
            SchedulerTimerHandlerSlot::DispatchGuard secondDispatch;
            ASSERT_TRUE(slot.beginDispatch(Owner, secondDispatch));
            EXPECT_EQ(secondDispatch.handler(), handler);
            EXPECT_EQ(slot.activeDispatches(), 2U);

            // Neither a self-removal nor ordinary teardown may report a false
            // lifetime barrier while either suspended frame still owns a pin.
            EXPECT_FALSE(slot.unpublish(Owner, handler));
        }

        EXPECT_EQ(slot.activeDispatches(), 1U);
        EXPECT_FALSE(slot.unpublish(Owner, handler));
    }

    EXPECT_EQ(slot.activeDispatches(), 0U);
    EXPECT_TRUE(slot.unpublish(Owner, handler));
    EXPECT_FALSE(slot.isPublished(Owner, handler));
}

TEST(SchedulerTimerHandlerSlot, ExplicitReleaseIsIdempotent)
{
    SchedulerTimerHandlerSlot slot;
    HandlerToken token;
    SchedulerTimerHandler *handler = handlerFor(token);

    ASSERT_TRUE(slot.publish(Owner, handler));
    {
        SchedulerTimerHandlerSlot::DispatchGuard dispatch;
        ASSERT_TRUE(slot.beginDispatch(Owner, dispatch));
        ASSERT_EQ(slot.activeDispatches(), 1U);
        dispatch.release();
        EXPECT_EQ(slot.activeDispatches(), 0U);
        EXPECT_EQ(dispatch.handler(), nullptr);
        dispatch.release();
        EXPECT_EQ(slot.activeDispatches(), 0U);
    }

    EXPECT_EQ(slot.activeDispatches(), 0U);
    EXPECT_TRUE(slot.unpublish(Owner, handler));
}

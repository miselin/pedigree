/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/debugger/commands/ThreadWaitDiagnostic.h"

#include <gtest/gtest.h>

TEST(ThreadWaitDiagnostic, OmitsNonWaitingThread) {
  ThreadWaitDiagnostic wait;
  HugeStaticString line;

  appendThreadWaitDiagnostic(wait, line);

  EXPECT_STREQ("", static_cast<const char*>(line));
}

TEST(ThreadWaitDiagnostic, FlagsSleepingThreadWithoutWaitQueue) {
  ThreadWaitDiagnostic wait;
  wait.sleeping = true;
  HugeStaticString line;

  appendThreadWaitDiagnostic(wait, line);

  EXPECT_STREQ(" [INVALID: sleeping without waitq]", static_cast<const char*>(line));
}

TEST(ThreadWaitDiagnostic, RendersCompleteQueuedWait) {
  ThreadWaitDiagnostic wait;
  wait.sleeping = true;
  wait.hasWait = true;
  wait.queue = 0x1234;
  wait.channelOwner = 0x5678;
  wait.channelValue = 0x9abc;
  wait.reason = WaitQueue::WakeReason::Waiting;
  wait.stateLevel = 2;
  wait.mutexOwner = 0xdef0;
  wait.queued = true;
  HugeStaticString line;

  appendThreadWaitDiagnostic(wait, line);

  EXPECT_STREQ(
      " [waitq=1234, channel=5678:9abc, reason=waiting, level=2, "
      "owner=def0, queued]",
      static_cast<const char*>(line));
}

TEST(ThreadWaitDiagnostic, RendersPublishingWakeWithoutMutexOwner) {
  ThreadWaitDiagnostic wait;
  wait.hasWait = true;
  wait.queue = 0x1234;
  wait.channelOwner = 0;
  wait.channelValue = 7;
  wait.reason = WaitQueue::WakeReason::Terminating;
  wait.stateLevel = 1;
  wait.queued = false;
  HugeStaticString line;

  appendThreadWaitDiagnostic(wait, line);

  EXPECT_STREQ(
      " [waitq=1234, channel=0:7, reason=terminating, level=1, "
      "publishing]",
      static_cast<const char*>(line));
}

/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <limits>

#include "modules/drivers/common/InterruptProbe.h"
#include <gtest/gtest.h>

TEST(InterruptProbe, PolledCompletionCannotValidateAnInterruptRoute) {
  for (unsigned staleCompletions : {0U, 1234U}) {
    unsigned polledCompletions = 0;
    EXPECT_FALSE(InterruptProbe::run(
        [&] {
          ++polledCompletions;
          return true;
        },
        [&] { return staleCompletions; }));
    EXPECT_EQ(polledCompletions, 1U);
  }
}

TEST(InterruptProbe, RequiresBothCommandSuccessAndFreshInterruptCompletion) {
  unsigned completions = 19;
  EXPECT_FALSE(InterruptProbe::run(
      [&] {
        ++completions;
        return false;
      },
      [&] { return completions; }));
  EXPECT_TRUE(InterruptProbe::run(
      [&] {
        ++completions;
        return true;
      },
      [&] { return completions; }));
}

TEST(InterruptProbe, AcceptsCounterWrapWithoutAcceptingOldCompletions) {
  unsigned completions = std::numeric_limits<unsigned>::max();
  EXPECT_TRUE(InterruptProbe::run(
      [&] {
        ++completions;
        return true;
      },
      [&] { return completions; }));
  EXPECT_EQ(completions, 0U);
  EXPECT_FALSE(InterruptProbe::run([] { return true; }, [&] { return completions; }));
}

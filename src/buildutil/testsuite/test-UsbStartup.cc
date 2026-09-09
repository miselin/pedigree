/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "modules/system/usb/UsbStartup.h"
#include <gtest/gtest.h>

using UsbStartup::Outcome;
using UsbStartup::State;

TEST(UsbStartup, ControllerWithoutFirmwareKeepsAcceptingAfterFailure) {
  State state;
  ASSERT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Failed));
  EXPECT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Neutral));
}

TEST(UsbStartup, TerminalFailureClosesBeforeRecoveryAndSignalsOnce) {
  State state;
  state.enableRecovery(true);
  ASSERT_TRUE(state.begin());
  EXPECT_TRUE(state.finish(Outcome::Failed));
  EXPECT_FALSE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Failed));
}

TEST(UsbStartup, NestedHubFailureWaitsForAllAdmittedProbes) {
  State state;
  state.enableRecovery(true);
  ASSERT_TRUE(state.begin());  // Initial root-port scan.
  ASSERT_TRUE(state.begin());  // Hub probe.
  ASSERT_TRUE(state.begin());  // Child enumeration.
  EXPECT_FALSE(state.finish(Outcome::Failed));
  EXPECT_FALSE(state.finish(Outcome::Neutral));
  EXPECT_TRUE(state.finish(Outcome::Neutral));
  EXPECT_FALSE(state.begin());
}

TEST(UsbStartup, SuccessfulConcurrentLeafPreventsControllerRevocation) {
  State state;
  state.enableRecovery(true);
  ASSERT_TRUE(state.begin());
  ASSERT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Failed));
  EXPECT_FALSE(state.finish(Outcome::DeviceReady));
  ASSERT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Failed));
  EXPECT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Neutral));
}

TEST(UsbStartup, IdleControllerAndUnmatchedInterfacesDoNotTriggerHandback) {
  State state;
  state.enableRecovery(true);
  ASSERT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Neutral));
  EXPECT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Neutral));
}

TEST(UsbStartup, DriverLoadedAfterEnumerationCanStillFailTakeover) {
  State state;
  state.enableRecovery(true);
  ASSERT_TRUE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Neutral));
  ASSERT_TRUE(state.begin());
  EXPECT_TRUE(state.finish(Outcome::Failed));
  EXPECT_FALSE(state.begin());
}

TEST(UsbStartup, TeardownSuppressesRecoveryFromDrainingProbes) {
  State state;
  state.enableRecovery(true);
  ASSERT_TRUE(state.begin());
  state.close();
  EXPECT_FALSE(state.begin());
  EXPECT_FALSE(state.finish(Outcome::Failed));
}

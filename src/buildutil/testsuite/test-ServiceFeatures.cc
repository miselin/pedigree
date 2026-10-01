/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/ServiceFeatures.h"

#include <gtest/gtest.h>

TEST(ServiceFeatures, OperationsCanBeAdvertisedAndWithdrawnIndependently) {
  const ServiceFeatures::Type operations[] = {ServiceFeatures::write, ServiceFeatures::read,
                                              ServiceFeatures::touch, ServiceFeatures::probe,
                                              ServiceFeatures::withdraw};
  ServiceFeatures features;
  for (auto operation : operations) {
    EXPECT_FALSE(features.provides(operation));
    features.add(operation);
  }
  for (auto operation : operations) {
    EXPECT_TRUE(features.provides(operation));
    features.remove(operation);
    for (auto other : operations) {
      EXPECT_EQ(features.provides(other), other != operation);
    }
    features.add(operation);
  }
}

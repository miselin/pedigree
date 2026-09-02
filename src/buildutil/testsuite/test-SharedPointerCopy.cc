/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/utility.h"

#include <gtest/gtest.h>

TEST(PedigreeUtility, NonTrivialOverlappingCopyToLowerAddress) {
  typedef SharedPointer<int> sharedintptr_t;

  sharedintptr_t ptr1 = sharedintptr_t::allocate(1);
  sharedintptr_t ptr2 = sharedintptr_t::allocate(2);
  sharedintptr_t ptr3 = sharedintptr_t::allocate(3);
  sharedintptr_t ptr4 = sharedintptr_t::allocate(4);

  sharedintptr_t values[] = {ptr1, ptr2, ptr3, ptr4};
  pedigree_std::copy(values, values + 1, 3);

  EXPECT_EQ(values[0].get(), ptr2.get());
  EXPECT_EQ(values[1].get(), ptr3.get());
  EXPECT_EQ(values[2].get(), ptr4.get());
}

/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/UniqueResource.h"
#include "pedigree/kernel/utilities/cpp.h"

#include <gtest/gtest.h>

namespace {
struct TestResource {
  explicit TestResource(size_t* releases) : releases(releases) {}

  size_t* releases;
};

struct TestResourceReleaser {
  static void release(TestResource* resource) {
    ++*resource->releases;
  }
};

using TestResourceOwner = UniqueResource<TestResource, TestResourceReleaser>;

static_assert(sizeof(TestResourceOwner) == sizeof(TestResource*),
              "UniqueResource must not add storage overhead");

}  // namespace

TEST(PedigreeUniqueResource, ReleasesExactlyOnce) {
  size_t releases = 0;
  TestResource resource(&releases);
  {
    TestResourceOwner owner = TestResourceOwner::adopt(&resource);
    EXPECT_EQ(owner.get(), &resource);
  }
  EXPECT_EQ(releases, 1U);
}

TEST(PedigreeUniqueResource, MoveTransfersOwnership) {
  size_t releases = 0;
  TestResource resource(&releases);
  {
    TestResourceOwner first = TestResourceOwner::adopt(&resource);
    TestResourceOwner second(pedigree_std::move(first));
    EXPECT_FALSE(first);
    EXPECT_EQ(second.get(), &resource);
  }
  EXPECT_EQ(releases, 1U);
}

TEST(PedigreeUniqueResource, MoveAssignmentReleasesDestinationFirst) {
  size_t firstReleases = 0;
  size_t secondReleases = 0;
  TestResource first(&firstReleases);
  TestResource second(&secondReleases);
  {
    TestResourceOwner destination = TestResourceOwner::adopt(&first);
    TestResourceOwner source = TestResourceOwner::adopt(&second);
    destination = pedigree_std::move(source);
    EXPECT_EQ(firstReleases, 1U);
    EXPECT_EQ(secondReleases, 0U);
    EXPECT_FALSE(source);
    EXPECT_EQ(destination.get(), &second);
  }
  EXPECT_EQ(firstReleases, 1U);
  EXPECT_EQ(secondReleases, 1U);
}

TEST(PedigreeUniqueResource, ReleaseTransfersToCaller) {
  size_t releases = 0;
  TestResource resource(&releases);
  TestResourceOwner owner = TestResourceOwner::adopt(&resource);
  TestResource* transferred = owner.release();
  EXPECT_EQ(transferred, &resource);
  EXPECT_FALSE(owner);
  EXPECT_EQ(releases, 0U);
}

TEST(PedigreeUniqueResource, ResetReleasesBeforeAdopting) {
  size_t firstReleases = 0;
  size_t secondReleases = 0;
  TestResource first(&firstReleases);
  TestResource second(&secondReleases);
  {
    TestResourceOwner owner = TestResourceOwner::adopt(&first);
    owner.reset(&second);
    EXPECT_EQ(firstReleases, 1U);
    EXPECT_EQ(secondReleases, 0U);
  }
  EXPECT_EQ(firstReleases, 1U);
  EXPECT_EQ(secondReleases, 1U);
}

TEST(PedigreeUniqueResource, EmptyResetIsAStableNoOp) {
  TestResourceOwner owner;
  owner.reset();
  EXPECT_FALSE(owner);
  owner = pedigree_std::move(owner);
  EXPECT_FALSE(owner);
}

TEST(PedigreeUniqueResource, ResetToOwnedResourceIsAStableNoOp) {
  size_t releases = 0;
  TestResource resource(&releases);
  {
    TestResourceOwner owner = TestResourceOwner::adopt(&resource);
    owner.reset(owner.get());
    EXPECT_EQ(owner.get(), &resource);
    EXPECT_EQ(releases, 0U);
  }
  EXPECT_EQ(releases, 1U);
}

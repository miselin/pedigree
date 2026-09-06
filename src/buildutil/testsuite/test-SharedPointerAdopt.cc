/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/utilities/SharedPointer.h"

#include <gtest/gtest.h>

namespace {
class AdoptBase {
 public:
  virtual ~AdoptBase() = default;
  virtual int value() const = 0;
};
class AdoptDerived final : public AdoptBase {
 public:
  explicit AdoptDerived(size_t& destroyed) : m_Destroyed(destroyed) {}
  ~AdoptDerived() override {
    ++m_Destroyed;
  }
  int value() const override {
    return 17;
  }

 private:
  size_t& m_Destroyed;
};
}  // namespace

TEST(SharedPointerAdopt, RetainsPolymorphicObjectUntilLastOwner) {
  size_t destroyed = 0;
  auto first = SharedPointer<AdoptBase>::tryAdopt(new AdoptDerived(destroyed));
  ASSERT_TRUE(first);
  EXPECT_EQ(first->value(), 17);
  auto second = first;
  first.reset();
  EXPECT_EQ(destroyed, 0U);
  second.reset();
  EXPECT_EQ(destroyed, 1U);
}

TEST(SharedPointerAdopt, NullInputProducesEmptyOwnership) {
  auto owner = SharedPointer<AdoptBase>::tryAdopt(nullptr);
  EXPECT_FALSE(owner);
  EXPECT_EQ(owner.refcount(), 0U);
}

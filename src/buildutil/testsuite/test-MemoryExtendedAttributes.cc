/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/MemoryExtendedAttributes.h"
#include <gtest/gtest.h>

namespace {
using Status = XattrStatus;

StringView view(const std::string& name) {
  return StringView(name.data(), name.size());
}

std::vector<std::string> names(MemoryExtendedAttributes& attributes) {
  size_t required = 0;
  EXPECT_EQ(attributes.list(nullptr, 0, required), Status::Success);
  std::vector<char> bytes(required);
  EXPECT_EQ(attributes.list(bytes.data(), bytes.size(), required), Status::Success);
  std::vector<std::string> result;
  for (size_t offset = 0; offset < bytes.size();) {
    size_t end = offset;
    while (end < bytes.size() && bytes[end])
      ++end;
    EXPECT_LT(end, bytes.size());
    if (end == bytes.size())
      break;
    result.emplace_back(bytes.data() + offset, end - offset);
    offset = end + 1;
  }
  return result;
}

class AttributeObserver final : public FileEventObserver {
 public:
  void fileEvent(const FileEvent& event) override {
    ++count;
    masks |= event.mask;
  }
  size_t count = 0;
  FileEventMask masks = 0;
};
}  // namespace

TEST(MemoryExtendedAttributes, BinaryEmptyAndMaximumValuesAreSnapshots) {
  MemoryExtendedAttributes attributes;
  const StringView key("user.binary");
  std::vector<uint8_t> expected(Xattr::MaximumValueLength);
  for (size_t i = 0; i < expected.size(); ++i)
    expected[i] = static_cast<uint8_t>(i * 37);
  auto input = expected;
  ASSERT_EQ(attributes.set(key, input.data(), input.size(), Xattr::Create), Status::Success);
  input.assign(input.size(), 0xff);
  size_t required = 0;
  EXPECT_EQ(attributes.get(key, reinterpret_cast<void*>(1), 0, required), Status::Success);
  ASSERT_EQ(required, expected.size());
  std::vector<uint8_t> output(required);
  ASSERT_EQ(attributes.get(key, output.data(), output.size(), required), Status::Success);
  EXPECT_EQ(output, expected);
  EXPECT_EQ(attributes.set(StringView("user.empty"), nullptr, 0, 0), Status::Success);
  required = 17;
  EXPECT_EQ(attributes.get(StringView("user.empty"), nullptr, 1, required), Status::Success);
  EXPECT_EQ(required, 0U);
  EXPECT_EQ(names(attributes), (std::vector<std::string>{"user.binary", "user.empty"}));
}

TEST(MemoryExtendedAttributes, ShortBuffersAndInvalidInputPreserveState) {
  MemoryExtendedAttributes attributes;
  const StringView key("user.sample");
  const uint8_t input[] = {0, 0x81, 0, 0xff};
  ASSERT_EQ(attributes.set(key, input, sizeof(input), 0), Status::Success);
  std::array<uint8_t, 32> output;
  output.fill(0xa5);
  size_t required = 0;
  EXPECT_EQ(attributes.get(key, output.data(), 3, required), Status::Range);
  EXPECT_EQ(required, sizeof(input));
  EXPECT_EQ(attributes.list(output.data(), 3, required), Status::Range);
  EXPECT_EQ(required, key.length() + 1);
  for (auto byte : output)
    EXPECT_EQ(byte, 0xa5);
  EXPECT_EQ(attributes.set(key, input, sizeof(input), 4), Status::Invalid);
  EXPECT_EQ(attributes.set(key, nullptr, 1, 0), Status::Invalid);
  EXPECT_EQ(attributes.set(key, input, Xattr::MaximumValueLength + 1, 0), Status::Range);
  EXPECT_EQ(attributes.set(StringView("user."), input, 1, 0), Status::Invalid);
  EXPECT_EQ(attributes.set(StringView("security.policy"), input, 1, 0), Status::Unsupported);
  const char embedded[] = "user.ab\0cd";
  EXPECT_EQ(attributes.set(StringView(embedded, sizeof(embedded) - 1), input, 1, 0),
            Status::Invalid);
  std::string maximum = "user." + std::string(Xattr::MaximumNameLength - 5, 'n');
  EXPECT_EQ(attributes.set(view(maximum), nullptr, 0, 0), Status::Success);
  maximum += 'n';
  EXPECT_EQ(attributes.set(view(maximum), nullptr, 0, 0), Status::Range);
  ASSERT_EQ(attributes.get(key, output.data(), output.size(), required), Status::Success);
  EXPECT_EQ(std::vector<uint8_t>(output.begin(), output.begin() + required),
            std::vector<uint8_t>(input, input + sizeof(input)));
}

TEST(MemoryExtendedAttributes, ConditionalUpdatesAndRemovalAreAtomic) {
  MemoryExtendedAttributes::Quota quota;
  {
    MemoryExtendedAttributes attributes(quota);
    const StringView key("user.key");
    EXPECT_EQ(attributes.set(key, "a", 1, Xattr::Replace), Status::Missing);
    EXPECT_EQ(attributes.set(key, "a", 1, Xattr::Create | Xattr::Replace), Status::Missing);
    EXPECT_EQ(quota.used(), 0U);
    ASSERT_EQ(attributes.set(key, "a", 1, Xattr::Create), Status::Success);
    const size_t charge = quota.used();
    EXPECT_EQ(attributes.set(key, "b", 1, Xattr::Create), Status::Exists);
    EXPECT_EQ(attributes.set(key, "b", 1, Xattr::Create | Xattr::Replace), Status::Exists);
    EXPECT_EQ(quota.used(), charge);
    char value = 0;
    size_t required = 0;
    ASSERT_EQ(attributes.get(key, &value, 1, required), Status::Success);
    EXPECT_EQ(value, 'a');
    ASSERT_EQ(attributes.set(key, "b", 1, Xattr::Replace), Status::Success);
    ASSERT_EQ(attributes.get(key, &value, 1, required), Status::Success);
    EXPECT_EQ(value, 'b');
    EXPECT_EQ(attributes.remove(key), Status::Success);
    EXPECT_EQ(attributes.remove(key), Status::Missing);
    EXPECT_EQ(attributes.get(key, &value, 1, required), Status::Missing);
    EXPECT_EQ(quota.used(), 0U);
    EXPECT_TRUE(names(attributes).empty());
  }
  EXPECT_EQ(quota.used(), 0U);
}

TEST(MemoryExtendedAttributes, EntryCapacityAllowsReplacementAndReleasedSlots) {
  MemoryExtendedAttributes attributes;
  for (size_t i = 0; i < MemoryExtendedAttributes::MaximumEntries; ++i) {
    const std::string key = "user." + std::to_string(i);
    ASSERT_EQ(attributes.set(view(key), nullptr, 0, Xattr::Create), Status::Success);
  }
  EXPECT_EQ(names(attributes).size(), MemoryExtendedAttributes::MaximumEntries);
  EXPECT_EQ(attributes.set(StringView("user.extra"), nullptr, 0, 0), Status::NoSpace);
  EXPECT_EQ(attributes.set(StringView("user.17"), "x", 1, Xattr::Replace), Status::Success);
  EXPECT_EQ(attributes.remove(StringView("user.17")), Status::Success);
  EXPECT_EQ(attributes.set(StringView("user.extra"), nullptr, 0, 0), Status::Success);
  EXPECT_EQ(names(attributes).size(), MemoryExtendedAttributes::MaximumEntries);
}

TEST(MemoryExtendedAttributes, StoreByteLimitPreservesValuesAndReclaimsCredit) {
  MemoryExtendedAttributes::Quota quota;
  {
    MemoryExtendedAttributes attributes(quota);
    const std::vector<uint8_t> value(Xattr::MaximumValueLength, 0xc3);
    for (int i = 0; i < 3; ++i) {
      const std::string key = "user." + std::to_string(i);
      ASSERT_EQ(attributes.set(view(key), value.data(), value.size(), 0), Status::Success);
    }
    const size_t charge = quota.used();
    EXPECT_EQ(attributes.set(StringView("user.3"), value.data(), value.size(), 0), Status::NoSpace);
    EXPECT_EQ(quota.used(), charge);
    EXPECT_EQ(names(attributes).size(), 3U);
    EXPECT_EQ(attributes.set(StringView("user.1"), nullptr, 0, Xattr::Replace), Status::Success);
    EXPECT_EQ(attributes.set(StringView("user.3"), value.data(), value.size(), 0), Status::Success);
    size_t required = 0;
    EXPECT_EQ(attributes.get(StringView("user.0"), nullptr, 0, required), Status::Success);
    EXPECT_EQ(required, value.size());
  }
  EXPECT_EQ(quota.used(), 0U);
}

TEST(MemoryExtendedAttributes, SharedQuotaUsesNetReplacementAndReleasesDestruction) {
  constexpr size_t limit = 512;
  MemoryExtendedAttributes::Quota quota(limit);
  {
    MemoryExtendedAttributes first(quota), second(quota);
    std::vector<uint8_t> value(100, 0x75);
    ASSERT_EQ(first.set(StringView("user.a"), value.data(), value.size(), 0), Status::Success);
    const size_t firstCharge = quota.used(), overhead = firstCharge - value.size();
    ASSERT_LT(firstCharge + overhead, limit);
    std::vector<uint8_t> remaining(limit - firstCharge - overhead, 0x4a);
    ASSERT_EQ(second.set(StringView("user.b"), remaining.data(), remaining.size(), 0),
              Status::Success);
    ASSERT_EQ(quota.used(), limit);
    EXPECT_EQ(first.set(StringView("user.a"), value.data(), value.size(), Xattr::Replace),
              Status::Success);
    remaining.push_back(0);
    EXPECT_EQ(second.set(StringView("user.b"), remaining.data(), remaining.size(), Xattr::Replace),
              Status::NoSpace);
    EXPECT_EQ(quota.used(), limit);
    value.pop_back();
    ASSERT_EQ(first.set(StringView("user.a"), value.data(), value.size(), Xattr::Replace),
              Status::Success);
    EXPECT_EQ(second.set(StringView("user.b"), remaining.data(), remaining.size(), Xattr::Replace),
              Status::Success);
    EXPECT_EQ(quota.used(), limit);
    EXPECT_EQ(first.remove(StringView("user.a")), Status::Success);
    EXPECT_LT(quota.used(), limit);
  }
  EXPECT_EQ(quota.used(), 0U);
}

TEST(MemoryExtendedAttributes, ConcurrentCreateHasOneWinner) {
  MemoryExtendedAttributes attributes;
  std::atomic<unsigned> ready{0};
  std::atomic<bool> begin{false};
  Status results[2] = {Status::Invalid, Status::Invalid};
  auto create = [&](size_t index) {
    ready.fetch_add(1);
    while (!begin.load())
      std::this_thread::yield();
    results[index] = attributes.set(StringView("user.race"), &index, sizeof(index), Xattr::Create);
  };
  std::thread first(create, 0), second(create, 1);
  while (ready.load() != 2)
    std::this_thread::yield();
  begin.store(true);
  first.join();
  second.join();
  EXPECT_TRUE((results[0] == Status::Success && results[1] == Status::Exists) ||
              (results[1] == Status::Success && results[0] == Status::Exists));
  EXPECT_EQ(names(attributes), (std::vector<std::string>{"user.race"}));
}

TEST(MemoryExtendedAttributes, ConcurrentGetsNeverExposeMixedReplacementBytes) {
  MemoryExtendedAttributes attributes;
  const std::vector<uint8_t> first(4096, 0x29), second(4096, 0xc7);
  ASSERT_EQ(attributes.set(StringView("user.snapshot"), first.data(), first.size(), 0),
            Status::Success);
  std::atomic<bool> begin{false};
  std::atomic<unsigned> failures{0};
  std::thread writer([&] {
    while (!begin.load())
      std::this_thread::yield();
    for (size_t i = 0; i < 400; ++i) {
      const auto& value = i % 2 ? first : second;
      if (attributes.set(StringView("user.snapshot"), value.data(), value.size(), Xattr::Replace) !=
          Status::Success)
        ++failures;
    }
  });
  std::thread reader([&] {
    std::vector<uint8_t> value(first.size());
    begin.store(true);
    for (size_t i = 0; i < 400; ++i) {
      size_t required = 0;
      if (attributes.get(StringView("user.snapshot"), value.data(), value.size(), required) !=
              Status::Success ||
          required != value.size() || (value != first && value != second))
        ++failures;
    }
  });
  writer.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0U);
}

TEST(MemoryExtendedAttributes, FileForwardersUpdateOnlyCtimeAndOneAttributeEvent) {
  RamFs filesystem;
  RamFile regular(String("xattrs"), 1, &filesystem, nullptr);
  RamDir directory(String("xattrs-dir"), 2, &filesystem, nullptr);
  for (File* file : {static_cast<File*>(&regular), static_cast<File*>(&directory)}) {
    file->setAccessedTime(11);
    file->setModifiedTime(22);
    file->setCreationTime(33);
    auto* observed = new AttributeObserver;
    SharedPointer<FileEventObserver> observer(observed);
    FileEventSubscription subscription;
    ASSERT_TRUE(file->subscribeFileEvents(FileEvents::Attributes | FileEvents::Modify, observer,
                                          subscription));
    const auto before = Time::getTime();
    ASSERT_EQ(file->setExtendedAttribute(StringView("user.time"), "x", 1, Xattr::Create),
              Status::Success);
    const auto attributes = file->getAttributes();
    EXPECT_EQ(attributes.accessed, 11U);
    EXPECT_EQ(attributes.modified, 22U);
    EXPECT_GE(attributes.changed, before);
    EXPECT_LE(attributes.changed, Time::getTime());
    EXPECT_EQ(observed->count, 1U);
    EXPECT_EQ(observed->masks, FileEvents::Attributes);
    EXPECT_EQ(file->setExtendedAttribute(StringView("user.time"), "y", 1, Xattr::Create),
              Status::Exists);
    EXPECT_EQ(file->removeExtendedAttribute(StringView("user.missing")), Status::Missing);
    EXPECT_EQ(file->getAttributes().changed, attributes.changed);
    EXPECT_EQ(observed->count, 1U);
    ASSERT_EQ(file->removeExtendedAttribute(StringView("user.time")), Status::Success);
    EXPECT_EQ(observed->count, 2U);
    EXPECT_EQ(observed->masks, FileEvents::Attributes);
    subscription.reset();
  }
}

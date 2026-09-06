/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "modules/subsys/posix/fanotify-queue.h"
#include <gtest/gtest.h>

namespace {
using Take = FanotifyQueue::Take;
constexpr uint64_t Overflow = 0x4000;

FanotifyRecord record(uint32_t identity) {
  FanotifyRecord result;
  result.mountId = 17;
  result.producer = 91;
  result.handle.type = 0x50444731;
  result.handle.length = sizeof(identity);
  std::memcpy(result.handle.bytes, &identity, sizeof(identity));
  result.fsid.words[0] = 0x12345678;
  result.fsid.words[1] = 0x87654321;
  result.mask = 2;
  return result;
}

template <typename T>
T field(const uint8_t* bytes, size_t offset) {
  T value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

template <typename Predicate>
void requireEventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      ADD_FAILURE() << "FanotifyQueue worker exceeded its bounded completion deadline";
      // A timed-out worker can still refer to stack state; a hanging join is unsafe.
      std::abort();
    }
    std::this_thread::yield();
  }
}

class QueryingObserver final : public ReadinessObserver {
 public:
  explicit QueryingObserver(FanotifyQueue& queue) : queue(queue) {}

  void readinessChanged(ReadyMask mask) override {
    if (count < masks.size()) {
      masks[count] = mask;
      levels[count] = queue.queryReady();
      bytes[count] = queue.queuedMetadataBytes();
    }
    ++count;
  }

  FanotifyQueue& queue;
  std::array<ReadyMask, 4> masks{};
  std::array<ReadyMask, 4> levels{};
  std::array<int, 4> bytes{};
  size_t count = 0;
};
}  // namespace

TEST(FanotifyQueue, EncodesFidMetadataAndZeroPaddingWithinRecord) {
  auto input = record(7);
  input.handle.length = 5;
  const uint8_t handle[] = {0, 0x81, 0xff, 0x35, 0};
  std::memcpy(input.handle.bytes, handle, sizeof(handle));
  input.mask = 0x22;
  std::array<uint8_t, FanotifyQueue::MaximumRecordSize + 8> bytes;
  bytes.fill(0xa5);
  ASSERT_EQ(input.encodedSize(), 52U);
  input.encode(bytes.data());
  EXPECT_EQ(field<uint32_t>(bytes.data(), 0), 52U);
  EXPECT_EQ(bytes[4], 3);
  EXPECT_EQ(bytes[5], 0);
  EXPECT_EQ(field<uint16_t>(bytes.data(), 6), 24U);
  EXPECT_EQ(field<uint64_t>(bytes.data(), 8), input.mask);
  EXPECT_EQ(field<int32_t>(bytes.data(), 16), -1);
  EXPECT_EQ(field<int32_t>(bytes.data(), 20), 91);
  EXPECT_EQ(bytes[24], 1);
  EXPECT_EQ(bytes[25], 0);
  EXPECT_EQ(field<uint16_t>(bytes.data(), 26), 28U);
  EXPECT_EQ(field<uint32_t>(bytes.data(), 28), input.fsid.words[0]);
  EXPECT_EQ(field<uint32_t>(bytes.data(), 32), input.fsid.words[1]);
  EXPECT_EQ(field<uint32_t>(bytes.data(), 36), sizeof(handle));
  EXPECT_EQ(field<int32_t>(bytes.data(), 40), input.handle.type);
  EXPECT_EQ(std::memcmp(bytes.data() + 44, handle, sizeof(handle)), 0);
  for (size_t i = 49; i < 52; ++i)
    EXPECT_EQ(bytes[i], 0);
  for (size_t i = 52; i < bytes.size(); ++i)
    EXPECT_EQ(bytes[i], 0xa5);

  input.handle.length = sizeof(input.handle.bytes);
  for (size_t i = 0; i < input.handle.length; ++i)
    input.handle.bytes[i] = static_cast<uint8_t>(i * 37);
  ASSERT_EQ(input.encodedSize(), FanotifyQueue::MaximumRecordSize);
  input.encode(bytes.data());
  EXPECT_EQ(std::memcmp(bytes.data() + 44, input.handle.bytes, input.handle.length), 0);
  for (size_t i = FanotifyQueue::MaximumRecordSize; i < bytes.size(); ++i)
    EXPECT_EQ(bytes[i], 0xa5);
}

TEST(FanotifyQueue, MergesOnlyAdjacentEqualMountHandleAndProducer) {
  FanotifyQueue queue;
  ASSERT_TRUE(queue.valid());
  auto first = record(3);
  auto merged = first;
  merged.mask = 4;
  queue.enqueue(first);
  queue.enqueue(merged);
  queue.enqueue(record(4));
  queue.enqueue(first);
  EXPECT_EQ(queue.queuedMetadataBytes(), 72);
  FanotifyRecord output;
  ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
  EXPECT_EQ(output.mask, 6U);
  EXPECT_TRUE(output.sameTarget(first));
  ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
  EXPECT_TRUE(output.sameTarget(record(4)));
  ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
  EXPECT_EQ(output.mask, first.mask);

  std::array<FanotifyRecord, 5> distinct;
  distinct.fill(first);
  ++distinct[0].mountId;
  ++distinct[1].producer;
  ++distinct[2].handle.type;
  ++distinct[3].handle.length;
  ++distinct[4].handle.bytes[0];
  for (const auto& next : distinct) {
    queue.enqueue(first);
    queue.enqueue(next);
    ASSERT_EQ(queue.queuedMetadataBytes(), 48);
    ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
    EXPECT_TRUE(output.sameTarget(first));
    ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
    EXPECT_TRUE(output.sameTarget(next));
  }
}

TEST(FanotifyQueue, CapacityPreservesAcceptedEventsAndOneOverflowAcrossWraps) {
  FanotifyQueue queue;
  ASSERT_TRUE(queue.valid());
  for (uint32_t round = 0; round < 3; ++round) {
    const uint32_t base = round * 1000;
    for (size_t i = 0; i < FanotifyQueue::MaximumEvents; ++i)
      queue.enqueue(record(base + i));
    auto last = record(base + FanotifyQueue::MaximumEvents - 1);
    last.mask = 4;
    queue.enqueue(last);
    EXPECT_EQ(queue.queuedMetadataBytes(), 256 * 24);
    for (uint32_t i = 256; i < 356; ++i)
      queue.enqueue(record(base + i));
    EXPECT_EQ(queue.queuedMetadataBytes(), 257 * 24);

    FanotifyRecord output;
    for (size_t i = 0; i < FanotifyQueue::MaximumEvents; ++i) {
      ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
      EXPECT_TRUE(output.sameTarget(record(base + i)));
      EXPECT_EQ(output.mask, i == 255 ? 6U : 2U);
    }
    EXPECT_EQ(queue.take(output, 23, false), Take::TooSmall);
    EXPECT_EQ(queue.queuedMetadataBytes(), 24);
    ASSERT_EQ(queue.take(output, 24, false), Take::Ready);
    EXPECT_EQ(output.mask, Overflow);
    EXPECT_EQ(output.producer, 0U);
    EXPECT_EQ(output.encodedSize(), 24U);
    std::array<uint8_t, 32> wire;
    wire.fill(0x71);
    output.encode(wire.data());
    EXPECT_EQ(field<uint32_t>(wire.data(), 0), 24U);
    EXPECT_EQ(field<uint16_t>(wire.data(), 6), 24U);
    EXPECT_EQ(field<uint64_t>(wire.data(), 8), Overflow);
    EXPECT_EQ(field<int32_t>(wire.data(), 16), -1);
    EXPECT_EQ(field<int32_t>(wire.data(), 20), 0);
    for (size_t i = 24; i < wire.size(); ++i)
      EXPECT_EQ(wire[i], 0x71);
    EXPECT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Empty);
    EXPECT_EQ(queue.queryReady(), ReadyNone);
  }
}

TEST(FanotifyQueue, ShortTakeRetainsSnapshotAndSuccessfulTakeConsumesBeforeCopyout) {
  FanotifyQueue queue;
  ASSERT_TRUE(queue.valid());
  auto input = record(73);
  const auto expected = input;
  queue.enqueue(input);
  input.handle.bytes[0] = 0xff;
  input.fsid.words[0] = 0;
  input.mask = 4;
  auto output = record(99);
  const auto untouched = output;
  EXPECT_EQ(queue.take(output, expected.encodedSize() - 1, false), Take::TooSmall);
  EXPECT_TRUE(output.sameTarget(untouched));
  EXPECT_EQ(output.mask, untouched.mask);
  EXPECT_EQ(output.fsid.words[0], untouched.fsid.words[0]);
  EXPECT_EQ(output.fsid.words[1], untouched.fsid.words[1]);
  EXPECT_EQ(queue.queuedMetadataBytes(), 24);
  EXPECT_EQ(queue.queryReady(), ReadyRead);
  ASSERT_EQ(queue.take(output, expected.encodedSize(), false), Take::Ready);
  EXPECT_TRUE(output.sameTarget(expected));
  EXPECT_EQ(output.mask, expected.mask);
  EXPECT_EQ(output.fsid.words[0], expected.fsid.words[0]);
  // take transfers a detached snapshot; usercopy failure above this layer drops it.
  EXPECT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Empty);
  EXPECT_EQ(queue.queuedMetadataBytes(), 0);
  auto invalid = record(88);
  invalid.handle.length = sizeof(invalid.handle.bytes) + 1;
  queue.enqueue(invalid);
  EXPECT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Empty);
}

TEST(FanotifyQueue, ReadinessEdgesAndCloseCallbacksPermitQueueQueries) {
  FanotifyQueue queue;
  ASSERT_TRUE(queue.valid());
  auto* observer = new QueryingObserver(queue);
  SharedPointer<ReadinessObserver> retained(observer);
  ReadinessSubscription subscription;
  ASSERT_TRUE(queue.subscribeReadiness(ReadyRead, retained, subscription));
  EXPECT_EQ(queue.queryReady(), ReadyNone);
  const auto initial = queue.readinessGenerations();
  queue.enqueue(record(1));
  queue.enqueue(record(1));
  queue.enqueue(record(2));
  EXPECT_EQ(observer->count, 1U);
  EXPECT_EQ(observer->masks[0], ReadyRead);
  EXPECT_EQ(observer->levels[0], ReadyRead);
  EXPECT_EQ(observer->bytes[0], 24);
  EXPECT_EQ(queue.readinessGenerations().read, initial.read + 1);
  FanotifyRecord output;
  ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
  ASSERT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Ready);
  EXPECT_EQ(queue.queryReady(), ReadyNone);
  queue.enqueue(record(3));
  EXPECT_EQ(observer->count, 2U);
  EXPECT_EQ(queue.readinessGenerations().read, initial.read + 2);
  queue.close();
  queue.close();
  queue.enqueue(record(4));
  EXPECT_EQ(observer->count, 3U);
  EXPECT_EQ(observer->masks[2], ReadyInvalid | ReadyHangup);
  EXPECT_EQ(observer->levels[2], ReadyInvalid | ReadyHangup);
  EXPECT_EQ(observer->bytes[2], 0);
  EXPECT_EQ(queue.queryReady(), ReadyInvalid | ReadyHangup);
  EXPECT_EQ(queue.take(output, FanotifyQueue::MaximumRecordSize, false), Take::Closed);
  ReadinessSubscription rejected;
  EXPECT_FALSE(queue.subscribeReadiness(ReadyRead, retained, rejected));
  EXPECT_FALSE(static_cast<bool>(rejected));
  subscription.reset();
}

TEST(FanotifyQueue, ConcurrentProducersAndBlockingConsumerPreserveEachOrderedStream) {
  FanotifyQueue queue;
  ASSERT_TRUE(queue.valid());
  constexpr uint32_t Count = 512;
  std::array<std::atomic<uint32_t>, 2> consumed{};
  std::atomic<unsigned> finished{0};
  std::atomic<bool> correct{true};
  std::array<std::thread, 2> producers;
  for (uint32_t p = 0; p < producers.size(); ++p) {
    producers[p] = std::thread([&, p] {
      for (uint32_t i = 0; i < Count; ++i) {
        // Bound outstanding records below capacity without serializing either producer.
        requireEventually([&] { return i < consumed[p].load() + 16; });
        auto input = record(i);
        input.producer = p + 1;
        queue.enqueue(input);
      }
      ++finished;
    });
  }
  std::thread consumer([&] {
    for (size_t i = 0; i < Count * 2; ++i) {
      FanotifyRecord output;
      const auto status = queue.take(output, FanotifyQueue::MaximumRecordSize, true);
      if (status != Take::Ready || output.mask != 2 || output.producer < 1 || output.producer > 2 ||
          output.handle.length != sizeof(uint32_t)) {
        correct = false;
        break;
      }
      const uint32_t p = output.producer - 1;
      if (field<uint32_t>(output.handle.bytes, 0) != consumed[p].load())
        correct = false;
      ++consumed[p];
    }
    ++finished;
  });
  requireEventually([&] { return finished.load() == 3; });
  for (auto& producer : producers)
    producer.join();
  consumer.join();
  EXPECT_TRUE(correct.load());
  EXPECT_EQ(consumed[0].load(), Count);
  EXPECT_EQ(consumed[1].load(), Count);
  EXPECT_EQ(queue.queuedMetadataBytes(), 0);
}

TEST(FanotifyQueue, CloseRacingBlockingTakeReturnsClosedWithoutARecord) {
  // The native pthread shim has no waiter-enrollment hook; either race order is permitted.
  for (unsigned i = 0; i < 24; ++i) {
    FanotifyQueue queue;
    ASSERT_TRUE(queue.valid());
    std::atomic<bool> entered{false}, finished{false};
    auto output = record(55);
    Take result = Take::Empty;
    std::thread reader([&] {
      entered = true;
      result = queue.take(output, FanotifyQueue::MaximumRecordSize, true);
      finished = true;
    });
    requireEventually([&] { return entered.load(); });
    queue.close();
    requireEventually([&] { return finished.load(); });
    reader.join();
    EXPECT_EQ(result, Take::Closed);
    EXPECT_TRUE(output.sameTarget(record(55)));
  }
}

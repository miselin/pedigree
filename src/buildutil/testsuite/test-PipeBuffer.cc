/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "modules/system/vfs/PipeBuffer.h"
#include <condition_variable>
#include <gtest/gtest.h>

class PipeBufferTestPeer {
 public:
  static const void* pairWaiter(PipeBuffer& buffer) {
    PipeBuffer::lock(buffer.m_Lock);
    const void* enrolled = buffer.m_PairWaiters;
    buffer.m_Lock.release();
    return enrolled;
  }
  static bool hasPairWaiter(PipeBuffer& buffer) {
    return pairWaiter(buffer) != nullptr;
  }
  static bool closing(PipeBuffer& buffer) {
    PipeBuffer::lock(buffer.m_Lock);
    const bool closing = buffer.m_Closing;
    buffer.m_Lock.release();
    return closing;
  }
};

namespace {
using Status = PipeBuffer::Status;
constexpr size_t Capacity = PipeBuffer::Capacity;

class Gate {
 public:
  void open() {
    std::lock_guard<std::mutex> guard(m_Mutex);
    m_Open = true;
    m_Condition.notify_all();
  }
  bool wait() {
    std::unique_lock<std::mutex> guard(m_Mutex);
    return m_Condition.wait_for(guard, std::chrono::seconds(2), [this] { return m_Open; });
  }

 private:
  std::mutex m_Mutex;
  std::condition_variable m_Condition;
  bool m_Open = false;
};

template <typename Predicate>
void requireEventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      ADD_FAILURE() << "PipeBuffer worker did not reach its bounded synchronization point";
      // A stranded worker can still own stack state; abort instead of a hanging join.
      std::abort();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void finish(std::thread& worker, Gate& done) {
  if (!done.wait()) {
    ADD_FAILURE() << "PipeBuffer worker exceeded its bounded completion deadline";
    std::abort();
  }
  worker.join();
}

void countChanges(void* context) {
  ++*static_cast<size_t*>(context);
}

struct DrainingObserver {
  PipeBuffer* buffer = nullptr;
  std::vector<uint8_t> collected;
  bool inside = false;
  size_t publications = 0;

  static void changed(void* context) {
    auto& self = *static_cast<DrainingObserver*>(context);
    ++self.publications;
    if (self.inside) {
      return;
    }
    self.inside = true;
    // Both queries and read reacquire the engine mutex from the callback.
    if (self.buffer->getDataSize() && self.buffer->canRead(false)) {
      std::array<uint8_t, Capacity> bytes;
      const size_t count = self.buffer->read(bytes.data(), bytes.size(), false);
      self.collected.insert(self.collected.end(), bytes.begin(), bytes.begin() + count);
    }
    self.inside = false;
  }
};
}  // namespace

TEST(PipeBuffer, RingMatchesIndependentByteQueueAcrossWraps) {
  PipeBuffer buffer;
  std::deque<uint8_t> model;
  uint32_t state = 0x4157;
  for (size_t iteration = 0; iteration < 1500; ++iteration) {
    state = state * 1664525U + 1013904223U;
    const size_t count = (state >> 8) % 641;
    std::vector<uint8_t> bytes(count);
    for (size_t i = 0; i < count; ++i) {
      bytes[i] = static_cast<uint8_t>(state + i);
    }
    if ((state & 3) == 0) {
      const size_t accepted = count < model.size() ? count : model.size();
      ASSERT_EQ(buffer.read(bytes.data(), count, false), accepted);
      for (size_t i = 0; i < accepted; ++i) {
        ASSERT_EQ(bytes[i], model.front());
        model.pop_front();
      }
    } else {
      const size_t space = Capacity - model.size();
      const bool atomic = (state & 3) == 1;
      const size_t accepted =
          atomic ? (count <= space ? count : 0) : (count < space ? count : space);
      ASSERT_EQ(atomic ? buffer.writeAtomic(bytes.data(), count, false)
                       : buffer.write(bytes.data(), count, false),
                accepted);
      model.insert(model.end(), bytes.begin(), bytes.begin() + accepted);
    }
    ASSERT_EQ(buffer.getDataSize(), model.size());
  }
  std::array<uint8_t, Capacity> bytes;
  ASSERT_EQ(buffer.read(bytes.data(), bytes.size(), false), model.size());
  EXPECT_EQ(std::vector<uint8_t>(bytes.begin(), bytes.begin() + model.size()),
            std::vector<uint8_t>(model.begin(), model.end()));
}

TEST(PipeBuffer, AtomicWriteDoesNotPublishAnInsufficientPrefix) {
  PipeBuffer buffer;
  std::array<uint8_t, Capacity> original;
  original.fill(0x35);
  ASSERT_EQ(buffer.write(original.data(), Capacity - 1, false), Capacity - 1);
  const uint8_t pair[] = {0x81, 0x82};
  EXPECT_EQ(buffer.writeAtomic(pair, sizeof(pair), false), 0U);
  EXPECT_EQ(buffer.getDataSize(), Capacity - 1);
  std::array<uint8_t, Capacity> result;
  ASSERT_EQ(buffer.read(result.data(), result.size(), false), Capacity - 1);
  EXPECT_EQ(std::vector<uint8_t>(result.begin(), result.end() - 1),
            std::vector<uint8_t>(original.begin(), original.end() - 1));
}

TEST(PipeBuffer, HeadReservationPreservesSuffixAndExcludesOtherReaders) {
  PipeBuffer buffer;
  const uint8_t bytes[] = {1, 2, 3, 4, 5, 6};
  ASSERT_EQ(buffer.write(bytes, 4, false), 4U);
  PipeBuffer::ReadReservation head, contender;
  ASSERT_EQ(buffer.reserveRead(3, false, head).count, 3U);
  EXPECT_EQ(buffer.reserveRead(1, false, head).status, Status::Invalid);
  EXPECT_EQ(buffer.reserveRead(1, false, contender).status, Status::WouldBlock);
  uint8_t result[6] = {};
  EXPECT_EQ(buffer.read(result, 1, false), 0U);
  ASSERT_EQ(buffer.write(bytes + 4, 2, false), 2U);
  head.copyTo(result, 3);
  EXPECT_EQ(std::vector<uint8_t>(result, result + 3), (std::vector<uint8_t>{1, 2, 3}));
  head.consume(2);
  head.consume(0);
  ASSERT_EQ(buffer.read(result, sizeof(result), false), 4U);
  EXPECT_EQ(std::vector<uint8_t>(result, result + 4), (std::vector<uint8_t>{3, 4, 5, 6}));
}

TEST(PipeBuffer, AppendReservationAllowsDrainAndRejectsCompetingWriters) {
  PipeBuffer buffer;
  const uint8_t bytes[] = {1, 2, 3, 4};
  ASSERT_EQ(buffer.write(bytes, sizeof(bytes), false), sizeof(bytes));
  PipeBuffer::WriteReservation append, contender;
  ASSERT_EQ(buffer.reserveWrite(Capacity, false, append).count, Capacity - sizeof(bytes));
  EXPECT_EQ(buffer.reserveWrite(1, false, contender).status, Status::WouldBlock);
  EXPECT_EQ(buffer.write(bytes, 1, false), 0U);
  uint8_t result[4] = {};
  ASSERT_EQ(buffer.read(result, sizeof(result), false), sizeof(result));
  ASSERT_EQ(append.commit(bytes + 1, 2).count, 2U);
  ASSERT_EQ(buffer.read(result, sizeof(result), false), 2U);
  EXPECT_EQ(result[0], 2);
  EXPECT_EQ(result[1], 3);
  ASSERT_EQ(buffer.reserveWrite(1, false, append).status, Status::Ready);
  EXPECT_EQ(append.commit(bytes, 2).status, Status::Invalid);
  EXPECT_EQ(append.size(), 0U);
  EXPECT_TRUE(buffer.canWrite(false));
}

TEST(PipeBuffer, QuietEndpointsAndCancellationRestoreReadinessGenerations) {
  size_t changes = 0;
  PipeBuffer buffer(countChanges, &changes);
  buffer.disableReads();
  buffer.disableWrites();
  EXPECT_FALSE(buffer.enableReads());
  EXPECT_FALSE(buffer.enableWrites(true));
  buffer.wipe();
  EXPECT_EQ(changes, 0U);
  const uint64_t writable = buffer.writableGeneration();
  PipeBuffer::WriteReservation append;
  ASSERT_EQ(buffer.reserveWrite(3, false, append).status, Status::Ready);
  EXPECT_FALSE(buffer.canWrite(false));
  append.cancel();
  EXPECT_TRUE(buffer.canWrite(false));
  EXPECT_GT(buffer.writableGeneration(), writable);
  EXPECT_EQ(buffer.getDataSize(), 0U);
  EXPECT_EQ(changes, 2U);
  const uint8_t value = 8;
  ASSERT_EQ(buffer.write(&value, 1, false), 1U);
  const uint64_t readable = buffer.readableGeneration();
  PipeBuffer::ReadReservation head;
  ASSERT_EQ(buffer.reserveRead(1, false, head).status, Status::Ready);
  EXPECT_FALSE(buffer.canRead(false));
  head.cancel();
  EXPECT_TRUE(buffer.canRead(false));
  EXPECT_GT(buffer.readableGeneration(), readable);
  EXPECT_EQ(buffer.getDataSize(), 1U);
}

TEST(PipeBuffer, WriterReopenDefersResetUntilReservedPrefixIsConsumed) {
  PipeBuffer buffer;
  const uint8_t original[] = {3, 4, 5};
  ASSERT_EQ(buffer.write(original, sizeof(original), false), sizeof(original));
  PipeBuffer::ReadReservation head;
  ASSERT_EQ(buffer.reserveRead(3, false, head).count, 3U);
  buffer.disableWrites();
  EXPECT_FALSE(buffer.enableWrites(true));
  EXPECT_EQ(buffer.getDataSize(), 3U);
  EXPECT_FALSE(buffer.canWrite(false));
  EXPECT_FALSE(buffer.canRead(false));
  uint8_t snapshot[3];
  head.copyTo(snapshot, sizeof(snapshot));
  EXPECT_EQ(std::vector<uint8_t>(snapshot, snapshot + 3),
            std::vector<uint8_t>(original, original + 3));
  head.consume(1);
  EXPECT_EQ(buffer.getDataSize(), 0U);
  EXPECT_TRUE(buffer.canWrite(false));
  EXPECT_EQ(buffer.write(original, 1, false), 1U);
}

TEST(PipeBuffer, CloseAndResetRejectOldAppendWithoutRevokingAReadPrefix) {
  const uint8_t bytes[] = {7, 8, 9};
  PipeBuffer buffer;
  ASSERT_EQ(buffer.write(bytes, sizeof(bytes), false), sizeof(bytes));
  PipeBuffer::ReadReservation head;
  PipeBuffer::WriteReservation append;
  ASSERT_EQ(buffer.reserveRead(2, false, head).count, 2U);
  ASSERT_EQ(buffer.reserveWrite(2, false, append).count, 2U);
  buffer.close();
  EXPECT_EQ(append.commit(bytes, 2).status, Status::Closed);
  uint8_t snapshot[2];
  head.copyTo(snapshot, sizeof(snapshot));
  EXPECT_EQ(snapshot[0], 7);
  EXPECT_EQ(snapshot[1], 8);
  head.consume(1);
  EXPECT_EQ(buffer.getDataSize(), 2U);
  EXPECT_EQ(buffer.waitTransfer(false, false).status, Status::Eof);

  PipeBuffer reopened;
  ASSERT_EQ(reopened.reserveWrite(2, false, append).count, 2U);
  reopened.disableWrites();
  reopened.enableWrites(true);
  EXPECT_EQ(append.commit(bytes, 2).status, Status::Closed);
  EXPECT_EQ(reopened.getDataSize(), 0U);
  EXPECT_TRUE(reopened.canWrite(false));
}

TEST(PipeBuffer, PairTeeAndFailureLeaveSourceBytesIntact) {
  PipeBuffer input, output;
  const uint8_t bytes[] = {1, 2, 3, 4};
  ASSERT_EQ(input.write(bytes, sizeof(bytes), false), sizeof(bytes));
  EXPECT_EQ(input.transferTo(input, 2, true, false).status, Status::Invalid);
  EXPECT_EQ(input.transferTo(output, 3, false, false).count, 3U);
  EXPECT_EQ(input.getDataSize(), 4U);
  EXPECT_EQ(output.getDataSize(), 3U);
  PipeBuffer::WriteReservation held;
  ASSERT_EQ(output.reserveWrite(1, false, held).status, Status::Ready);
  EXPECT_EQ(input.transferTo(output, 2, true, false).status, Status::WouldBlock);
  EXPECT_EQ(input.getDataSize(), 4U);
  EXPECT_EQ(output.getDataSize(), 3U);
  held.cancel();
  ASSERT_EQ(input.transferTo(output, 2, true, false).count, 2U);
  uint8_t result[5];
  ASSERT_EQ(output.read(result, sizeof(result), false), sizeof(result));
  EXPECT_EQ(std::vector<uint8_t>(result, result + 5), (std::vector<uint8_t>{1, 2, 3, 1, 2}));
  ASSERT_EQ(input.read(result, sizeof(result), false), 2U);
  EXPECT_EQ(result[0], 3);
  EXPECT_EQ(result[1], 4);
}

TEST(PipeBuffer, PairWaitObservesDestinationCloseWhileSourceIsEmpty) {
  PipeBuffer input, output;
  Gate done;
  PipeBuffer::Result result{Status::Invalid, 0};
  std::thread worker([&] {
    result = input.transferTo(output, 1, true, true);
    done.open();
  });
  requireEventually([&] { return PipeBufferTestPeer::hasPairWaiter(input); });
  output.disableReads();
  finish(worker, done);
  EXPECT_EQ(result.status, Status::Closed);
  EXPECT_EQ(input.getDataSize(), 0U);
  EXPECT_EQ(output.getDataSize(), 0U);
}

TEST(PipeBuffer, PairWaitObservesSourceEofWhileDestinationIsFull) {
  PipeBuffer input, output;
  std::array<uint8_t, Capacity> bytes{};
  ASSERT_EQ(output.write(bytes.data(), bytes.size(), false), bytes.size());
  Gate done;
  PipeBuffer::Result result{Status::Invalid, 0};
  std::thread worker([&] {
    result = input.transferTo(output, 1, true, true);
    done.open();
  });
  requireEventually([&] { return PipeBufferTestPeer::hasPairWaiter(output); });
  input.disableWrites();
  finish(worker, done);
  EXPECT_EQ(result.status, Status::Eof);
  EXPECT_EQ(input.getDataSize(), 0U);
  EXPECT_EQ(output.getDataSize(), Capacity);
}

TEST(PipeBuffer, OppositePairWaitsReleaseBothLocksAndReservations) {
  PipeBuffer first, second;
  std::array<uint8_t, Capacity> bytes;
  bytes.fill(11);
  ASSERT_EQ(first.write(bytes.data(), bytes.size(), false), bytes.size());
  bytes.fill(22);
  ASSERT_EQ(second.write(bytes.data(), bytes.size(), false), bytes.size());
  Gate firstDone, secondDone;
  PipeBuffer::Result a{Status::Invalid, 0}, b{Status::Invalid, 0};
  std::thread firstWorker([&] {
    a = first.transferTo(second, 1, true, true);
    firstDone.open();
  });
  requireEventually([&] { return PipeBufferTestPeer::hasPairWaiter(first); });
  const void* firstWaiter = PipeBufferTestPeer::pairWaiter(first);
  std::thread secondWorker([&] {
    b = second.transferTo(first, 1, true, true);
    secondDone.open();
  });
  requireEventually([&] {
    const void* head = PipeBufferTestPeer::pairWaiter(first);
    return head && head != firstWaiter;
  });
  uint8_t discarded = 0;
  EXPECT_EQ(first.read(&discarded, 1, false), 1U);
  EXPECT_EQ(second.read(&discarded, 1, false), 1U);
  finish(firstWorker, firstDone);
  finish(secondWorker, secondDone);
  EXPECT_EQ(a.status, Status::Ready);
  EXPECT_EQ(b.status, Status::Ready);
  EXPECT_EQ(a.count, 1U);
  EXPECT_EQ(b.count, 1U);
  EXPECT_EQ(first.read(bytes.data(), bytes.size(), false), Capacity - 1);
  EXPECT_EQ(bytes[Capacity - 2], 22);
  EXPECT_EQ(second.read(bytes.data(), bytes.size(), false), Capacity - 1);
  EXPECT_EQ(bytes[Capacity - 2], 11);
}

TEST(PipeBuffer, CallbackCanRequeryAndDrainEachBlockingWriteChunk) {
  DrainingObserver observer;
  PipeBuffer buffer(DrainingObserver::changed, &observer);
  observer.buffer = &buffer;
  std::vector<uint8_t> bytes(Capacity * 2 + 23);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(i);
  }
  Gate done;
  size_t result = 0;
  std::thread writer([&] {
    result = buffer.write(bytes.data(), bytes.size(), true);
    done.open();
  });
  finish(writer, done);
  EXPECT_EQ(result, bytes.size());
  EXPECT_EQ(observer.collected, bytes);
  EXPECT_GE(observer.publications, 6U);
  EXPECT_EQ(buffer.getDataSize(), 0U);
}

TEST(PipeBuffer, DestructionDrainsReadTokenBeforeFreeingRingStorage) {
  PipeBuffer* buffer = new PipeBuffer;
  const uint8_t bytes[] = {31, 32};
  ASSERT_EQ(buffer->write(bytes, sizeof(bytes), false), sizeof(bytes));
  PipeBuffer::ReadReservation held;
  ASSERT_EQ(buffer->reserveRead(2, false, held).count, 2U);
  Gate done;
  std::thread closer([&] {
    delete buffer;
    done.open();
  });
  requireEventually([&] { return PipeBufferTestPeer::closing(*buffer); });
  uint8_t copied[2];
  held.copyTo(copied, sizeof(copied));
  EXPECT_EQ(copied[0], 31);
  EXPECT_EQ(copied[1], 32);
  held.consume(1);
  finish(closer, done);
  held.cancel();
}

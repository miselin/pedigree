// SPDX-License-Identifier: ISC
// Exercises the production batch loop with deterministic slot admission/completion.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

struct Disk {
  struct ReadBuffer {
    uint64_t location;
    void* buffer;
    size_t length;
    bool complete;
  };
  using WriteBuffer = ReadBuffer;
  static constexpr size_t MaxReadBuffers = 32;
  static constexpr size_t MaxWriteBuffers = 32;
};
struct TargetInfo {
  static constexpr size_t getPageSize() {
    return 4096;
  }
};
constexpr size_t MaxTransfer = 65536;
struct Mutex {
  void acquire() {
    assert(!held);
    held = true;
  }
  void release() {
    assert(held);
    held = false;
  }
  bool held = false;
};
template <class T>
struct LockGuard {
  explicit LockGuard(T& mutex) : mutex(mutex) {
    mutex.acquire();
  }
  ~LockGuard() {
    if (owned) {
      mutex.release();
    }
  }
  void disown() {
    owned = false;
  }
  T& mutex;
  bool owned = true;
};
struct TerminationDeferral {
  TerminationDeferral() {
    ++active;
  }
  ~TerminationDeferral() {
    --active;
  }
  static inline size_t active = 0;
};
namespace Time {
namespace Multiplier {
constexpr uint64_t Millisecond = 1000000;
constexpr uint64_t Second = 1000000000;
}  // namespace Multiplier
uint64_t ticks = 0;
std::function<void()> onDelay;
uint64_t getTicks() {
  return ticks;
}
void delay(uint64_t delta) {
  ticks += delta;
  if (onDelay)
    onDelay();
}
}  // namespace Time
class AhciPort {
 public:
  bool readBatch(Disk::ReadBuffer*, size_t, bool);
  bool writeBatch(Disk::WriteBuffer*, size_t, bool);
  bool transferBatch(Disk::ReadBuffer*, size_t, bool, bool);
  bool command(uint8_t, uint64_t, uint16_t, void*, size_t, bool, bool, bool = false);
  void waitForProgress() { Time::delay(Time::Multiplier::Millisecond); }
  bool chooseSlot(bool queued, size_t& index) {
    assert(m_CommandLock.held);
    index = 32;
    if (!online)
      return false;
    if (!queued && (external || owned)) {
      return true;
    }
    for (size_t i = 0; i < (queued ? m_QueueDepth : 1); ++i) {
      if (!(external & (1U << i)) && !(owned & (1U << i))) {
        index = i;
        break;
      }
    }
    return true;
  }
  bool issueCommand(size_t index, uint8_t opcode, uint64_t, uint16_t sectors, void*, size_t bytes,
                    bool writing, bool queued, bool) {
    assert(TerminationDeferral::active && m_CommandLock.held);
    assert(sectors * m_SectorBytes == bytes);
    assert(bytes <= MaxTransfer);
    transferSizes.push_back(bytes);
    assert(!(owned & (1U << index)) && !(external & (1U << index)));
    if (!queued) {
      assert(!owned && !external);
      if (opcode == 0xe7 || opcode == 0xea) {
        assert(!bytes && !writing);
        ++flushes;
      } else {
        assert(opcode == (writing ? 0x35 : 0x25));
        if (++sequential == failSequential) {
          return false;
        }
      }
      owned |= 1U << index;
      return true;
    }
    assert(opcode == (writing ? 0x61 : 0x60));
    if (issued == failIssue) {
      online = false;
      return false;
    }
    owned |= 1U << index;
    ++issued;
    const size_t active = __builtin_popcount(owned);
    if (active > maximumOwned)
      maximumOwned = active;
    return true;
  }
  bool reapCommand(size_t index, uint8_t opcode, void* buffer, size_t bytes, bool writing,
                   bool queued, bool, bool probe) {
    assert(TerminationDeferral::active && m_CommandLock.held == !queued && !probe);
    assert(owned & (1U << index));
    if (!queued) {
      owned &= ~(1U << index);
      if (opcode == 0xe7 || opcode == 0xea) {
        return !failFlush;
      }
      if (!writing) {
        std::memset(buffer, 0x6b, bytes);
      }
      return true;
    }
    assert(opcode == (writing ? 0x61 : 0x60));
    if (reaped == failReap)
      online = false;
    ++reaped;
    owned &= ~(1U << index);
    if (!owned)
      ++waves;
    if (online && !writing)
      std::memset(buffer, 0x6b, bytes);
    return online;
  }
  void delay() {
    assert(!owned && "waited for capacity while owning unreaped tags");
    assert(m_CommandLock.held);
    ++delays;
    if (releaseExternal)
      external = 0;
    else
      Time::ticks += 120 * Time::Multiplier::Second;
  }
  size_t m_SectorBytes = 512;
  size_t m_QueueDepth = 32;
  Mutex m_CommandLock;
  uint32_t external = 0;
  uint32_t owned = 0;
  bool online = true;
  bool m_WritesPending = true;
  bool failFlush = false;
  size_t flushes = 0;
  bool releaseExternal = true;
  size_t issued = 0, reaped = 0, maximumOwned = 0, waves = 0, delays = 0, sequential = 0;
  size_t failIssue = ~size_t(0), failReap = ~size_t(0), failSequential = ~size_t(0);
  std::vector<size_t> transferSizes;
};
#include "ahci-read-batch.inc"

static bool testWriting;
struct Fixture {
  explicit Fixture(size_t count = 32) : data(count * 4096), requests(count) {
    for (size_t i = 0; i < count; ++i)
      requests[i] = {i * 8192, data.data() + i * 4096, 4096, true};
    Time::ticks = 0;
    Time::onDelay = [this] { port.delay(); };
  }
  ~Fixture() {
    Time::onDelay = {};
    assert(!port.owned && !TerminationDeferral::active);
  }
  bool run() {
    return testWriting ? port.writeBatch(requests.data(), requests.size(), true)
                       : port.readBatch(requests.data(), requests.size(), true);
  }
  void complete() {
    for (const auto& request : requests) {
      assert(request.complete);
      for (size_t i = 0; i < request.length; ++i)
        assert(static_cast<uint8_t*>(request.buffer)[i] == (testWriting ? 0 : 0x6b));
    }
  }
  AhciPort port;
  std::vector<uint8_t> data;
  std::vector<Disk::ReadBuffer> requests;
};
void runCases() {
  for (size_t depth : {size_t{0}, size_t{1}, size_t{32}}) {
    Fixture f;
    f.port.m_QueueDepth = depth;
    for (size_t i = 0; i < f.requests.size(); ++i) {
      f.requests[i].location = i * 4096;
    }
    assert(f.run());
    f.complete();
    assert(f.port.transferSizes.size() == (testWriting ? 32 : 8));
    for (size_t bytes : f.port.transferSizes) {
      assert(bytes == (testWriting ? 4096 : 16384));
    }
  }
  {
    Fixture f(3);
    for (size_t i = 0; i < f.requests.size(); ++i) {
      f.requests[i].location = i * 4096;
    }
    std::swap(f.requests[1].buffer, f.requests[2].buffer);
    assert(f.run());
    f.complete();
    assert(f.port.transferSizes.size() == 3);
  }
  {
    Fixture f;
    assert(f.run());
    f.complete();
    assert(f.port.maximumOwned == 32 && f.port.waves == 1);
  }
  {
    Fixture f;
    f.port.external = ~uint32_t(0);
    assert(f.run());
    f.complete();
    assert(f.port.delays == 1 && f.port.reaped == 32);
  }
  {
    Fixture f(7);
    f.port.m_QueueDepth = 4;
    assert(f.run());
    f.complete();
    assert(f.port.maximumOwned == 4 && f.port.waves == 2 && !f.port.delays);
  }
  {
    Fixture f(7);
    f.port.m_QueueDepth = 4;
    f.port.external = 7;
    assert(f.run());
    f.complete();
    assert(f.port.maximumOwned == 1 && f.port.waves == 7 && !f.port.delays);
  }
  {
    Fixture f;
    f.port.external = ~uint32_t(0);
    f.port.releaseExternal = false;
    assert(!f.run());
    assert(!f.port.issued && !f.port.reaped);
  }
  {
    Fixture f;
    f.port.failIssue = 5;
    assert(!f.run());
    assert(f.port.issued == 5 && f.port.reaped == 5);
    for (const auto& r : f.requests)
      assert(!r.complete);
  }
  {
    Fixture f;
    f.port.failReap = 3;
    assert(!f.run());
    assert(f.port.issued == 32 && f.port.reaped == 32);
    for (size_t i = 0; i < 32; ++i)
      assert(f.requests[i].complete == (i < 3));
  }
  {
    Fixture f(7);
    f.port.m_QueueDepth = 0;
    assert(f.run());
    f.complete();
    assert(f.port.sequential == 7 && !f.port.issued);
  }
  {
    Fixture f(7);
    f.port.m_QueueDepth = 0;
    f.port.failSequential = 3;
    assert(!f.run());
    assert(f.port.sequential == 3);
    for (size_t i = 0; i < 7; ++i)
      assert(f.requests[i].complete == (i < 2));
  }
  {
    Fixture f(3);
    f.port.m_SectorBytes = 4096;
    assert(f.run());
    f.complete();
  }
  {
    Fixture f(3);
    f.requests.back().length = 512;
    assert(f.run());
    f.complete();
  }
  {
    Fixture f;
    f.requests.back().location = 1;
    assert(!f.run());
    assert(!f.port.issued);
    for (const auto& r : f.requests)
      assert(!r.complete);
  }
  {
    Fixture f;
    f.requests.back().length = 0;
    assert(!f.run());
    assert(!f.port.issued);
  }
  {
    Fixture f;
    assert(!f.port.readBatch(nullptr, 1, true));
    assert(!f.port.readBatch(f.requests.data(), 33, true));
    assert(f.port.readBatch(nullptr, 0, true));
  }
}

int main() {
  runCases();
  for (size_t depth : {size_t{0}, size_t{32}}) {
    Fixture f;
    f.port.m_QueueDepth = depth;
    for (size_t i = 0; i < f.requests.size(); ++i) {
      f.requests[i].location = i * 4096;
    }
    f.port.failReap = 1;
    f.port.failSequential = 2;
    assert(!f.run());
    if (depth) {
      assert(f.port.issued == 8 && f.port.reaped == 8);
    } else {
      assert(f.port.sequential == 2);
    }
    for (size_t i = 0; i < f.requests.size(); ++i) {
      assert(f.requests[i].complete == (i < 4));
    }
  }
  {
    Fixture f(5);
    for (size_t i = 0; i < f.requests.size(); ++i) {
      f.requests[i].location = i * 4096;
    }
    f.requests.back().length = 512;
    assert(f.run());
    f.complete();
    assert((f.port.transferSizes == std::vector<size_t>{16384, 512}));
  }
  {
    Fixture f(2);
    f.requests[0].location = (uint64_t{1} << 48) * 512 - 4096;
    f.requests[1].location = f.requests[0].location + 4096;
    assert(!f.run());
    assert(!f.port.issued);
    assert(!f.requests[0].complete && !f.requests[1].complete);
  }
  testWriting = true;
  runCases();
  for (size_t depth : {size_t{0}, size_t{32}}) {
    Fixture f(3);
    f.port.m_QueueDepth = depth;
    auto flush = [&] { return f.port.command(0xea, 0, 0, nullptr, 0, false, true); };
    // Start conservatively, including any writes predating driver ownership.
    assert(flush() && f.port.flushes == 1);
    assert(flush() && f.port.flushes == 1);
    assert(f.port.command(0x35, 0, 8, f.data.data(), 4096, true, true));
    f.port.failFlush = true;
    assert(!flush() && f.port.flushes == 2);
    assert(!flush() && f.port.flushes == 3);
    f.port.failFlush = false;
    assert(flush() && f.port.flushes == 4);
    assert(flush() && f.port.flushes == 4);
    assert(f.run());
    // Another queued writer still owns a tag. The barrier must wait for its
    // owner to drain, holding the gate against later submissions until reaped.
    f.port.external = 1;
    assert(flush() && f.port.flushes == 5 && f.port.delays == 1);
    assert(flush() && f.port.flushes == 5);
    assert(f.port.command(0x25, 0, 8, f.data.data(), 4096, false, true));
    assert(flush() && f.port.flushes == 5);
    f.port.online = false;
    assert(!flush() && f.port.flushes == 5);
  }
}

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
  static constexpr size_t MaxReadBuffers = 32;
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
    mutex.release();
  }
  T& mutex;
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
  void waitForProgress() { Time::delay(Time::Multiplier::Millisecond); }
  bool chooseSlot(bool queued, size_t& index) {
    assert(m_CommandLock.held && queued);
    index = 32;
    if (!online)
      return false;
    for (size_t i = 0; i < m_QueueDepth; ++i) {
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
    assert(opcode == 0x60 && !writing && queued && sectors * m_SectorBytes == bytes);
    assert(!(owned & (1U << index)) && !(external & (1U << index)));
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
    assert(TerminationDeferral::active && !m_CommandLock.held);
    assert(opcode == 0x60 && !writing && queued && !probe);
    assert(owned & (1U << index));
    if (reaped == failReap)
      online = false;
    ++reaped;
    owned &= ~(1U << index);
    if (!owned)
      ++waves;
    if (online)
      std::memset(buffer, 0x6b, bytes);
    return online;
  }
  bool command(uint8_t opcode, uint64_t, uint16_t sectors, void* buffer, size_t bytes, bool writing,
               bool) {
    assert(TerminationDeferral::active && !m_CommandLock.held);
    assert(!m_QueueDepth && opcode == 0x25 && !writing && sectors * m_SectorBytes == bytes);
    ++sequential;
    if (sequential == failSequential)
      return false;
    std::memset(buffer, 0x6b, bytes);
    return true;
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
  bool releaseExternal = true;
  size_t issued = 0, reaped = 0, maximumOwned = 0, waves = 0, delays = 0, sequential = 0;
  size_t failIssue = ~size_t(0), failReap = ~size_t(0), failSequential = ~size_t(0);
};
#include "ahci-read-batch.inc"

struct Fixture {
  explicit Fixture(size_t count = 32) : data(count * 4096), requests(count) {
    for (size_t i = 0; i < count; ++i)
      requests[i] = {i * 4096, data.data() + i * 4096, 4096, true};
    Time::ticks = 0;
    Time::onDelay = [this] { port.delay(); };
  }
  ~Fixture() {
    Time::onDelay = {};
    assert(!port.owned && !TerminationDeferral::active);
  }
  bool run() {
    return port.readBatch(requests.data(), requests.size(), true);
  }
  void complete() {
    for (const auto& request : requests) {
      assert(request.complete);
      for (size_t i = 0; i < request.length; ++i)
        assert(static_cast<uint8_t*>(request.buffer)[i] == 0x6b);
    }
  }
  AhciPort port;
  std::vector<uint8_t> data;
  std::vector<Disk::ReadBuffer> requests;
};
int main() {
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

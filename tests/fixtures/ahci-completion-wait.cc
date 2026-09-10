// SPDX-License-Identifier: ISC
// Production polling/reaping against a register model with no delivered IRQs.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>

#include "ahci-registers.inc"
using namespace Ahci;
#define FENCE() \
  do {          \
  } while (false)
#define ERROR(...) \
  do {             \
  } while (false)
#define MemoryCopy std::memcpy
[[noreturn]] void panic(const char*) {
  throw std::runtime_error("DMA still running");
}
struct Thread {
  bool deferred = true;
  bool eventsDeferred() const {
    return deferred;
  }
};
Thread current;
namespace Time {
namespace Multiplier {
constexpr uint64_t Millisecond = 1000000;
constexpr uint64_t Second = 1000000000;
}  // namespace Multiplier
uint64_t ticks = 0;
size_t delays = 0;
std::function<void()> progress;
uint64_t getTicks() {
  return ticks;
}
void step() {
  ticks += Multiplier::Millisecond;
  if (progress)
    progress();
}
void delay(uint64_t) {
  assert(!current.deferred);
  ++delays;
  step();
}
}  // namespace Time
struct Processor {
  struct Information {
    Thread* getCurrentThread() {
      return present ? &current : nullptr;
    }
  };
  static inline bool present = true, enabled = true;
  static inline size_t pauses = 0;
  static Information& information() {
    static Information i;
    return i;
  }
  static bool getInterrupts() {
    return enabled;
  }
  static void pause() {
    ++pauses;
    Time::step();
  }
};
struct Scheduler {
  static inline size_t yields = 0;
  static Scheduler& instance() {
    static Scheduler s;
    return s;
  }
  void yield() {
    assert(Processor::present && Processor::enabled);
    ++yields;
    Time::step();
  }
};
struct Semaphore {
  size_t count = 0, waits = 0;
  uint64_t lastSeconds = 0;
  void release() {
    ++count;
  }
  bool acquireForCompletion(size_t, size_t seconds, size_t usecs) {
    assert(!current.deferred && Processor::enabled);
    ++waits;
    lastSeconds = seconds;
    if (count) {
      --count;
      return true;
    }
    Time::ticks += seconds * Time::Multiplier::Second + usecs * 1000;
    if (Time::progress)
      Time::progress();
    return false;
  }
};
struct Mutex {
  bool held = false;
};
template <class T>
struct LockGuard {
  T& lock;
  explicit LockGuard(T& l) : lock(l) {
    assert(!lock.held);
    lock.held = true;
  }
  ~LockGuard() {
    lock.held = false;
  }
};
struct Region {
  void* pointer;
  void* virtualAddress() {
    return pointer;
  }
};
class AhciPort {
 public:
  struct Slot {
    std::array<unsigned char, 512> bytes{};
    Region data{bytes.data()};
    Semaphore completion;
    bool done = false;
    uint32_t errors = 0;
    uint64_t deadline = 30 * Time::Multiplier::Second;
  };
  AhciPort() {
    registers[Ssts] = 3;
    registers[PortIe] = PortInterrupts;
    registers[Cmd] = Start | FisEnable | CommandRunning | FisRunning;
    registers[Sact] = registers[Ci] = 3;
    for (auto& s : m_Slots)
      s.bytes.fill(0xa7);
  }
  uint32_t read(size_t reg) const {
    return registers[reg];
  }
  void write(size_t reg, uint32_t value) {
    if (reg == PortIs || reg == Serr) {
      if (reg == PortIs && onAcknowledge)
        onAcknowledge();
      registers[reg] &= ~value;
    } else if (reg == Cmd) {
      // Running bits are hardware-owned, cleared only by the progress callback.
      registers[reg] = (value & ~(CommandRunning | FisRunning)) |
                       (registers[reg] & (CommandRunning | FisRunning));
      if (!(value & Start) && !(value & FisEnable)) {
        assert(!(registers[reg] & CommandRunning));
        assert(m_Active);  // No caller buffer may be released before DMA stops.
      }
    } else
      registers[reg] = value;
  }
  void waitForProgress();
  bool waitClear(size_t, uint32_t, size_t);
  bool stopEngines();
  void acknowledge(uint32_t);
  void observe(uint32_t, bool);
  void pollCompletions(bool);
  bool interrupt(bool);
  bool reapCommand(size_t, uint8_t, void*, size_t, bool, bool, bool, bool);
  void completeHardware(uint32_t mask) {
    registers[Sact] &= ~mask;
    registers[Ci] &= ~mask;
    registers[PortIs] |= 1U << 3;
  }
  void stopHardware() {
    if (!(registers[Cmd] & Start))
      registers[Cmd] &= ~CommandRunning;
    if (!(registers[Cmd] & FisEnable))
      registers[Cmd] &= ~FisRunning;
  }
  std::function<void()> onAcknowledge;
  std::array<uint32_t, 128> registers{};
  std::array<CommandHeader, 2> headers{};
  Region m_Control{headers.data()};
  Slot m_Slots[2];
  Mutex m_StateLock;
  bool m_Online = true, m_PolledInterrupt = false;
  uint32_t m_Active = 3, m_Queued = 3;
  size_t m_SlotCount = 2, m_Outstanding = 2, m_InterruptCompletions = 0;
};
#include "ahci-completion-wait.inc"

void reset() {
  current.deferred = true;
  Processor::present = Processor::enabled = true;
  Processor::pauses = Scheduler::yields = Time::delays = 0;
  Time::ticks = 0;
  Time::progress = {};
}
int main() {
  reset();
  {
    AhciPort p;
    std::array<unsigned char, 512> output{};
    Time::progress = [&] {
      if (Scheduler::yields == 3)
        p.completeHardware(3);
    };
    assert(p.reapCommand(0, 0x60, output.data(), output.size(), false, true, true, false));
    assert(output[0] == 0xa7 && Scheduler::yields == 3 && !Time::delays);
    assert(!p.m_Slots[0].completion.waits && !p.m_Outstanding && p.m_Active == 2);
    assert(p.interrupt(false));  // Polling preserves credit for the IRQ worker.
    assert(p.reapCommand(1, 0x60, output.data(), output.size(), false, true, true, false));
    assert(!p.m_Active && !p.m_Queued);
  }
  reset();
  {
    AhciPort p;
    p.completeHardware(2);
    p.onAcknowledge = [&] {
      // The other tag finishes after observe sampled pending, before W1C.
      p.completeHardware(1);
    };
    assert(p.reapCommand(0, 0x60, nullptr, 0, false, true, true, false));
    assert(Scheduler::yields == 1 && !p.m_Slots[0].completion.waits);
    assert(!p.registers[PortIs] && p.m_Slots[1].done);
  }
  reset();
  {
    current.deferred = false;
    AhciPort p;
    p.completeHardware(3);
    assert(p.reapCommand(0, 0x60, nullptr, 0, false, true, true, false));
    assert(!p.m_Slots[0].completion.waits);  // Check hardware before sleeping.
  }
  reset();
  {
    AhciPort p;
    std::array<unsigned char, 512> output{};
    Time::progress = [&] { p.stopHardware(); };
    assert(!p.reapCommand(0, 0x60, output.data(), output.size(), false, true, true, false));
    assert(Time::ticks >= 30 * Time::Multiplier::Second && !p.m_Online);
    assert(!(p.registers[Cmd] & (CommandRunning | FisRunning)));
    assert(!p.m_Outstanding && p.m_Active == 2 && output[0] == 0);
    assert(p.m_Slots[1].done && p.m_Slots[1].errors);
    assert(!p.reapCommand(1, 0x60, nullptr, 0, false, true, true, false));
    assert(!p.m_Active && !Time::delays && !p.m_Slots[0].completion.waits);
  }
  reset();
  {
    AhciPort p;
    p.m_Slots[0].deadline = 0;
    bool refused = false;
    try {
      p.reapCommand(0, 0x60, nullptr, 0, false, true, true, false);
    } catch (const std::runtime_error&) {
      refused = true;
    }
    assert(refused && p.m_Active == 3 && (p.registers[Cmd] & CommandRunning));
  }
  reset();
  {
    AhciPort p;
    current.deferred = false;
    Time::progress = [&] {
      p.completeHardware(3);
      p.interrupt(true);
    };
    assert(p.reapCommand(0, 0xec, nullptr, 0, false, true, true, true));
    assert(p.m_Slots[0].completion.waits == 1 && p.m_Slots[0].completion.lastSeconds == 1);
    assert(p.m_InterruptCompletions == 2);  // Probe cannot be satisfied by initial polling.
  }
  reset();
  {
    AhciPort p;
    Processor::enabled = false;
    p.waitForProgress();
    Processor::enabled = true;
    Processor::present = false;
    p.waitForProgress();
    assert(Processor::pauses == 2 && !Scheduler::yields && !Time::delays);
  }
}

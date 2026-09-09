/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#define THREADS 1
#define HOSTED 0
#define EXPORTED_PUBLIC
#define Hex std::hex
#define NOTICE(x)     \
  do {                \
    if (false)        \
      std::cout << x; \
  } while (0)
#define ERROR(x) NOTICE(x)
#define TRACE(x) \
  do {           \
  } while (0)
#define FATAL(x)   \
  do {             \
    assert(false); \
  } while (0)
using irq_id_t = unsigned;
using String = std::string;

static std::vector<std::string> lifecycle;

template <class T>
class Atomic {
 public:
  explicit Atomic(T v = {}) : current(v) {}
  T value() const {
    return current;
  }
  Atomic& operator=(T v) {
    current = v;
    return *this;
  }
  Atomic& operator+=(T v) {
    current += v;
    return *this;
  }

 private:
  T current;
};
class Thread {};
struct ProcessorInformation {
  Thread* getCurrentThread() {
    static Thread t;
    return &t;
  }
};
struct Processor {
  static ProcessorInformation& information() {
    static ProcessorInformation p;
    return p;
  }
  static bool inDeviceHardIrq() {
    return false;
  }
  static bool getInterrupts() {
    return true;
  }
  static void pause() {}
};
class Scheduler {
 public:
  static Scheduler& instance() {
    static Scheduler s;
    return s;
  }
  void yield() {
    ++yields;
    if (onYield)
      onYield();
  }
  size_t yields = 0;
  std::function<void()> onYield;
};
class Mutex {
 public:
  bool acquire() {
    assert(!locked);
    locked = true;
    return true;
  }
  void release() {
    assert(locked);
    locked = false;
  }

 private:
  bool locked = false;
};
template <class T>
class LockGuard {
 public:
  explicit LockGuard(T& l) : lock(l) {
    assert(lock.acquire());
  }
  ~LockGuard() {
    lock.release();
  }

 private:
  T& lock;
};
template <class T>
class Buffer {
 public:
  explicit Buffer(size_t n) : capacity(n) {}
  void wipe() {
    data.clear();
  }
  void enableWrites() {
    enabled = true;
  }
  void disableWrites() {
    enabled = false;
    lifecycle.push_back("close");
  }
  size_t writeAvailable(const T* bytes, size_t count) {
    ++availableCalls;
    assert(enabled);
    const size_t n = std::min(count, capacity - data.size());
    data.insert(data.end(), bytes, bytes + n);
    return n;
  }
  size_t write(const T*, size_t, bool) {
    assert(false && "PS/2 delivery must wait for the buffer mutex, not drop on contention");
    return 0;
  }
  size_t read(T* bytes, size_t count, bool) {
    size_t n = 0;
    while (n < count && !data.empty()) {
      bytes[n++] = data.front();
      data.pop_front();
    }
    return n;
  }
  std::deque<T> data;
  size_t capacity;
  size_t availableCalls = 0;
  bool enabled = true;
};
class IoBase {
 public:
  std::deque<std::pair<uint8_t, bool>> input;
  std::vector<std::pair<uint8_t, size_t>> writes;
  uint8_t read8(size_t offset = 0) {
    if (offset == 4)
      return input.empty() ? 0 : 1 | (input.front().second ? 0x20 : 0);
    assert(offset == 0 && !input.empty());
    uint8_t value = input.front().first;
    input.pop_front();
    return value;
  }
  void write8(uint8_t value, size_t offset) {
    writes.emplace_back(value, offset);
  }
};
struct Address {
  IoBase* m_Io;
};
class Controller {
 public:
  Controller() = default;
  explicit Controller(Controller*) {}
  virtual ~Controller() = default;
  std::vector<Address*>& addresses() {
    return addressList;
  }
  std::vector<Address*> addressList;
};
enum class IrqDisposition { NotHandled, Handled, Quiesced };
class IrqHandlerBase {
 public:
  virtual ~IrqHandlerBase() = default;
};
class IrqHandler : public IrqHandlerBase {
 public:
  virtual IrqDisposition irq(irq_id_t) = 0;
};
struct IrqPolicy {
  static IrqPolicy edgeThreaded() {
    return {};
  }
};
class IrqManager {
 public:
  enum ControlCode { MitigationThreshold };
  std::map<irq_id_t, IrqHandler*> handlers;
  std::map<irq_id_t, bool> enabled;
  std::function<void(irq_id_t)> beforeUnregister;
  irq_id_t failRegistration = 0;
  irq_id_t registerIsaIrqHandler(uint8_t line, IrqHandler* handler, const IrqPolicy&) {
    lifecycle.push_back("register" + std::to_string(line));
    if (line == failRegistration)
      return 0;
    assert(handlers.emplace(line, handler).second);
    return line;
  }
  bool unregisterHandler(irq_id_t id, IrqHandlerBase* handler) {
    assert(handlers.at(id) == handler);
    assert(!enabled[1] && !enabled[12]);
    if (beforeUnregister)
      beforeUnregister(id);
    handlers.erase(id);
    lifecycle.push_back("unregister" + std::to_string(id));
    return true;
  }
  void control(irq_id_t, ControlCode, size_t) {}
  void enable(irq_id_t id, bool active) {
    enabled[id] = active;
  }
  IrqDisposition dispatch(irq_id_t id) {
    return handlers.at(id)->irq(id);
  }
};
class Machine {
 public:
  static Machine& instance() {
    static Machine m;
    return m;
  }
  IrqManager* getIrqManager() {
    return &manager;
  }
  IrqManager manager;
};

#define private public
#include "Ps2Controller.h"
#undef private
#include "Ps2Controller.cc"

int main(int argc, char** argv) {
  assert(argc == 2);
  const auto is = [&](const char* name) { return std::strcmp(argv[1], name) == 0; };
  IoBase io;
  Ps2Controller controller;
  controller.m_pBase = &io;
  controller.m_bHasSecondPort = !is("first-port-only");
  auto& manager = Machine::instance().manager;
  if (is("partial-init"))
    manager.failRegistration = 12;
  if (is("first-init-failure"))
    manager.failRegistration = 1;
  const bool initialised = controller.initialise3();
  if (is("partial-init") || is("first-init-failure")) {
    assert(!initialised);
    assert(manager.handlers.empty());
    assert(controller.readsStopping());
    if (is("partial-init")) {
      assert((lifecycle == std::vector<std::string>{"register1", "register12", "unregister1",
                                                    "close", "close"}));
    } else {
      assert((lifecycle == std::vector<std::string>{"register1", "close", "close"}));
    }
    std::cout << argv[1] << ": PASS\n";
    return 0;
  }
  assert(initialised);
  assert(manager.handlers.size() == (is("first-port-only") ? 1 : 2));
  if (is("first-port-only")) {
    controller.uninitialise();
    assert((lifecycle == std::vector<std::string>{"register1", "unregister1", "close", "close"}));
    std::cout << argv[1] << ": PASS\n";
    return 0;
  }
  controller.setIrqEnable(true, true);
  if (is("routing")) {
    io.input = {{0x12, false}, {0x81, true}, {0x92, false}, {0x82, true}};
    assert(manager.dispatch(12) == IrqDisposition::Handled);
    assert(io.input.empty());
    assert((controller.m_FirstPortBuffer.data == std::deque<uint8_t>{0x12, 0x92}));
    assert((controller.m_SecondPortBuffer.data == std::deque<uint8_t>{0x81, 0x82}));
    assert(controller.m_FirstPortBuffer.availableCalls == 2);
    assert(controller.m_SecondPortBuffer.availableCalls == 2);
    assert(manager.dispatch(1) == IrqDisposition::Handled);
    assert(manager.dispatch(12) == IrqDisposition::Handled);
    controller.setDebugState(true);
    io.input = {{0x12, false}};
    assert(manager.dispatch(1) == IrqDisposition::Handled);
    assert(io.input.size() == 1);
    controller.setDebugState(false);
    io.input.clear();
    for (size_t i = 0; i < 600; ++i)
      io.input.emplace_back(static_cast<uint8_t>(i), false);
    assert(manager.dispatch(1) == IrqDisposition::Handled);
    assert(io.input.empty());
    assert(controller.m_FirstPortBuffer.data.size() == 602);
    assert(Scheduler::instance().yields >= 2);
  } else if (is("controller-busy")) {
    assert(controller.m_IoGate.tryAcquire());
    Scheduler::instance().onYield = [&] { controller.m_IoGate.release(); };
    io.input = {{0x12, false}, {0x92, false}};
    assert(manager.dispatch(1) == IrqDisposition::Handled);
    Scheduler::instance().onYield = {};
    assert(Scheduler::instance().yields == 1);
    assert(io.input.empty());
    assert((controller.m_FirstPortBuffer.data == std::deque<uint8_t>{0x12, 0x92}));
  } else if (is("buffer-full")) {
    controller.m_SecondPortBuffer.capacity = 1;
    io.input = {{0x81, true}, {0x82, true}, {0x12, false}, {0x92, false}};
    assert(manager.dispatch(12) == IrqDisposition::Handled);
    assert(io.input.empty());
    assert((controller.m_FirstPortBuffer.data == std::deque<uint8_t>{0x12, 0x92}));
    assert((controller.m_SecondPortBuffer.data == std::deque<uint8_t>{0x81}));
  } else if (is("shutdown")) {
    manager.beforeUnregister = [&](irq_id_t id) {
      assert(controller.readsStopping());
      assert(controller.m_FirstPortBuffer.enabled && controller.m_SecondPortBuffer.enabled);
      assert(io.writes.back().second == 0 && !(io.writes.back().first & 3));
      assert(manager.dispatch(id) == IrqDisposition::Quiesced);
    };
  } else {
    assert(false);
  }
  controller.uninitialise();
  assert(manager.handlers.empty());
  assert(controller.readsStopping());
  uint8_t value = 0;
  assert(!controller.readFirstPort(value, false));
  assert(!controller.readSecondPort(value, false));
  assert((std::vector<std::string>(lifecycle.end() - 4, lifecycle.end()) ==
          std::vector<std::string>{"unregister1", "unregister12", "close", "close"}));
  std::cout << argv[1] << ": PASS\n";
}

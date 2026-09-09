/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <utility>
#include <vector>

#define DEBUGGER 0
#define MEMORY_TRACING 0
#define MACHINE_X86_PS2CONTROLLER_H
#define NOTICE(x) \
  do {            \
  } while (0)
#define ERROR(x) \
  do {           \
  } while (0)
#define DEBUG_LOG(x) \
  do {               \
  } while (0)
#define FATAL(x)   \
  do {             \
    assert(false); \
  } while (0)
using irq_id_t = unsigned;

class Process {};
class Thread {
 public:
  enum State { Continue, Exit };
  Thread() = default;
  Thread(Process*, int (*)(void*), void*) {}
  Process* getParent() {
    return nullptr;
  }
  void setName(const char*) {}
  State getUnwindState() {
    return Exit;
  }
};
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
};
class OwnedThread {
 public:
  explicit operator bool() const {
    return false;
  }
  void stop() {}
  void adopt(Thread*) {}
};
class Mutex {
 public:
  void acquire() {
    assert(!locked);
    locked = true;
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
  explicit LockGuard(T& lock) : m_Lock(lock) {
    m_Lock.acquire();
  }
  ~LockGuard() {
    m_Lock.release();
  }

 private:
  T& m_Lock;
};
class Keyboard {
 public:
  virtual ~Keyboard() = default;
  enum { CapsLock = 4, NumLock = 2, ScrollLock = 1 };
  static constexpr uint64_t Special = 1ULL << 63;
};
class KeymapManager {
 public:
  enum EscapeState { EscapeNone };
  static KeymapManager& instance() {
    static KeymapManager m;
    return m;
  }
  uint8_t convertPc102ScancodeToHidKeycode(uint8_t b, EscapeState&) {
    switch (b & 0x7f) {
      case 0x12:
        return 0x08;
      case 0x2a:
        return 0xe1;
      case 0x3a:
        return 0x39;
      case 0x45:
        return 0x53;
      case 0x46:
        return 0x47;
      default:
        assert(false);
        return 0;
    }
  }
  bool handleHidModifier(uint8_t, bool) {
    return false;
  }
  uint64_t resolveHidKeycode(uint8_t) {
    return 'e';
  }
};
class InputManager {
 public:
  static InputManager& instance() {
    static InputManager i;
    return i;
  }
  void machineKeyUpdate(uint8_t, bool) {}
};
using KeyEvent = std::pair<uint8_t, bool>;
class HidInputManager {
 public:
  std::vector<KeyEvent> events;
  static HidInputManager& instance() {
    static HidInputManager h;
    return h;
  }
  void keyDown(uint8_t k) {
    events.emplace_back(k, true);
  }
  void keyUp(uint8_t k) {
    events.emplace_back(k, false);
  }
};
class Ps2Controller {
 public:
  std::deque<uint8_t> input;
  std::vector<uint8_t> writes;
  size_t reads = 0;
  bool autoAck = false;
  bool readFirstPort(uint8_t& b, bool = true) {
    ++reads;
    if (input.empty())
      return false;
    b = input.front();
    input.pop_front();
    return true;
  }
  void writeFirstPort(uint8_t b) {
    writes.push_back(b);
    if (autoAck)
      input.push_back(0xfa);
  }
  void setIrqEnable(bool, bool) {}
  void setDebugState(bool) {}
  bool getDebugState() {
    return false;
  }
  uint8_t readByte() {
    return 0;
  }
  uint8_t readByteNonBlock() {
    return 0;
  }
};

// Stop the actual reader when the finite input queue empties.
#define private public
#include "Keyboard.h"
#undef private
#include KEYBOARD_SOURCE

static void feed(X86Keyboard& keyboard, Ps2Controller& controller,
                 std::initializer_list<uint8_t> bytes) {
  controller.input.insert(controller.input.end(), bytes.begin(), bytes.end());
  keyboard.readerThread();
  assert(controller.input.empty());
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const auto is = [&](const char* name) { return std::strcmp(argv[1], name) == 0; };
  Ps2Controller controller;
  X86Keyboard keyboard(&controller);
  auto& events = HidInputManager::instance().events;
  const std::vector<KeyEvent> shiftedE = {{0xe1, true}, {0x08, true}, {0x08, false}, {0xe1, false}};

  if (is("release-interleaving")) {
    controller.autoAck = true;
    feed(keyboard, controller, {0x12, 0xba, 0x92});
    assert((events == std::vector<KeyEvent>{{8, true}, {0x39, false}, {8, false}}));
    assert((controller.writes == std::vector<uint8_t>{0xed, 4}));
  } else if (is("ack-sequencing")) {
    keyboard.setLedState(4);
    assert(controller.reads == 0);
    assert((controller.writes == std::vector<uint8_t>{0xed}));
    feed(keyboard, controller, {0x2a, 0x12, 0xfa, 0x92, 0xaa});
    assert(events == shiftedE);
    assert((controller.writes == std::vector<uint8_t>{0xed, 4}));
    feed(keyboard, controller, {0xfa, 0xfa, 0xfe});
    assert((controller.writes == std::vector<uint8_t>{0xed, 4}));
    assert(keyboard.getLedState() == 4);
  } else if (is("resend")) {
    keyboard.setLedState(2);
    feed(keyboard, controller, {0xfe, 0xfe, 0xfa, 0xfe, 0xfe, 0xfa});
    assert((controller.writes == std::vector<uint8_t>{0xed, 0xed, 0xed, 2, 2, 2}));
    feed(keyboard, controller, {0x2a, 0x12, 0x92, 0xaa});
    assert(events == shiftedE);
  } else if (is("command-exhaustion") || is("data-exhaustion")) {
    keyboard.setLedState(2);
    const bool dataPhase = is("data-exhaustion");
    if (dataPhase)
      feed(keyboard, controller, {0xfa});
    feed(keyboard, controller, {0xfe, 0xfe, 0xfe, 0xfe, 0xfe});
    assert(controller.writes.size() == (dataPhase ? 5 : 4));
    for (size_t i = dataPhase ? 1 : 0; i < controller.writes.size(); ++i)
      assert(controller.writes[i] == (dataPhase ? 2 : 0xed));
    feed(keyboard, controller, {0x2a, 0x12, 0x92, 0xaa});
    assert(events == shiftedE);
    controller.writes.clear();
    keyboard.setLedState(1);
    feed(keyboard, controller, {0xfa, 0xfa});
    assert((controller.writes == std::vector<uint8_t>{0xed, 1}));
  } else if (is("coalescing")) {
    keyboard.setLedState(1);
    keyboard.setLedState(2);
    keyboard.setLedState(4);
    assert((controller.writes == std::vector<uint8_t>{0xed}));
    feed(keyboard, controller, {0xfa});
    assert((controller.writes == std::vector<uint8_t>{0xed, 4}));
    keyboard.setLedState(2);
    keyboard.setLedState(3);
    feed(keyboard, controller, {0xfe});
    assert((controller.writes == std::vector<uint8_t>{0xed, 4, 4}));
    feed(keyboard, controller, {0xfa, 0xfa, 0xfa});
    assert((controller.writes == std::vector<uint8_t>{0xed, 4, 4, 0xed, 3}));
    assert(keyboard.getLedState() == 3);
  } else if (is("missing-ack")) {
    keyboard.setLedState(4);
    for (size_t i = 0; i < 1024; ++i) {
      events.clear();
      feed(keyboard, controller, {0x2a, 0x12, 0x92, 0xaa});
      assert(events == shiftedE);
    }
    assert((controller.writes == std::vector<uint8_t>{0xed}));
  } else if (is("lock-toggles")) {
    feed(keyboard, controller, {0xba, 0xc5, 0xc6, 0xba});
    assert(keyboard.getLedState() == 3);
    feed(keyboard, controller, {0xfa, 0xfa});
    assert((controller.writes == std::vector<uint8_t>{0xed, 3}));
  } else {
    assert(false);
  }
  std::cout << argv[1] << ": PASS\n";
}

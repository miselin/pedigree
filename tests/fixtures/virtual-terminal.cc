/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define THREADS 1
#define NOTICE(...)
#define WARNING(...)
#define ERROR(...)
#define F_NOTICE(...)
#define SYSCALL_ERROR(...)
#define ByteSet std::memset

#include "vt-abi.inc"

struct String : std::string {
  using std::string::string;
  void Format(const char* format, size_t number) {
    char text[32];
    std::snprintf(text, sizeof(text), format, static_cast<unsigned>(number));
    assign(text);
  }
};
struct Mutex {
  bool held = false;
};
template <class T>
struct LockGuard {
  explicit LockGuard(T& value) : lock(value) {
    assert(!lock.held);
    lock.held = true;
  }
  ~LockGuard() {
    lock.held = false;
  }
  T& lock;
};
struct File {
  explicit File(const String& value) : name(value) {}
  virtual ~File() = default;
  const String& getName() const {
    return name;
  }
  String name;
};
class DevFsDirectory {
 public:
  ~DevFsDirectory() {
    for (File* file : files) {
      delete file;
    }
  }
  void addEntry(const String&, File* file) {
    files.push_back(file);
  }
  File* find(const char* name) {
    for (File* file : files) {
      if (file->name == name) {
        return file;
      }
    }
    return nullptr;
  }
  std::vector<File*> files;
};
struct DevFs;
struct TextIO : File {
  enum InputMode { Standard, Raw };
  TextIO(const String& name, size_t, DevFs*, DevFsDirectory*) : File(name) {}
  bool initialise(bool) {
    return true;
  }
  void markPrimary() {
    primary = true;
    ++presentations;
  }
  void unmarkPrimary() {
    primary = false;
  }
  void writeStr(const char*, size_t) {}
  void setMode(InputMode value) {
    inputMode = value;
  }
  InputMode getMode() const {
    return inputMode;
  }
  bool primary = false;
  unsigned presentations = 0;
  InputMode inputMode = Standard;
};
struct ConsoleFile : File {
  ConsoleFile(size_t number, const String& name) : File(name), number(number) {}
  size_t getPhysicalConsoleNumber() const {
    return number;
  }
  size_t number;
};
struct ConsolePhysicalFile : ConsoleFile {
  ConsolePhysicalFile(size_t number, TextIO*, const String& name, DevFs*)
      : ConsoleFile(number, name) {}
};
struct Vga {
  void setLargestTextMode() {
    ++textModeRestores;
  }
  unsigned textModeRestores = 0;
};
struct Machine {
  static Machine& instance() {
    static Machine machine;
    return machine;
  }
  unsigned getNumVga() const {
    return 1;
  }
  Vga* getVga(unsigned) {
    return &vga;
  }
  Vga vga;
};
class Process;
struct Thread {
  Process* getParent() const {
    return process;
  }
  size_t getId() const {
    return id;
  }
  bool acceptingEvents() const {
    return accepting;
  }
  Process* process = nullptr;
  size_t id = 0;
  bool accepting = true;
};
struct SignalDelivery {
  Thread* thread;
  int signal;
  bool yield, processDirected;
};
struct PosixSubsystem {
  static constexpr size_t MaximumSupportedSignal = 64;
  void sendSignal(Thread* thread, int signal, bool yield, bool processDirected) {
    deliveries.push_back({thread, signal, yield, processDirected});
  }
  std::vector<SignalDelivery> deliveries;
};
class Process {
 public:
  enum State { Active, Suspended, Terminating };
  struct ThreadLease {
    Thread* get() const {
      return thread;
    }
    Thread* operator->() const {
      return thread;
    }
    Thread* thread = nullptr;
  };
  size_t getId() const {
    return id;
  }
  State getState() const {
    return state;
  }
  PosixSubsystem* getSubsystem() const {
    return subsystem;
  }
  bool acquireThreadById(ThreadLease& lease, size_t id) {
    for (Thread* thread : threads) {
      if (thread->id == id) {
        lease.thread = thread;
        return true;
      }
    }
    return false;
  }
  size_t id = 42;
  State state = Active;
  PosixSubsystem* subsystem = nullptr;
  std::vector<Thread*> threads;
};
struct Scheduler {
  struct ProcessLease {
    Process* operator->() const {
      return process;
    }
    Process* process = nullptr;
  };
  static Scheduler& instance() {
    static Scheduler scheduler;
    return scheduler;
  }
  bool acquireProcessById(ProcessLease& lease, size_t id) {
    auto process = processes.find(id);
    if (process == processes.end()) {
      return false;
    }
    lease.process = process->second;
    return true;
  }
  std::map<size_t, Process*> processes;
};
struct Processor {
  static Processor& information() {
    static Processor processor;
    return processor;
  }
  Thread* getCurrentThread() const {
    return current;
  }
  Thread* current = nullptr;
};

#include "virtual-terminal-header.inc"

struct DevFs {
  size_t getNextInode() {
    return ++inode;
  }
  void revertInode() {
    --inode;
  }
  VirtualTerminalManager& getTerminalManager() {
    return *manager;
  }
  size_t inode = 0;
  VirtualTerminalManager* manager = nullptr;
};
DevFs* g_pDevFs = nullptr;

#include "virtual-terminal.inc"

struct ConsoleManager {
  static ConsoleManager& instance() {
    static ConsoleManager manager;
    return manager;
  }
  bool isConsole(File* file) const {
    return dynamic_cast<ConsoleFile*>(file);
  }
};
struct Descriptor {
  File* getFile() const {
    return file;
  }
  File* file;
};
template <class T>
int copyIoctlResult(void* destination, const T& value) {
  assert(destination);
  std::memcpy(destination, &value, sizeof(value));
  return 0;
}
template <class T>
bool copyIoctlInput(const void* source, T& value) {
  if (!source) {
    return false;
  }
  std::memcpy(&value, source, sizeof(value));
  return true;
}
int vtIoctl(File* file, size_t command, void* buf) {
  Descriptor descriptor{file};
  Descriptor* f = &descriptor;
#include "vt-ioctl.inc"
}

int main() {
  DevFs device;
  g_pDevFs = &device;
  DevFsDirectory directory;
  PosixSubsystem subsystem;
  Process controller;
  controller.subsystem = &subsystem;
  Thread render{&controller, 7}, worker{&controller, 8};
  controller.threads = {&render, &worker};
  Processor::information().current = &render;
  Scheduler::instance().processes[controller.id] = &controller;
  VirtualTerminalManager manager(&directory);
  device.manager = &manager;
  assert(manager.initialise());
  for (size_t n = 0; n < 8; ++n) {
    const std::string name = "tty" + std::to_string(n + 1);
    auto* file = dynamic_cast<ConsoleFile*>(directory.find(name.c_str()));
    assert(file && file->getPhysicalConsoleNumber() == n);
  }
  File* tty1 = directory.find("tty1");
  File* tty2 = directory.find("tty2");
  assert(manager.getCurrentTerminal()->primary);
  assert(!manager.activate(MAX_VT) && !manager.activate(8));
  assert(manager.getState().v_active == 1);
  assert(vtIoctl(tty2, 0x4b3a, reinterpret_cast<void*>(1)) == 0);
  assert(manager.getSystemMode(1) == VirtualTerminalManager::Graphics);
  assert(manager.getSystemMode(0) == VirtualTerminalManager::Text);
  assert(manager.getCurrentTerminal()->primary);
  int mode = -1;
  assert(vtIoctl(tty2, 0x4b3b, &mode) == 0 && mode == 1);
  assert(vtIoctl(tty1, 0x4b3b, &mode) == 0 && mode == 0);
  assert(vtIoctl(tty2, 0x4b3a, reinterpret_cast<void*>(2)) == -1);
  assert(vtIoctl(tty2, 0x4b3a, nullptr) == 0);

  vt_mode owned = {VT_PROCESS, 0, 10, 12, 0};
  assert(vtIoctl(tty1, 0x5602, &owned) == 0);
  vt_mode observed = {};
  assert(vtIoctl(tty1, 0x5601, &observed) == 0 && observed.mode == VT_PROCESS);
  assert(vtIoctl(tty2, 0x5601, &observed) == 0 && observed.mode == VT_AUTO);
  assert(vtIoctl(tty1, 0x4b3a, reinterpret_cast<void*>(1)) == 0);
  assert(!manager.getCurrentTerminal()->primary);
  assert(manager.activate(1) && manager.getState().v_active == 1);
  assert(subsystem.deliveries.size() == 1);
  auto release = subsystem.deliveries.back();
  assert(release.thread == &render && release.signal == owned.relsig);
  assert(!release.yield && !release.processDirected);
  assert(!manager.activate(2));
  assert(vtIoctl(tty2, 0x5605, reinterpret_cast<void*>(1)) == -1);
  Process stranger;
  stranger.id = 99;
  Thread strangerThread{&stranger, 0};
  Processor::information().current = &strangerThread;
  assert(!manager.reportPermission(0, VirtualTerminalManager::Allowed));
  Processor::information().current = &render;
  assert(vtIoctl(tty1, 0x5605, nullptr) == 0);
  assert(manager.getState().v_active == 1);
  assert(!manager.reportPermission(0, VirtualTerminalManager::Allowed));
  assert(manager.activate(1));
  assert(vtIoctl(tty1, 0x5605, reinterpret_cast<void*>(1)) == 0);
  assert(manager.getState().v_active == 2 && manager.getCurrentTerminal()->primary);
  assert(!manager.reportPermission(1, VirtualTerminalManager::Allowed));
  assert(manager.activate(0) && manager.getState().v_active == 1);
  assert(!manager.getCurrentTerminal()->primary);
  auto acquire = subsystem.deliveries.back();
  assert(acquire.thread == &render && acquire.signal == owned.acqsig);
  assert(!acquire.yield && !acquire.processDirected);
  assert(vtIoctl(tty1, 0x5605, reinterpret_cast<void*>(VT_ACKACQ)) == 0);
  assert(vtIoctl(tty2, 0x5605, reinterpret_cast<void*>(VT_ACKACQ)) == -1);
  assert(vtIoctl(tty1, 0x5605, reinterpret_cast<void*>(3)) == -1);
  assert(manager.activate(1));
  assert(manager.reportPermission(0, VirtualTerminalManager::Allowed));
  assert(manager.getState().v_active == 2);
  assert(manager.activate(0));
  assert(manager.activate(1) && manager.getState().v_active == 1);
  render.accepting = false;
  assert(manager.activate(2) && manager.getState().v_active == 3);
  assert(manager.getTerminalMode(0).mode == VT_AUTO);
  assert(manager.getSystemMode(0) == VirtualTerminalManager::Text);
  render.accepting = true;
  assert(manager.setTerminalMode(0, owned));
  manager.setSystemMode(0, VirtualTerminalManager::Graphics);
  Scheduler::instance().processes.erase(controller.id);
  assert(manager.activate(0) && manager.getCurrentTerminal()->primary);
  assert(manager.getTerminalMode(0).mode == VT_AUTO);
  assert(manager.getSystemMode(0) == VirtualTerminalManager::Text);
  assert(manager.activate(1));
  manager.lockSwitching(true);
  assert(!manager.activate(2));
  manager.lockSwitching(false);
  assert(manager.activate(2));
  assert(vtIoctl(tty1, 0x5606, nullptr) == -1);
  assert(vtIoctl(tty1, 0x5606, reinterpret_cast<void*>(MAX_VT + 1)) == -1);
  assert(vtIoctl(tty1, 0x5606, reinterpret_cast<void*>(3)) == 0);
  ConsoleFile pty(~size_t(0), String("pty"));
  assert(vtIoctl(&pty, 0x5601, &observed) == -1);
  File other(String("other"));
  assert(vtIoctl(&other, 0x4b3b, &mode) == -1);
  int inactive = -1;
  assert(vtIoctl(tty1, 0x5600, &inactive) == 0 && inactive == 9);
  auto* tty9 = dynamic_cast<ConsoleFile*>(directory.find("tty9"));
  assert(tty9 && tty9->getPhysicalConsoleNumber() == 8);
  assert(manager.activate(8));
  for (size_t n = 9; n < MAX_VT; ++n) {
    assert(manager.openInactive() == n);
  }
  assert(vtIoctl(tty1, 0x5600, &inactive) == 0 && inactive == -1);
}

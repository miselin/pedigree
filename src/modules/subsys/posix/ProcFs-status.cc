/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LatencyAccounting.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/StaticString.h"

#include "DevFs-block.h"
#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "ProcFs.h"

namespace {
constexpr uint64_t ClockTicksPerSecond = 100;
constexpr uint64_t NanosecondsPerClockTick = Time::Multiplier::Second / ClockTicksPerSecond;

class GeneratedFile : public File {
 public:
  GeneratedFile(const String& name, uintptr_t inode, ProcFs& filesystem, File* parent)
      : File(name, 0, 0, 0, inode, &filesystem, 0, parent) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    String contents;
    if (!generate(contents) || location >= contents.length())
      return 0;
    if (size > contents.length() - location)
      size = contents.length() - location;
    MemoryCopy(reinterpret_cast<void*>(buffer), contents.cstr() + location, size);
    return size;
  }

  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    return 0;
  }

  size_t getSize() override {
    String contents;
    return generate(contents) ? contents.length() : 0;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }
  virtual bool generate(String& contents) const = 0;
};

struct TaskCounts {
  size_t total = 0;
  size_t runnable = 0;
  size_t lastPid = 0;
};

TaskCounts taskCounts() {
  TaskCounts result;
  Scheduler& scheduler = Scheduler::instance();
  const size_t processCount = scheduler.getNumProcesses();
  for (size_t index = 0; index < processCount; ++index) {
    Scheduler::ProcessLease process;
    if (!scheduler.acquireProcess(process, index)) {
      continue;
    }
    if (process->getType() == Process::Posix && process->getUserspaceId() > result.lastPid) {
      result.lastPid = process->getUserspaceId();
    }
    const size_t threadCount = process->getNumThreads();
    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
      Process::ThreadLease thread;
      if (!process->acquireThread(thread, threadIndex)) {
        continue;
      }
      const auto* owner = thread->getScheduler();
      if (owner && owner->isIdleThread(thread.get())) {
        continue;
      }
      ++result.total;
      const Thread::Status status = thread->getStatus();
      if (status == Thread::Ready || status == Thread::Running) {
        ++result.runnable;
      }
    }
  }
  return result;
}

class KernelThreadsFile final : public GeneratedFile {
 public:
  KernelThreadsFile(uintptr_t inode, ProcFs& filesystem, File* parent)
      : GeneratedFile(String("threads"), inode, filesystem, parent) {}

 private:
  bool generate(String& contents) const override {
    // These are internal process/task identities, not the separate POSIX PID namespace.
    contents = String("process_id task_id cpu state priority user_ns system_ns idle name\n");
    Scheduler& scheduler = Scheduler::instance();
    const size_t processCount = scheduler.getNumProcesses();
    for (size_t index = 0; index < processCount; ++index) {
      Scheduler::ProcessLease process;
      if (!scheduler.acquireProcess(process, index) || process->getType() == Process::Posix) {
        continue;
      }
      const size_t threadCount = process->getNumThreads();
      for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        Process::ThreadLease thread;
        if (!process->acquireThread(thread, threadIndex)) {
          continue;
        }
        const auto* owner = thread->getScheduler();
        const auto status = thread->getStatus();
        const char state = status == Thread::Zombie                               ? 'Z'
                           : status == Thread::Ready || status == Thread::Running ? 'R'
                                                                                  : 'S';
        // Keep one line per thread even when a diagnostic name contains whitespace.
        char name[128];
        const String& description = thread->getName();
        const size_t length = min(description.length(), sizeof(name) - 1);
        for (size_t i = 0; i < length; ++i) {
          const char value = description[i];
          name[i] = value < ' ' || value == 127 ? ' ' : value;
        }
        name[length] = 0;
        String line;
        // Advancing counters must not shift later offsets in a partial read.
        line.Format("%20lu %20lu %20lu %c %20lu %20lu %20lu %u ", process->getId(),
                    thread->getTaskId(), owner ? owner->logicalCpu() : 0, state,
                    thread->getPriority(), thread->getUserTime(), thread->getKernelTime(),
                    owner && owner->isIdleThread(thread.get()) ? 1U : 0U);
        contents += line;
        contents += String(length ? name : "unnamed");
        contents += String("\n");
      }
    }
    return true;
  }
};

class LatencyFile final : public GeneratedFile {
 public:
  LatencyFile(uintptr_t inode, ProcFs& filesystem, File* parent)
      : GeneratedFile(String("latency"), inode, filesystem, parent) {
    // Origin addresses are useful for symbolizing a long IRQ-off interval.
    setPermissionsOnly(FILE_UR);
  }

 private:
  bool generate(String& contents) const override {
    contents.Format("enabled %u\n", unsigned(PEDIGREE_LATENCY_ACCOUNTING));
    contents += String(
        "cpu sample_ns valid online_since_ns irq_off_ns irq_off_count irq_off_max_ns "
        "irq_off_max_end_ns irq_off_max_site irq_off_ge_1ms irq_off_ge_10ms "
        "irq_off_open_since_ns irq_off_open_site event_deferred_cpu_ns "
        "event_deferred_thread_ns event_deferred_count event_deferred_max_ns\n");
    for (size_t cpu = 0; cpu < Processor::getCount(); ++cpu) {
      LatencyAccounting::Snapshot snapshot;
      const bool valid = LatencyAccounting::snapshot(cpu, snapshot);
      String line;
      line.Format("%20lu %20lu %u", cpu, Time::getTicks(), unsigned(valid));
      contents += line;
      for (size_t field = 0; field < LatencyAccounting::Count; ++field) {
        line.Format(" %20lu", snapshot.values[field]);
        contents += line;
      }
      contents += String("\n");
    }
    return true;
  }
};

class LoadAverageFile final : public GeneratedFile {
 public:
  LoadAverageFile(uintptr_t inode, ProcFs& filesystem, File* parent)
      : GeneratedFile(String("loadavg"), inode, filesystem, parent) {}

 private:
  bool generate(String& contents) const override {
    const auto activity = Scheduler::instance().systemActivity();
    const TaskCounts tasks = taskCounts();
    uint64_t whole[3], fraction[3];
    for (size_t i = 0; i < 3; ++i) {
      const uint64_t hundredths =
          (activity.loads[i] * 100 + LoadAverage::Scale / 2) / LoadAverage::Scale;
      whole[i] = hundredths / 100;
      fraction[i] = hundredths % 100;
    }
    contents.Format("%lu.%02lu %lu.%02lu %lu.%02lu %lu/%lu %lu\n", whole[0], fraction[0], whole[1],
                    fraction[1], whole[2], fraction[2], tasks.runnable, tasks.total, tasks.lastPid);
    return true;
  }
};

class SystemStatFile final : public GeneratedFile {
 public:
  SystemStatFile(uintptr_t inode, ProcFs& filesystem, File* parent)
      : GeneratedFile(String("stat"), inode, filesystem, parent) {}

 private:
  bool generate(String& contents) const override {
    const auto activity = Scheduler::instance().systemActivity();
    const TaskCounts tasks = taskCounts();
    const uint64_t user = activity.userNanoseconds / NanosecondsPerClockTick;
    const uint64_t kernel = activity.kernelNanoseconds / NanosecondsPerClockTick;
    const uint64_t idle = activity.idleNanoseconds / NanosecondsPerClockTick;
    const uint64_t uptime = Time::getTicks() / Time::Multiplier::Second;
    const uint64_t realtime = Time::getTime();
    const uint64_t bootTime = realtime > uptime ? realtime - uptime : 0;
    contents.Format(
        "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n"
        "btime %lu\n"
        "procs_running %lu\n"
        "procs_blocked 0\n",
        user, kernel, idle, bootTime, tasks.runnable);
    return true;
  }
};

class CpuInfoFile final : public GeneratedFile {
 public:
  CpuInfoFile(uintptr_t inode, ProcFs& filesystem, File* parent)
      : GeneratedFile(String("cpuinfo"), inode, filesystem, parent) {}

 private:
  bool generate(String& contents) const override {
    for (size_t processor = 0; processor < Processor::getCount(); ++processor) {
      String entry;
      entry.Format(
          "processor\t: %lu\n"
          "vendor_id\t: Pedigree\n"
          "model name\t: Pedigree virtual processor\n"
          "flags\t\t:\n\n",
          processor);
      contents += entry;
    }
    return true;
  }
};

class PartitionsFile final : public GeneratedFile {
 public:
  PartitionsFile(uintptr_t inode, ProcFs& filesystem, File* parent)
      : GeneratedFile(String("partitions"), inode, filesystem, parent) {}

 private:
  bool generate(String& contents) const override {
    contents = String("major minor  #blocks  name\n\n");
    uint32_t ids[DiskEndpoints::Capacity];
    const size_t count = DiskEndpoints::snapshot(ids, DiskEndpoints::Capacity);
    for (size_t index = 0; index < count; ++index) {
      uint64_t bytes = 0;
      if (!DiskEndpoints::describe(ids[index], bytes))
        continue;
      String line;
      line.Format("%5u %5u %9lu disk%u\n", PosixBlock::PhysicalMajor, ids[index], bytes / 1024,
                  ids[index]);
      contents += line;
    }
    return true;
  }
};

class ProcessFile : public GeneratedFile {
 public:
  ProcessFile(const String& name, uintptr_t inode, ProcFs& filesystem, File* parent, size_t pid)
      : GeneratedFile(name, inode, filesystem, parent), m_Pid(pid) {}

 protected:
  bool acquire(Scheduler::ProcessLease& process) const {
    return Scheduler::instance().acquireProcessByUserspaceId(process, m_Pid) &&
           process->getType() == Process::Posix;
  }

 private:
  size_t m_Pid;
};

String processName(Process& process) {
  const LargeStaticString& description = process.description();
  size_t start = 0;
  for (size_t i = 0; i < description.length(); ++i)
    if (description[i] == '/')
      start = i + 1;
  size_t length = description.length() - start;
  if (length > 15)
    length = 15;
  return String(static_cast<const char*>(description) + start, length);
}

size_t parentId(Process& process) {
  while (Process* expected = process.getParent()) {
    Scheduler::ProcessLease parent;
    if (!Scheduler::instance().acquireProcess(parent, expected)) {
      if (process.getParent() != expected)
        continue;
      return 0;
    }
    if (process.getParent() == parent.get())
      return parent->getUserspaceId();
  }
  return 0;
}

char processState(Process& process) {
  const auto state = process.getState();
  if (state == Process::Suspended)
    return 'T';
  if (state == Process::Terminated || state == Process::Reaped)
    return 'Z';
  bool sleeping = false;
  const size_t count = process.getNumThreads();
  for (size_t i = 0; i < count; ++i) {
    Process::ThreadLease thread;
    if (!process.acquireThread(thread, i))
      continue;
    const auto status = thread->getStatus();
    if (status == Thread::Ready || status == Thread::Running)
      return 'R';
    if (status == Thread::Sleeping || status == Thread::AwaitingJoin)
      sleeping = true;
  }
  return sleeping ? 'S' : 'R';
}

class ProcessStatFile final : public ProcessFile {
 public:
  ProcessStatFile(uintptr_t inode, ProcFs& filesystem, File* parent, size_t pid)
      : ProcessFile(String("stat"), inode, filesystem, parent, pid) {}

 private:
  bool generate(String& contents) const override {
    Scheduler::ProcessLease lease;
    if (!acquire(lease))
      return false;
    auto& process = *static_cast<PosixProcess*>(lease.get());
    size_t processGroup = 0;
    process.getProcessGroupId(processGroup);
    const uint64_t user = process.getUserTime() / NanosecondsPerClockTick;
    const uint64_t kernel = process.getKernelTime() / NanosecondsPerClockTick;
    const uint64_t childrenUser = process.getReapedChildrenUserTime() / NanosecondsPerClockTick;
    const uint64_t childrenKernel = process.getReapedChildrenKernelTime() / NanosecondsPerClockTick;
    const uint64_t start = process.getStartTimeTicks() / NanosecondsPerClockTick;
    const ssize_t virtualPages = process.getVirtualPageCount();
    const ssize_t residentPages = process.getPhysicalPageCount();
    const uint64_t virtualBytes = virtualPages > 0 ? static_cast<uint64_t>(virtualPages) *
                                                         PhysicalMemoryManager::getPageSize()
                                                   : 0;
    const long resident = residentPages > 0 ? residentPages : 0;
    // Proc consumers advance by field number even when they ignore the value.
    contents.Format(
        "%lu (%s) %c %lu %lu %lu 0 0 0 0 0 0 0 %lu %lu %lu %lu 20 0 %lu 0 %lu %lu %ld"
        " 0 0 0 0 0 0 0"
        " 0 0 0 0 0 0 0"
        " 0 0 0 0 0 0 0"
        " 0 0 0 0 0 0 0\n",
        process.getUserspaceId(), processName(process).cstr(), processState(process),
        parentId(process), processGroup, process.getSessionId(), user, kernel, childrenUser,
        childrenKernel, process.getNumThreads(), start, virtualBytes, resident);
    return true;
  }
};

class ProcessStatmFile final : public ProcessFile {
 public:
  ProcessStatmFile(uintptr_t inode, ProcFs& filesystem, File* parent, size_t pid)
      : ProcessFile(String("statm"), inode, filesystem, parent, pid) {}

 private:
  bool generate(String& contents) const override {
    Scheduler::ProcessLease process;
    if (!acquire(process))
      return false;
    const ssize_t total = process->getVirtualPageCount();
    const ssize_t resident = process->getPhysicalPageCount();
    const ssize_t shared = process->getSharedPageCount();
    contents.Format("%ld %ld %ld 0 0 0 0\n", total > 0 ? total : 0, resident > 0 ? resident : 0,
                    shared > 0 ? shared : 0);
    return true;
  }
};

class ProcessStatusFile final : public ProcessFile {
 public:
  ProcessStatusFile(uintptr_t inode, ProcFs& filesystem, File* parent, size_t pid)
      : ProcessFile(String("status"), inode, filesystem, parent, pid) {}

 private:
  bool generate(String& contents) const override {
    Scheduler::ProcessLease lease;
    if (!acquire(lease))
      return false;
    auto& process = *static_cast<PosixProcess*>(lease.get());
    const auto credentials = process.snapshotCredentials();
    const ssize_t virtualPages = process.getVirtualPageCount();
    const ssize_t residentPages = process.getPhysicalPageCount();
    const uint64_t kilobytesPerPage = PhysicalMemoryManager::getPageSize() / 1024;
    contents.Format(
        "Name:\t%s\n"
        "State:\t%c\n"
        "Tgid:\t%lu\n"
        "Pid:\t%lu\n"
        "PPid:\t%lu\n"
        "Uid:\t%lu\t%lu\t%lu\t%lu\n"
        "Gid:\t%lu\t%lu\t%lu\t%lu\n"
        "Threads:\t%lu\n"
        "VmSize:\t%lu kB\n"
        "VmRSS:\t%lu kB\n",
        processName(process).cstr(), processState(process), process.getUserspaceId(),
        process.getUserspaceId(), parentId(process), credentials.ruid, credentials.euid,
        credentials.suid, credentials.euid, credentials.rgid, credentials.egid, credentials.sgid,
        credentials.egid, process.getNumThreads(),
        virtualPages > 0 ? static_cast<uint64_t>(virtualPages) * kilobytesPerPage : 0,
        residentPages > 0 ? static_cast<uint64_t>(residentPages) * kilobytesPerPage : 0);
    return true;
  }
};

class ProcessCommandLineFile final : public ProcessFile {
 public:
  ProcessCommandLineFile(uintptr_t inode, ProcFs& filesystem, File* parent, size_t pid)
      : ProcessFile(String("cmdline"), inode, filesystem, parent, pid) {}

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    Vector<String> arguments;
    if (!snapshot(arguments))
      return 0;
    uint64_t copied = 0;
    for (const auto& argument : arguments) {
      const uint64_t fieldSize = argument.length() + 1;
      if (location >= fieldSize) {
        location -= fieldSize;
        continue;
      }
      const uint64_t available = fieldSize - location;
      const uint64_t amount = min(size - copied, available);
      MemoryCopy(reinterpret_cast<void*>(buffer + copied), argument.cstr() + location, amount);
      copied += amount;
      location = 0;
      if (copied == size)
        break;
    }
    return copied;
  }

  size_t getSize() override {
    Vector<String> arguments;
    if (!snapshot(arguments))
      return 0;
    size_t size = 0;
    for (const auto& argument : arguments)
      size += argument.length() + 1;
    return size;
  }

 private:
  bool snapshot(Vector<String>& arguments) const {
    Scheduler::ProcessLease lease;
    if (!acquire(lease))
      return false;
    auto* subsystem = static_cast<PosixSubsystem*>(lease->getSubsystem());
    return subsystem && subsystem->commandLine(arguments);
  }

  bool generate(String& contents) const override {
    contents.clear();
    return false;
  }
};
}  // namespace

bool procfsAddSystemStatusFiles(ProcFs& filesystem, ProcFsDirectory& root) {
  auto* loadAverage = new LoadAverageFile(filesystem.getNextInode(), filesystem, &root);
  auto* stat = new SystemStatFile(filesystem.getNextInode(), filesystem, &root);
  auto* cpuInfo = new CpuInfoFile(filesystem.getNextInode(), filesystem, &root);
  auto* partitions = new PartitionsFile(filesystem.getNextInode(), filesystem, &root);
  auto* kernel = new ProcFsDirectory(String("kernel"), 0, 0, 0, filesystem.getNextInode(),
                                     &filesystem, 0, &root);
  auto* threads =
      kernel ? new KernelThreadsFile(filesystem.getNextInode(), filesystem, kernel) : nullptr;
  auto* latency =
      kernel ? new LatencyFile(filesystem.getNextInode(), filesystem, kernel) : nullptr;
  if (!loadAverage || !stat || !cpuInfo || !partitions || !kernel || !threads || !latency) {
    delete loadAverage;
    delete stat;
    delete cpuInfo;
    delete partitions;
    delete threads;
    delete latency;
    delete kernel;
    return false;
  }
  root.addEntry(loadAverage->getName(), loadAverage);
  root.addEntry(stat->getName(), stat);
  root.addEntry(cpuInfo->getName(), cpuInfo);
  root.addEntry(partitions->getName(), partitions);
  kernel->setPermissions(FILE_UR | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX);
  kernel->addEntry(threads->getName(), threads);
  kernel->addEntry(latency->getName(), latency);
  root.addEntry(kernel->getName(), kernel);
  return true;
}

bool procfsAddProcessStatusFiles(ProcFs& filesystem, ProcFsDirectory& directory, size_t pid) {
  auto* stat = new ProcessStatFile(filesystem.getNextInode(), filesystem, &directory, pid);
  auto* statm = new ProcessStatmFile(filesystem.getNextInode(), filesystem, &directory, pid);
  auto* status = new ProcessStatusFile(filesystem.getNextInode(), filesystem, &directory, pid);
  auto* commandLine =
      new ProcessCommandLineFile(filesystem.getNextInode(), filesystem, &directory, pid);
  if (!stat || !statm || !status || !commandLine) {
    delete stat;
    delete statm;
    delete status;
    delete commandLine;
    return false;
  }
  directory.addEntry(stat->getName(), stat);
  directory.addEntry(statm->getName(), statm);
  directory.addEntry(status->getName(), status);
  directory.addEntry(commandLine->getName(), commandLine);
  return true;
}

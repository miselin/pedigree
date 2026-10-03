/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LatencyAccounting.h"
#include "pedigree/kernel/Metrics.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/StaticString.h"

#include "DevFs-block.h"
#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "ProcFs.h"
#include "file-metadata.h"
#include "metadata-abi.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/VFS.h"
#include <sys/stat.h>

namespace {
constexpr uint64_t ClockTicksPerSecond = 100;
constexpr uint64_t NanosecondsPerClockTick = Time::Multiplier::Second / ClockTicksPerSecond;

struct TaskCounts {
  size_t processes = 0;
  size_t total = 0;
  size_t runnable = 0;
  size_t lastPid = 0;
};

TaskCounts taskCounts();

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

class MetricsSnapshotFile final : public File {
 public:
  MetricsSnapshotFile(uintptr_t inode, ProcFs& filesystem, File* parent, String&& contents)
      : File(String("metrics"), 0, 0, 0, inode, &filesystem, contents.length(), parent),
        m_Contents(pedigree_std::move(contents)) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    if (location >= m_Contents.length()) {
      return 0;
    }
    size = min(size, uint64_t(m_Contents.length() - location));
    MemoryCopy(reinterpret_cast<void*>(buffer), m_Contents.cstr() + location, size);
    return size;
  }

  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    SYSCALL_ERROR(PermissionDenied);
    return 0;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

 private:
  const String m_Contents;
};

class MetricsFile final : public File {
 public:
  MetricsFile(uintptr_t inode, ProcFs& filesystem, File* parent)
      : File(String("metrics"), 0, 0, 0, inode, &filesystem, 0, parent) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
  }

  File* openForDescriptor(RetainedFile& owner) override {
    const size_t cpus = Processor::getCount();
#if PEDIGREE_METRICS
    auto snapshots = UniqueArray<Metrics::Snapshot>::allocate(cpus);
    if (!snapshots) {
      SYSCALL_ERROR(OutOfMemory);
      return nullptr;
    }
    // Capture every counter before formatting, without stopping other CPUs.
    for (size_t cpu = 0; cpu < cpus; ++cpu) {
      Metrics::snapshot(cpu, snapshots.get()[cpu]);
    }
#endif
    const uint64_t uptime = Time::getTicks();
    StaticString<32> uptimeText;
    uptimeText.append(uptime / Time::Multiplier::Second);
    uptimeText.append(".");
    uptimeText.append(uptime % Time::Multiplier::Second, 10, 9);
    String contents;
    contents.Format(
        "# TYPE pedigree_metrics_enabled gauge\npedigree_metrics_enabled %u\n"
        "# TYPE pedigree_cpus gauge\npedigree_cpus %lu\n"
        "# TYPE pedigree_uptime_seconds gauge\npedigree_uptime_seconds %s\n",
        unsigned(PEDIGREE_METRICS), cpus, static_cast<const char*>(uptimeText));
    const auto memory = PhysicalMemoryManager::instance().memorySnapshot();
    const TaskCounts tasks = taskCounts();
    String line;
    // String::Format has a 256-byte buffer, so format each gauge separately.
    line.Format(
        "# HELP pedigree_memory_managed_bytes Physical memory managed by the page allocator.\n"
        "# TYPE pedigree_memory_managed_bytes gauge\npedigree_memory_managed_bytes %lu\n",
        memory.totalPages * PhysicalMemoryManager::getPageSize());
    contents += line;
    line.Format(
        "# HELP pedigree_memory_free_bytes Physical memory currently free in the page allocator.\n"
        "# TYPE pedigree_memory_free_bytes gauge\npedigree_memory_free_bytes %lu\n",
        memory.freePages * PhysicalMemoryManager::getPageSize());
    contents += line;
    line.Format(
        "# HELP pedigree_processes Processes registered with the scheduler.\n"
        "# TYPE pedigree_processes gauge\npedigree_processes %lu\n",
        tasks.processes);
    contents += line;
    line.Format(
        "# HELP pedigree_threads Non-idle threads registered with a process.\n"
        "# TYPE pedigree_threads gauge\npedigree_threads %lu\n",
        tasks.total);
    contents += line;
    line.Format(
        "# HELP pedigree_runnable_threads Non-idle threads ready or running.\n"
        "# TYPE pedigree_runnable_threads gauge\npedigree_runnable_threads %lu\n",
        tasks.runnable);
    contents += line;
#if PEDIGREE_METRICS
    struct Counter {
      const char* name;
      Metrics::Counter counter;
      const char* help;
    };
    static const Counter counters[] = {
        {"pedigree_scheduler_schedules_total", Metrics::Schedule, "Scheduler selection calls."},
        {"pedigree_scheduler_yields_total", Metrics::Yield, "Explicit scheduler yield calls."},
        {"pedigree_scheduler_context_switches_total", Metrics::ContextSwitch,
         "Context switches to another thread."},
        {"pedigree_scheduler_same_thread_selections_total", Metrics::SameThread,
         "Scheduler selections that kept the current thread."},
        {"pedigree_scheduler_idle_selections_total", Metrics::IdleSelection,
         "Context switches selecting an idle thread."},
        {"pedigree_scheduler_timer_callbacks_total", Metrics::Timer, "Scheduler timer callbacks."},
        {"pedigree_scheduler_reschedule_services_total", Metrics::RescheduleService,
         "Pending scheduling and IRQ work service calls, including calls without pending work."},
        {"pedigree_scheduler_worker_wakeups_total", Metrics::WorkerWake,
         "Successful IRQ worker wakeups."},
#if X64
        {"pedigree_interrupt_entries_total", Metrics::Interrupt,
         "x86-64 interrupt handler entries for vectors 32 and above."},
        {"pedigree_exception_entries_total", Metrics::Exception,
         "x86-64 exception handler entries for vectors below 32."},
        {"pedigree_syscalls_total", Metrics::Syscall, "x86-64 C++ syscall dispatch entries."},
#endif
        {"pedigree_process_created_total", Metrics::ProcessCreated,
         "Completed process constructions."},
        {"pedigree_process_exited_total", Metrics::ProcessExited,
         "Processes published as terminated."},
        {"pedigree_process_destroyed_total", Metrics::ProcessDestroyed,
         "Completed process destructions."},
        {"pedigree_thread_created_total", Metrics::ThreadCreated,
         "Threads registered with a process, including idle threads."},
        {"pedigree_thread_exit_started_total", Metrics::ThreadExitStarted,
         "Threads beginning shutdown for the first time."},
        {"pedigree_thread_reapable_total", Metrics::ThreadReapable,
         "Threads retired from their execution stack."},
        {"pedigree_thread_destroyed_total", Metrics::ThreadDestroyed,
         "Completed thread destructions."},
        {"pedigree_wait_queue_waits_total", Metrics::WaitQueueWait,
         "Waiter enrollments in wait queues."},
        {"pedigree_wait_queue_unqueues_total", Metrics::WaitQueueUnqueue,
         "Waiter removals from wait queues."},
        {"pedigree_wait_queue_wakes_total", Metrics::WaitQueueWake,
         "First successful completion of an enrolled waiter."},
        {"pedigree_wait_queue_early_wakes_total", Metrics::WaitQueueEarlyWake,
         "Waiter completions before the thread entered sleeping state."},
        {"pedigree_wait_queue_cancels_total", Metrics::WaitQueueCancel,
         "Enrolled waiters removed by cancellation."},
        {"pedigree_wait_queue_requeues_total", Metrics::WaitQueueRequeue,
         "Waiter moves to another channel."},
        {"pedigree_semaphore_contended_acquires_total", Metrics::SemaphoreContended,
         "Semaphore acquires that missed the initial fast attempts."},
        {"pedigree_physical_page_allocations_total", Metrics::PhysicalPageAlloc,
         "Successful single-page allocation calls after CPU initialization."},
        {"pedigree_physical_page_allocation_failures_total", Metrics::PhysicalPageAllocFailure,
         "Failed single-page allocation calls after CPU initialization."},
        {"pedigree_physical_page_frees_total", Metrics::PhysicalPageFree,
         "Pages returned to the allocator after their last reference."},
        {"pedigree_memory_pressure_passes_total", Metrics::MemoryPressurePass,
         "Admitted memory pressure compact passes."},
        {"pedigree_memory_pressure_successes_total", Metrics::MemoryPressurePassSuccess,
         "Memory pressure passes with a handler reporting success."},
        {"pedigree_memory_pressure_kill_requests_total", Metrics::MemoryPressureKill,
         "Victim kill calls made under memory pressure."},
        {"pedigree_page_fault_entries_total", Metrics::PageFault,
         "Architecture page-fault handler entries."},
        {"pedigree_page_fault_copy_on_write_total", Metrics::PageFaultCopyOnWrite,
         "Page faults resolved by copy-on-write."},
        {"pedigree_page_fault_handled_total", Metrics::PageFaultHandled,
         "Page faults resolved by a registered trap handler."},
        {"pedigree_page_fault_deferred_total", Metrics::PageFaultDeferred,
         "Page faults deferred to a userspace subsystem."},
        {"pedigree_cache_lookup_hits_total", Metrics::CacheLookupHit,
         "Successful Cache::lookup calls."},
        {"pedigree_cache_lookup_misses_total", Metrics::CacheLookupMiss,
         "Cache::lookup calls without an available page."},
        {"pedigree_cache_read_bytes_total", Metrics::CacheReadBytes,
         "Bytes returned by Cache::read."},
        {"pedigree_cache_evicted_pages_total", Metrics::CacheEvictedPages,
         "Cache pages successfully retired."},
        {"pedigree_cache_writeback_pages_total", Metrics::CacheWritebackPages,
         "Cache pages submitted to backing-store writeback callbacks."},
        {"pedigree_cache_writeback_failures_total", Metrics::CacheWritebackFailures,
         "Cache pages whose backing-store writeback callback failed."},
        {"pedigree_network_rx_packets_total", Metrics::NetworkRxAccepted,
         "Received packets accepted by the network stack."},
        {"pedigree_network_rx_bytes_total", Metrics::NetworkRxBytes,
         "Received bytes accepted by the network stack."},
        {"pedigree_network_rx_no_device_total", Metrics::NetworkRxNoDevice,
         "Received packets dropped without a registered device."},
        {"pedigree_network_rx_filtered_total", Metrics::NetworkRxFiltered,
         "Received packets rejected by the network filter."},
        {"pedigree_network_rx_no_buffer_total", Metrics::NetworkRxNoBuffer,
         "Received packets dropped for lack of a network buffer."},
        {"pedigree_network_rx_input_failures_total", Metrics::NetworkRxInputFailed,
         "Received packets rejected by the network stack input."},
        {"pedigree_network_tx_packets_total", Metrics::NetworkTxAccepted,
         "Transmitted packets accepted by a device send call."},
        {"pedigree_network_tx_bytes_total", Metrics::NetworkTxBytes,
         "Transmitted bytes accepted by a device send call."},
        {"pedigree_network_tx_filtered_total", Metrics::NetworkTxFiltered,
         "Transmitted packets rejected by the network filter."},
        {"pedigree_network_tx_send_failures_total", Metrics::NetworkTxSendFailed,
         "Transmitted packets rejected by a device send call."},
        {"pedigree_file_read_calls_total", Metrics::FileReadCalls,
         "File::read calls, including cached reads through that path."},
        {"pedigree_file_cached_read_calls_total", Metrics::FileCachedReadCalls,
         "Direct File::readCached calls."},
        {"pedigree_file_read_bytes_total", Metrics::FileReadBytes,
         "Bytes returned by file read calls, counted once per path."},
        {"pedigree_file_write_calls_total", Metrics::FileWriteCalls,
         "File write and append calls."},
        {"pedigree_file_write_bytes_total", Metrics::FileWriteBytes,
         "Bytes returned by file write and append calls."},
        {"pedigree_vfs_find_calls_total", Metrics::VfsFindCalls, "VFS::find calls."},
        {"pedigree_vfs_find_misses_total", Metrics::VfsFindMisses,
         "VFS::find calls returning no file."},
        {"pedigree_vfs_sync_calls_total", Metrics::VfsSyncCalls,
         "Filesystem backend sync invocations."},
        {"pedigree_vfs_sync_unsupported_total", Metrics::VfsSyncUnsupported,
         "Filesystem backend sync invocations reporting unsupported."},
        {"pedigree_vfs_sync_failures_total", Metrics::VfsSyncFailed,
         "Filesystem backend sync invocations that failed for another reason."},
    };
    for (const Counter& counter : counters) {
      line.Format("# HELP %s %s\n# TYPE %s counter\n", counter.name, counter.help, counter.name);
      contents += line;
      for (size_t cpu = 0; cpu < cpus; ++cpu) {
        StaticString<32> value;
        value.append(snapshots.get()[cpu].values[counter.counter]);
        line.Format("%s{cpu=\"%lu\"} %s\n", counter.name, cpu, static_cast<const char*>(value));
        contents += line;
      }
    }
    for (unsigned contended = 0; contended < 2; ++contended) {
      const char* name = contended ? "pedigree_spinlock_contended_acquires_total"
                                   : "pedigree_spinlock_acquires_total";
      const char* help = contended
                             ? "Successful acquisitions that paused at least once while waiting."
                             : "Successful acquisition calls, including recursive acquisitions.";
      line.Format("# HELP %s %s\n# TYPE %s counter\n", name, help, name);
      contents += line;
      for (size_t cpu = 0; cpu < cpus; ++cpu) {
        const auto counter = contended ? Metrics::SpinlockContended : Metrics::SpinlockAcquire;
        StaticString<32> value;
        value.append(snapshots.get()[cpu].values[counter]);
        line.Format("%s{cpu=\"%lu\",policy=\"no_irq\"} %s\n", name, cpu,
                    static_cast<const char*>(value));
        contents += line;
      }
    }
#endif
    contents += String("# EOF\n");
    auto* snapshot = new MetricsSnapshotFile(getInode(), *static_cast<ProcFs*>(getFilesystem()),
                                             getParent(), pedigree_std::move(contents));
    if (!snapshot || !VFS::instance().tryTrackFile(snapshot)) {
      delete snapshot;
      SYSCALL_ERROR(OutOfMemory);
      return nullptr;
    }
    owner.adopt(snapshot);
    return snapshot;
  }
};

TaskCounts taskCounts() {
  TaskCounts result;
  Scheduler& scheduler = Scheduler::instance();
  const size_t processCount = scheduler.getNumProcesses();
  result.processes = processCount;
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
    return Scheduler::instance().acquireProcessByUserspaceId(
               process, m_Pid, static_cast<ProcFs*>(getFilesystem())->pidNamespace().get()) &&
           process->getType() == Process::Posix;
  }

  const UserspacePidNamespace* pidNamespace() const {
    return static_cast<ProcFs*>(getFilesystem())->pidNamespace().get();
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

size_t parentId(Process& process, const UserspacePidNamespace* space) {
  while (Process* expected = process.getParent()) {
    Scheduler::ProcessLease parent;
    if (!Scheduler::instance().acquireProcess(parent, expected)) {
      if (process.getParent() != expected)
        continue;
      return 0;
    }
    if (process.getParent() == parent.get())
      return parent->getUserspaceId(space);
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

String mountinfoEscape(const String& value) {
  String result;
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case ' ':
        result += "\\040";
        break;
      case '\t':
        result += "\\011";
        break;
      case '\n':
        result += "\\012";
        break;
      case '\\':
        result += "\\134";
        break;
      default:
        result += String(value.cstr() + i, 1);
        break;
    }
  }
  return result;
}

class ProcessMountInfoFile final : public ProcessFile {
 public:
  ProcessMountInfoFile(uintptr_t inode, ProcFs& filesystem, File* parent, size_t pid)
      : ProcessFile(String("mountinfo"), inode, filesystem, parent, pid) {}

 private:
  bool generate(String& contents) const override {
    Scheduler::ProcessLease process;
    if (!acquire(process)) {
      return false;
    }
    auto context = process->acquireFilesystemContext();
    auto* view = VfsMountView::fromContext(context);
    Vector<VfsMountView::MountSnapshot> mounts;
    if (!view || !view->snapshotMounts(context, mounts)) {
      return false;
    }
    for (const auto& mount : mounts) {
      VFS::MountOperation backing;
      if (!mount.backing.acquire(backing)) {
        continue;
      }
      struct stat attributes = {};
      if (!posix_stat_file("", backing.filesystem()->getRoot(), &attributes)) {
        return false;
      }
      const unsigned major = PosixMetadata::deviceMajor(attributes.st_dev);
      const unsigned minor = PosixMetadata::deviceMinor(attributes.st_dev);
      String options(mount.flags & VfsMountView::ReadOnly ? "ro" : "rw");
      if (mount.flags & VfsMountView::NoSuid) {
        options += ",nosuid";
      }
      if (mount.flags & VfsMountView::NoDev) {
        options += ",nodev";
      }
      if (mount.flags & VfsMountView::NoExec) {
        options += ",noexec";
      }
      const String root = mountinfoEscape(mount.root);
      const String path = mountinfoEscape(mount.path);
      const String kind = mountinfoEscape(backing.filesystem()->getVolumeLabel());
      String line;
      line.Format("%lu %lu %u:%u %s %s %s - %s none %s\n", mount.id, mount.parentId, major, minor,
                  root.cstr(), path.cstr(), options.cstr(), kind.cstr(),
                  mount.flags & VfsMountView::ReadOnly ? "ro" : "rw");
      contents += line;
    }
    return true;
  }
};

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
    process.getProcessGroupId(processGroup, pidNamespace());
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
        process.getUserspaceId(pidNamespace()), processName(process).cstr(), processState(process),
        parentId(process, pidNamespace()), processGroup, process.getSessionId(pidNamespace()), user,
        kernel, childrenUser, childrenKernel, process.getNumThreads(), start, virtualBytes,
        resident);
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
        processName(process).cstr(), processState(process), process.getUserspaceId(pidNamespace()),
        process.getUserspaceId(pidNamespace()), parentId(process, pidNamespace()), credentials.ruid,
        credentials.euid, credentials.suid, credentials.euid, credentials.rgid, credentials.egid,
        credentials.sgid, credentials.egid, process.getNumThreads(),
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
      const char* field = argument.length() ? argument.cstr() : "";
      MemoryCopy(reinterpret_cast<void*>(buffer + copied), field + location, amount);
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
  auto* metrics = new MetricsFile(filesystem.getNextInode(), filesystem, &root);
  auto* kernel = new ProcFsDirectory(String("kernel"), 0, 0, 0, filesystem.getNextInode(),
                                     &filesystem, 0, &root);
  auto* threads =
      kernel ? new KernelThreadsFile(filesystem.getNextInode(), filesystem, kernel) : nullptr;
  auto* latency = kernel ? new LatencyFile(filesystem.getNextInode(), filesystem, kernel) : nullptr;
  if (!loadAverage || !stat || !cpuInfo || !partitions || !metrics || !kernel || !threads ||
      !latency) {
    delete loadAverage;
    delete stat;
    delete cpuInfo;
    delete partitions;
    delete metrics;
    delete threads;
    delete latency;
    delete kernel;
    return false;
  }
  root.addEntry(loadAverage->getName(), loadAverage);
  root.addEntry(stat->getName(), stat);
  root.addEntry(cpuInfo->getName(), cpuInfo);
  root.addEntry(partitions->getName(), partitions);
  root.addEntry(metrics->getName(), metrics);
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
  auto* mountinfo =
      new ProcessMountInfoFile(filesystem.getNextInode(), filesystem, &directory, pid);
  if (!stat || !statm || !status || !commandLine || !mountinfo) {
    delete stat;
    delete statm;
    delete status;
    delete commandLine;
    delete mountinfo;
    return false;
  }
  directory.addEntry(stat->getName(), stat);
  directory.addEntry(statm->getName(), statm);
  directory.addEntry(status->getName(), status);
  directory.addEntry(commandLine->getName(), commandLine);
  directory.addEntry(mountinfo->getName(), mountinfo);
  return true;
}

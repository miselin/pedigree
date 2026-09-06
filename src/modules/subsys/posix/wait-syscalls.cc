/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "linux-resource-abi.h"
#include "wait-state.h"
#include "wait-syscalls.h"

namespace {
constexpr unsigned NoHang = 1, Stopped = 2, Exited = 4, Continued = 8;
constexpr unsigned NoWait = 0x01000000;
constexpr unsigned DeferredOptions = 0xE0000000;  // __WNOTHREAD, __WALL, __WCLONE.

struct LinuxWaitInformation {
  int32_t signal;
  int32_t error;
  int32_t code;
  uint32_t reserved;
  int32_t pid;
  uint32_t uid;
  int32_t status;
  uint8_t remainder[100];
};
static_assert(sizeof(LinuxWaitInformation) == 128, "Linux amd64 siginfo size changed");
static_assert(offsetof(LinuxWaitInformation, pid) == 16, "Linux amd64 child PID offset changed");
static_assert(offsetof(LinuxWaitInformation, status) == 24,
              "Linux amd64 child status offset changed");

class WaitResult {
 public:
  WaitResult() : thread(*Processor::information().getCurrentThread()) {}
  ~WaitResult() {
    thread.setErrno(error);
  }
  int finish(int result) {
    error = result < 0 ? thread.getErrno() : 0;
    return result;
  }

 private:
  Thread& thread;
  size_t error = 0;
};

void snapshotCurrentGroup(PosixWait::Request& request) {
  auto* process =
      static_cast<PosixProcess*>(Processor::information().getCurrentThread()->getParent());
  size_t group = 0;
  request.id = process->getProcessGroupId(group) ? static_cast<int32_t>(group) : -1;
}

void eventOptions(unsigned options, PosixWait::Request& request) {
  request.events = ((options & Exited) ? static_cast<unsigned>(PosixWait::Exited) : 0U) |
                   ((options & Stopped) ? static_cast<unsigned>(PosixWait::Stopped) : 0U) |
                   ((options & Continued) ? static_cast<unsigned>(PosixWait::Continued) : 0U);
  request.noHang = options & NoHang;
  request.noWait = options & NoWait;
}

int prepareWait4(int pid, unsigned options, PosixWait::Request& request) {
  if (options & ~(NoHang | Stopped | Continued | DeferredOptions)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (pid == INT32_MIN) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  if (options & DeferredOptions) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  eventOptions(options | Exited, request);
  if (pid > 0) {
    request.selector = PosixWait::Selector::Pid;
    request.id = pid;
  } else if (pid != -1) {
    request.selector = PosixWait::Selector::Pgid;
    if (!pid)
      snapshotCurrentGroup(request);
    else
      request.id = -pid;
  }
  return 0;
}

int prepareWaitId(int which, int32_t id, unsigned options, PosixWait::Request& request) {
  if ((options & ~(NoHang | NoWait | Exited | Stopped | Continued | DeferredOptions)) ||
      !(options & (Exited | Stopped | Continued))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (which < 0 || which > 3 || (which == 1 && id <= 0) || (which >= 2 && id < 0)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if ((options & DeferredOptions) || which == 3) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  eventOptions(options, request);
  if (which == 1) {
    request.selector = PosixWait::Selector::Pid;
    request.id = id;
  } else if (which == 2) {
    request.selector = PosixWait::Selector::Pgid;
    request.id = id;
    if (!id)
      snapshotCurrentGroup(request);
  }
  return 0;
}

LinuxRusage64 usageFor(const PosixWait::Report& report) {
  LinuxRusage64 usage = {};
  usage.userSeconds = report.userNanoseconds / Time::Multiplier::Second;
  usage.userMicroseconds =
      (report.userNanoseconds % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
  usage.systemSeconds = report.kernelNanoseconds / Time::Multiplier::Second;
  usage.systemMicroseconds =
      (report.kernelNanoseconds % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
  return usage;
}

int encodedStatus(const PosixWait::Report& report) {
  switch (report.cause) {
    case PosixWait::Exit:
      return (report.status & 0xFF) << 8;
    case PosixWait::Stop:
      return ((report.status & 0xFF) << 8) | 0x7F;
    case PosixWait::Continue:
      return 0xFFFF;
    default:
      return (report.status & 0x7F) | (report.cause == PosixWait::Dumped ? 0x80 : 0);
  }
}

bool copyInformation(void* destination, const PosixWait::Report& report, bool selected) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(destination);
  constexpr size_t extent = sizeof(LinuxWaitInformation);
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  // Linux access_ok admits the whole ABI object by address range, while the
  // actual stores touch six fields only. Inaccessible padding is not a fault.
  if (address > ~uintptr_t(0) - (extent - 1))
    return false;
  const uintptr_t end = address + extent - 1;
  if (address < space.getUserStart() || end >= space.getKernelStart() ||
      !space.isAddressValid(reinterpret_cast<void*>(address)) ||
      !space.isAddressValid(reinterpret_cast<void*>(end))) {
    return false;
  }
  LinuxWaitInformation snapshot = {};
  if (selected) {
    snapshot.signal = 17;  // Linux SIGCHLD.
    snapshot.code = report.cause;
    snapshot.pid = report.pid;
    snapshot.uid = report.uid;
    snapshot.status = report.status;
  }
  constexpr size_t offsets[] = {0, 4, 8, 16, 20, 24};
  const auto* bytes = reinterpret_cast<const uint8_t*>(&snapshot);
  for (size_t offset : offsets) {
    if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(address + offset), bytes + offset,
                                    sizeof(uint32_t))) {
      return false;
    }
  }
  return true;
}
}  // namespace

int posix_waitpid(int pid, int* status, int options, LinuxRusage64* usage) {
  WaitResult completion;
  PosixWait::Request request;
  if (prepareWait4(pid, static_cast<unsigned>(options), request) < 0)
    return completion.finish(-1);
  PosixWait::Report report;
  const int result = PosixWait::collect(request, report);
  if (result <= 0)
    return completion.finish(result);

  // Linux consumes the selected event even if either output subsequently
  // faults, and a failed status write prevents the rusage write.
  const int encoded = encodedStatus(report);
  if (status && !PosixSubsystem::copyToUser(status, &encoded, sizeof(encoded))) {
    SYSCALL_ERROR(BadAddress);
    return completion.finish(-1);
  }
  if (usage) {
    const auto snapshot = usageFor(report);
    if (!PosixSubsystem::copyToUser(usage, &snapshot, sizeof(snapshot))) {
      SYSCALL_ERROR(BadAddress);
      return completion.finish(-1);
    }
  }
  return completion.finish(report.pid);
}

int posix_waitid(int which, int32_t id, void* information, int options, LinuxRusage64* usage) {
  WaitResult completion;
  PosixWait::Request request;
  PosixWait::Report report;
  int result = prepareWaitId(which, id, static_cast<unsigned>(options), request);
  if (!result)
    result = PosixWait::collect(request, report);
  Thread* thread = Processor::information().getCurrentThread();
  const size_t savedError = result < 0 ? thread->getErrno() : 0;
  if (result > 0 && usage) {
    const auto snapshot = usageFor(report);
    if (!PosixSubsystem::copyToUser(usage, &snapshot, sizeof(snapshot))) {
      SYSCALL_ERROR(BadAddress);
      return completion.finish(-1);
    }
  }
  // Even ECHILD/EINVAL or a WNOHANG miss writes the six zero fields. A NULL
  // info pointer is permitted by the Linux syscall, independently of libc.
  if (information && !copyInformation(information, report, result > 0)) {
    SYSCALL_ERROR(BadAddress);
    return completion.finish(-1);
  }
  thread->setErrno(savedError);
  return completion.finish(result < 0 ? -1 : 0);
}

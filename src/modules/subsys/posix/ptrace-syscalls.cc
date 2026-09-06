/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <config.h>

#include "PosixSubsystem.h"
#include "ptrace-syscalls.h"
#include "trace-state.h"

namespace {
constexpr unsigned long TraceMe = 0, Continue = 7, GetRegisters = 12, Detach = 17;
constexpr unsigned long GetSignalInfo = 0x4202, GetRegisterSet = 0x4204;
constexpr uint32_t GeneralRegisters = 1;

struct LinuxIovec {
  uint64_t base, length;
};
static_assert(sizeof(LinuxIovec) == 16, "Linux amd64 iovec size");
static_assert(offsetof(LinuxIovec, length) == 8, "Linux amd64 iov_len offset");

class TraceResult {
 public:
  TraceResult() : m_Thread(*Processor::information().getCurrentThread()) {}
  ~TraceResult() {
    m_Thread.setErrno(m_Error);
  }
  long finish(long value) {
    m_Error = value < 0 ? m_Thread.getErrno() : 0;
    return value;
  }

 private:
  Thread& m_Thread;
  size_t m_Error = 0;
};

int statusResult(TraceStatus status) {
  switch (status) {
    case TraceStatus::Success:
      return 0;
    case TraceStatus::Missing:
    case TraceStatus::NotStopped:
      SYSCALL_ERROR(NoSuchProcess);
      break;
    case TraceStatus::Denied:
      SYSCALL_ERROR(NotEnoughPermissions);
      break;
    case TraceStatus::Busy:
    case TraceStatus::Unsupported:
      SYSCALL_ERROR(OperationNotSupported);
      break;
    case TraceStatus::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      break;
    case TraceStatus::Full:
      SYSCALL_ERROR(NoMoreProcesses);
      break;
    case TraceStatus::Invalid:
      SYSCALL_ERROR(InvalidArgument);
      break;
  }
  return -1;
}

bool deferredRequest(unsigned long request) {
  switch (request) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 8:
    case 9:
    case 13:
    case 14:
    case 15:
    case 16:
    case 18:
    case 19:
    case 24:
    case 25:
    case 26:
    case 30:
    case 31:
    case 32:
    case 33:
    case 0x4200:
    case 0x4201:
    case 0x4203:
    case 0x4205:
    case 0x4206:
    case 0x4207:
    case 0x4208:
    case 0x4209:
    case 0x420a:
    case 0x420b:
    case 0x420c:
    case 0x420d:
    case 0x420e:
    case 0x420f:
      return true;
    default:
      return false;
  }
}

int copyRegisters(const Amd64UserRegisters& registers, uintptr_t destination, size_t length) {
  if (length &&
      !PosixSubsystem::copyToUser(reinterpret_cast<void*>(destination), &registers, length)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}
}  // namespace

long posix_ptrace(unsigned long request, int32_t pid, uintptr_t address, uintptr_t data,
                  bool linuxAbi) {
  TraceResult result;
  Uninterruptible lifetime;
#if !X64
  SYSCALL_ERROR(OperationNotSupported);
  return result.finish(-1);
#else
  if (!linuxAbi || deferredRequest(request)) {
    SYSCALL_ERROR(OperationNotSupported);
    return result.finish(-1);
  }
  if (request == TraceMe)
    return result.finish(statusResult(posix_trace_traceme()));
  if (request != Continue && request != Detach && request != GetRegisters &&
      request != GetSignalInfo && request != GetRegisterSet) {
    SYSCALL_ERROR(IoError);
    return result.finish(-1);
  }

  TraceRelationRef relation;
  if (statusResult(posix_trace_lookup(pid, relation)))
    return result.finish(-1);

  if (request == Continue || request == Detach) {
    // Validate the full word before narrowing: -1 and high-bit values are
    // invalid signals even when their low bits name a supported signal.
    if (data > 64) {
      SYSCALL_ERROR(IoError);
      return result.finish(-1);
    }
    return result.finish(statusResult(relation->resume(static_cast<int>(data), request == Detach)));
  }
  if (request == GetSignalInfo) {
    TraceSignalInfo info{};
    if (statusResult(relation->snapshotSignalInfo(info)))
      return result.finish(-1);
    relation.reset();
    if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(data), &info, sizeof(info))) {
      SYSCALL_ERROR(BadAddress);
      return result.finish(-1);
    }
    return result.finish(0);
  }

  Amd64UserRegisters registers{};
  if (statusResult(relation->snapshotRegisters(registers)))
    return result.finish(-1);
  relation.reset();
  if (request == GetRegisters)
    return result.finish(copyRegisters(registers, data, sizeof(registers)));

  LinuxIovec vector;
  if (!PosixSubsystem::copyFromUser(&vector, reinterpret_cast<const void*>(data), sizeof(vector))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  if (static_cast<uint32_t>(address) != GeneralRegisters || vector.length % sizeof(uint64_t)) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  if (vector.length > sizeof(registers))
    vector.length = sizeof(registers);
  if (copyRegisters(registers, vector.base, vector.length))
    return result.finish(-1);
  // The data write can succeed even when this final length store faults.
  // The input base field is never rewritten, including for a zero-length read.
  if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(data + offsetof(LinuxIovec, length)),
                                  &vector.length, sizeof(vector.length))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  return result.finish(0);
#endif
}

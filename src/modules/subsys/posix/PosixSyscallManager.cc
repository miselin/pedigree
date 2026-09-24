/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/debugger/Backtrace.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include <fcntl.h>
#include <time.h>

#include "PosixSyscallManager.h"
#include "clock-adjust-syscalls.h"
#include "clone3-syscalls.h"
#include "console-syscalls.h"
#include "epoll-syscalls.h"
#include "eventfd-syscalls.h"
#include "execveat-syscalls.h"
#include "fanotify-syscalls.h"
#include "file-handle-syscalls.h"
#include "file-sync-syscalls.h"
#include "file-syscalls.h"
#include "filesystem-capability-syscalls.h"
#include "global-sync-syscalls.h"
#include "init-module-syscalls.h"
#include "inotify-syscalls.h"
#include "job-control-syscalls.h"
#include "linux-amd64-signal.h"
#include "logging.h"
#include "memfd-syscalls.h"
#include "metadata-syscalls.h"
#include "module-syscalls.h"
#include "mount-view-syscalls.h"
#include "mqueue-syscalls.h"
#include "namespace-syscalls.h"
#include "net-syscalls.h"
#include "pipe-syscalls.h"
#include "pipe-transfer-syscalls.h"
#include "poll-syscalls.h"
#include "posix-timer-syscalls.h"
#include "posixSyscallNumbers.h"
#include "process-accounting.h"
#include "process-vm-syscalls.h"
#include "pthread-syscalls.h"
#include "ptrace-syscalls.h"
#include "queued-signal.h"
#include "quota-syscalls.h"
#include "recvmmsg-syscalls.h"
#include "scheduling-syscalls.h"
#include "select-syscalls.h"
#include "signal-syscalls.h"
#include "signalfd-syscalls.h"
#include "swap-syscalls.h"
#include "syscalls/translate.h"
#include "system-information-syscalls.h"
#include "system-syscalls.h"
#include "sysv-message-syscalls.h"
#include "sysv-semaphore-syscalls.h"
#include "sysv-shm-syscalls.h"
#include "terminal-syscalls.h"
#include "timerfd-syscalls.h"
#include "transfer-syscalls.h"
#include "vm-syscalls.h"
#include "wait-syscalls.h"
#include "xattr-syscalls.h"

#if PEDIGREE_BENCHMARK_SYSCALL_TRACE
#include "syscall-trace.h"
#endif

namespace {
off_t linuxAmd64VectorOffset(uintptr_t low, uintptr_t high) {
  const uint64_t bits =
      (static_cast<uint64_t>(high) << 32U) | (static_cast<uint64_t>(low) & 0xFFFFFFFFULL);
  return static_cast<off_t>(bits);
}

#if PEDIGREE_SYSCALL_COUNTER
class SyscallLatencyRecorder {
 public:
  explicit SyscallLatencyRecorder(Process* process)
      : m_pProcess(process), m_Start(Time::getTicks()) {}

  ~SyscallLatencyRecorder() {
    if (m_pProcess) {
      m_pProcess->recordSyscallDuration(Time::getTicks() - m_Start);
    }
  }

 private:
  Process* m_pProcess;
  Time::Timestamp m_Start;
};
#endif
}  // namespace

PosixSyscallManager::PosixSyscallManager() {}

PosixSyscallManager::~PosixSyscallManager() {}

bool PosixSyscallManager::initialise() {
  SyscallManager& manager = SyscallManager::instance();
  if (!manager.registerSyscallHandler(linuxCompat, this, m_LinuxRegistration, syscallEntry)) {
    return false;
  }
  if (!manager.registerSyscallHandler(posix, this, m_PosixRegistration, syscallEntry)) {
    if (!m_LinuxRegistration.reset()) {
      FATAL("POSIX syscall registration rollback failed.");
    }
    return false;
  }
  return true;
}

bool PosixSyscallManager::closeAdmission() {
  const bool posixClosed = m_PosixRegistration.closeAdmission();
  const bool linuxClosed = m_LinuxRegistration.closeAdmission();
  return posixClosed && linuxClosed;
}

bool PosixSyscallManager::finishShutdown() {
  const bool posixRetired = m_PosixRegistration.reset();
  const bool linuxRetired = m_LinuxRegistration.reset();
  return posixRetired && linuxRetired;
}

bool PosixSyscallManager::shutdown() {
  return closeAdmission() && finishShutdown();
}

uintptr_t PosixSyscallManager::call(uintptr_t function, uintptr_t p1, uintptr_t p2, uintptr_t p3,
                                    uintptr_t p4, uintptr_t p5) {
  if (function >= serviceEnd) {
    ERROR("PosixSyscallManager: invalid function called: " << Dec << static_cast<int>(function));
    return 0;
  }

  uintptr_t ret = SyscallManager::instance().syscall(posix, function, p1, p2, p3, p4, p5);
  return ret;
}

uintptr_t PosixSyscallManager::syscall(SyscallState& state) {
  return syscallEntry(this, state);
}

uintptr_t PosixSyscallManager::syscallEntry(SyscallHandler* handler, SyscallState& state) {
#if PEDIGREE_BENCHMARK_SYSCALL_TRACE
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread ? thread->getParent() : nullptr;
  if (!process || !process->benchmarkSyscallTraceEnabled()) {
    return syscallDispatch(handler, state);
  }

  const int entryError = thread->getErrno();
  SyscallTrace trace(state, *thread);
  thread->setErrno(entryError);
  const Time::Timestamp start = Time::getTicks();
  const uintptr_t result = syscallDispatch(handler, state);
  const int error = thread->getErrno();
  const Time::Timestamp elapsed = Time::getTicks() - start;
  trace.finish(result, error, elapsed);
  thread->setErrno(error);
  return result;
}

uintptr_t PosixSyscallManager::syscallDispatch(SyscallHandler* handler, SyscallState& state) {
#endif
  auto* manager = static_cast<PosixSyscallManager*>(handler);
#if PEDIGREE_SYSCALL_COUNTER
  Process* syscallProcess = Processor::information().getCurrentThread()->getParent();
  if (syscallProcess) {
    // This is after the architecture entry stub and before ABI translation, so
    // it counts each user-visible POSIX/Linux syscall exactly once.
    syscallProcess->recordSyscall();
  }
  SyscallLatencyRecorder syscallLatencyRecorder(syscallProcess);
#endif
  const uint64_t syscallNumber = state.getSyscallNumber();
  const bool linuxAbi = state.getSyscallService() == linuxCompat;
  const uintptr_t base = linuxAbi ? 6 : 0;
  const auto argument = [&state, base](size_t index)
                            ALWAYS_INLINE { return state.getSyscallParameter(base + index); };

  if (linuxAbi) {
    // Switch ABI now that we've seen a Linux syscall come in.
    Process* pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
    pSubsystem->setAbi(PosixSubsystem::LinuxAbi);

    EMIT_IF(POSIX_LOG_FACILITIES & 128) {
      const long which = posix_translate_syscall(syscallNumber);
      if (which >= 0) {
        NOTICE("TRANSLATED syscall: Linux #" << syscallNumber << " -> Pedigree #" << which);
        NOTICE("[" << pProcess->getId() << "] : " << Dec << which << Hex);
      }
    }

    switch (syscallNumber) {
#if ARMV7
      case 0x0f0005:  // __ARM_NR_set_tls
        Processor::information().getCurrentThread()->setTlsBase(argument(0));
        return 0;
      case 140: {  // _llseek
        const off_t offset = (static_cast<uint64_t>(argument(1)) << 32) | argument(2);
        const off_t result = posix_lseek(static_cast<int>(argument(0)), offset,
                                         static_cast<int>(argument(4)));
        if (result < 0) {
          return -1;
        }
        if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(argument(3)), &result,
                                        sizeof(result))) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        return 0;
      }
#endif
#if ARM64
#define PEDIGREE_LINUX_ARM64_SYSCALL(name, number, target) \
  case PedigreeLinuxArm64Syscall_##name:                   \
    goto handle_##target;
#include "syscalls/linuxSyscallMappings-arm64.h"
#undef PEDIGREE_LINUX_ARM64_SYSCALL
#elif ARMV7
#define PEDIGREE_LINUX_ARMV7_SYSCALL(name, number, target) \
  case PedigreeLinuxArmv7Syscall_##name:                   \
    goto handle_##target;
#include "syscalls/linuxSyscallMappings-armv7.h"
#undef PEDIGREE_LINUX_ARMV7_SYSCALL
#else
#define PEDIGREE_LINUX_AMD64_SYSCALL(name, number, target) \
  case PedigreeLinuxAmd64Syscall_##name:                   \
    goto handle_##target;
#include "syscalls/linuxSyscallMappings-amd64.h"
#undef PEDIGREE_LINUX_AMD64_SYSCALL
#endif
    }

    uint64_t key = (static_cast<uint64_t>(pProcess->getId()) << 32ULL) | syscallNumber;
    bool firstOccurrence = false;
    {
      TerminationDeferral terminationDeferral;
      LockGuard<Mutex> guard(manager->m_UnknownSyscallsLock);
      firstOccurrence = !manager->m_SeenUnknownSyscalls.lookup(key);
      if (firstOccurrence) {
        manager->m_SeenUnknownSyscalls.insert(key, true);
      }
    }
    if (firstOccurrence) {
      ERROR("POSIX: unknown Linux syscall " << syscallNumber << " by pid=" << pProcess->getId()
                                            << ", translation failed!");
    }
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  EMIT_IF(POSIX_LOG_FACILITIES & 128) {
    NOTICE("[" << Processor::information().getCurrentThread()->getParent()->getId() << "] : " << Dec
               << syscallNumber << Hex);
  }

  // Both ABIs enter the same bodies without extracting unused arguments.
#define POSIX_CASE(target) \
  case target:             \
    handle_##target:
  switch (syscallNumber) {
    // POSIX system calls
    POSIX_CASE(POSIX_OPEN)
      return posix_open(reinterpret_cast<const char*>(argument(0)), argument(1), argument(2));
    POSIX_CASE(POSIX_WRITE)
      return posix_write(argument(0), reinterpret_cast<char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_READ)
      return posix_read(argument(0), reinterpret_cast<char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_PREAD64)
      return posix_pread64(static_cast<int>(argument(0)), reinterpret_cast<char*>(argument(1)),
                           static_cast<size_t>(argument(2)), static_cast<off_t>(argument(3)));
    POSIX_CASE(POSIX_PWRITE64)
      return posix_pwrite64(static_cast<int>(argument(0)),
                            reinterpret_cast<const char*>(argument(1)),
                            static_cast<size_t>(argument(2)), static_cast<off_t>(argument(3)));
    POSIX_CASE(POSIX_PREADV)
      return posix_preadv(
          static_cast<int>(argument(0)), reinterpret_cast<const struct iovec*>(argument(1)),
          static_cast<int>(argument(2)), linuxAmd64VectorOffset(argument(3), argument(4)));
    POSIX_CASE(POSIX_PWRITEV)
      return posix_pwritev(
          static_cast<int>(argument(0)), reinterpret_cast<const struct iovec*>(argument(1)),
          static_cast<int>(argument(2)), linuxAmd64VectorOffset(argument(3), argument(4)));
    POSIX_CASE(POSIX_PREADV2)
      return posix_preadv2(
          static_cast<int>(argument(0)), reinterpret_cast<const struct iovec*>(argument(1)),
          static_cast<int>(argument(2)), linuxAmd64VectorOffset(argument(3), argument(4)),
          static_cast<int>(argument(5)));
    POSIX_CASE(POSIX_PWRITEV2)
      return posix_pwritev2(
          static_cast<int>(argument(0)), reinterpret_cast<const struct iovec*>(argument(1)),
          static_cast<int>(argument(2)), linuxAmd64VectorOffset(argument(3), argument(4)),
          static_cast<int>(argument(5)));
    POSIX_CASE(POSIX_CLOSE)
      return posix_close(argument(0));
    case POSIX_SBRK:
      return posix_sbrk(argument(0));
    POSIX_CASE(POSIX_FORK)
      return posix_fork(state);
    POSIX_CASE(POSIX_VFORK)
      return posix_vfork(state);
    POSIX_CASE(POSIX_EXECVE)
      return posix_execve(reinterpret_cast<const char*>(argument(0)),
                          reinterpret_cast<const char**>(argument(1)),
                          reinterpret_cast<const char**>(argument(2)), state);
    POSIX_CASE(POSIX_EXECVEAT)
      return posix_execveat(
          static_cast<int>(argument(0)), reinterpret_cast<const char*>(argument(1)),
          reinterpret_cast<const char**>(argument(2)), reinterpret_cast<const char**>(argument(3)),
          static_cast<int>(argument(4)), state);
    POSIX_CASE(POSIX_INIT_MODULE)
      return posix_init_module(reinterpret_cast<const void*>(argument(0)), argument(1),
                               reinterpret_cast<const char*>(argument(2)));
    POSIX_CASE(POSIX_ACCT)
      return posix_acct(reinterpret_cast<const char*>(argument(0)));
    POSIX_CASE(POSIX_VHANGUP)
      return posix_vhangup();
    POSIX_CASE(POSIX_QUOTACTL)
      return posix_quotactl(static_cast<int>(argument(0)),
                            reinterpret_cast<const char*>(argument(1)),
                            static_cast<int>(argument(2)), reinterpret_cast<void*>(argument(3)));
    POSIX_CASE(POSIX_SWAPON)
      return posix_swapon(reinterpret_cast<const char*>(argument(0)),
                          static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_SWAPOFF)
      return posix_swapoff(reinterpret_cast<const char*>(argument(0)));
    POSIX_CASE(POSIX_SYSINFO)
      return posix_sysinfo(reinterpret_cast<void*>(argument(0)));
    POSIX_CASE(POSIX_PERSONALITY)
      return posix_personality(static_cast<unsigned long>(argument(0)));
    POSIX_CASE(POSIX_PIVOT_ROOT)
      return posix_pivot_root(reinterpret_cast<const char*>(argument(0)),
                              reinterpret_cast<const char*>(argument(1)));
    POSIX_CASE(POSIX_TRUNCATE)
      return posix_truncate(reinterpret_cast<const char*>(argument(0)),
                            static_cast<off_t>(argument(1)));
    POSIX_CASE(POSIX_LCHOWN)
      return posix_lchown(reinterpret_cast<const char*>(argument(0)),
                          static_cast<uid_t>(argument(1)), static_cast<gid_t>(argument(2)));
    POSIX_CASE(POSIX_UTIMENSAT)
      return posix_utimensat(
          static_cast<int>(argument(0)), reinterpret_cast<const char*>(argument(1)),
          reinterpret_cast<const void*>(argument(2)), static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_STATX)
      return posix_statx(static_cast<int>(argument(0)), reinterpret_cast<const char*>(argument(1)),
                         static_cast<int>(argument(2)), static_cast<unsigned>(argument(3)),
                         reinterpret_cast<void*>(argument(4)));
    POSIX_CASE(POSIX_FCHMODAT2)
      return posix_fchmodat2(static_cast<int>(argument(0)),
                             reinterpret_cast<const char*>(argument(1)),
                             static_cast<mode_t>(argument(2)), static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_MKNODAT)
      return posix_mknodat(static_cast<int>(argument(0)),
                           reinterpret_cast<const char*>(argument(1)),
                           static_cast<mode_t>(argument(2)), static_cast<dev_t>(argument(3)));
    POSIX_CASE(POSIX_WAITPID)
      return posix_waitpid(argument(0), reinterpret_cast<int*>(argument(1)), argument(2),
                           linuxAbi ? reinterpret_cast<LinuxRusage64*>(argument(3)) : nullptr);
    POSIX_CASE(POSIX_WAITID)
      return posix_waitid(static_cast<int>(argument(0)), static_cast<int32_t>(argument(1)),
                          reinterpret_cast<void*>(argument(2)), static_cast<int>(argument(3)),
                          reinterpret_cast<LinuxRusage64*>(argument(4)));
    POSIX_CASE(POSIX_PTRACE)
      return posix_ptrace(argument(0), static_cast<int32_t>(argument(1)), argument(2), argument(3),
                          linuxAbi);
    POSIX_CASE(POSIX_DELETE_MODULE)
      return posix_delete_module(reinterpret_cast<const char*>(argument(0)),
                                 static_cast<unsigned>(argument(1)));
    POSIX_CASE(POSIX_ADJTIMEX)
      return posix_adjtimex(reinterpret_cast<void*>(argument(0)));
    POSIX_CASE(POSIX_CLOCK_ADJTIME)
      return posix_clock_adjtime(static_cast<int>(argument(0)),
                                 reinterpret_cast<void*>(argument(1)));
    POSIX_CASE(POSIX_FDATASYNC)
      return posix_fdatasync(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_SYNC)
      return posix_sync();
    POSIX_CASE(POSIX_GETSID)
      return posix_getsid(argument(0));
    POSIX_CASE(POSIX_FALLOCATE)
      return posix_fallocate(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                             static_cast<off_t>(argument(2)), static_cast<off_t>(argument(3)));
    POSIX_CASE(POSIX_RENAMEAT2)
      return posix_renameat2(
          static_cast<int>(argument(0)), reinterpret_cast<const char*>(argument(1)),
          static_cast<int>(argument(2)), reinterpret_cast<const char*>(argument(3)),
          static_cast<unsigned>(argument(4)));
    POSIX_CASE(POSIX_RECVMMSG)
      return posix_recvmmsg(static_cast<int>(argument(0)),
                            reinterpret_cast<LinuxMmsghdr*>(argument(1)),
                            static_cast<unsigned>(argument(2)), static_cast<unsigned>(argument(3)),
                            reinterpret_cast<LinuxKernelTimespec*>(argument(4)));
    POSIX_CASE(POSIX_SYNCFS)
      return posix_syncfs(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_READAHEAD)
      return posix_readahead(static_cast<int>(argument(0)), static_cast<off_t>(argument(1)),
                             argument(2));
    POSIX_CASE(POSIX_FADVISE64)
      return posix_fadvise64(static_cast<int>(argument(0)), static_cast<off_t>(argument(1)),
                             static_cast<off_t>(argument(2)), static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_SYNC_FILE_RANGE)
      return posix_sync_file_range(static_cast<int>(argument(0)), static_cast<off_t>(argument(1)),
                                   static_cast<off_t>(argument(2)),
                                   static_cast<unsigned>(argument(3)));
    POSIX_CASE(POSIX_EXIT) {
      const uintptr_t p1 = argument(0);
      NOTICE("POSIX exit request: pid="
             << Processor::information().getCurrentThread()->getParent()->getId()
             << " code=" << p1);
      if (linuxAbi) {
        Process* process = Processor::information().getCurrentThread()->getParent();
        const bool lastThread = process->prepareThreadExit();
        const bool requested =
            lastThread ? SyscallManager::instance().requestProcessExit(static_cast<int>(p1))
                       : SyscallManager::instance().requestThreadExit();
        if (!requested) {
          FATAL("POSIX thread exit was not dispatched.");
        }
      } else if (!SyscallManager::instance().requestProcessExit(static_cast<int>(p1))) {
        FATAL("POSIX process exit was not dispatched.");
      }
      return 0;
    }
    POSIX_CASE(POSIX_EXIT_GROUP) {
      const uintptr_t p1 = argument(0);
      NOTICE("POSIX exit-group request: pid="
             << Processor::information().getCurrentThread()->getParent()->getId()
             << " code=" << p1);
      if (!SyscallManager::instance().requestProcessExit(static_cast<int>(p1))) {
        FATAL("POSIX process-group exit was not dispatched.");
      }
      return 0;
    }
    case POSIX_PTHREAD_RETURN:
      NOTICE("POSIX thread-return request: pid="
             << Processor::information().getCurrentThread()->getParent()->getId()
             << " code=" << argument(0));
      if (!SyscallManager::instance().requestThreadExit()) {
        FATAL("POSIX pthread exit was not dispatched.");
      }
      return 0;
    case POSIX_TCGETATTR:
      return posix_tcgetattr(argument(0), reinterpret_cast<struct termios*>(argument(1)));
    case POSIX_TCSETATTR:
      return posix_tcsetattr(argument(0), argument(1),
                             reinterpret_cast<struct termios*>(argument(2)));
    POSIX_CASE(POSIX_IOCTL)
    // musl's signed int request can be sign-extended into the syscall slot.
      return posix_ioctl(argument(0), linuxAbi ? static_cast<uint32_t>(argument(1)) : argument(1),
                         reinterpret_cast<void*>(argument(2)));
    POSIX_CASE(POSIX_STAT)
      return posix_stat(reinterpret_cast<const char*>(argument(0)),
                        reinterpret_cast<struct stat*>(argument(1)));
    POSIX_CASE(POSIX_FSTAT)
      return posix_fstat(argument(0), reinterpret_cast<struct stat*>(argument(1)));
    POSIX_CASE(POSIX_GETPID)
      return posix_getpid();
    POSIX_CASE(POSIX_CHDIR)
      return posix_chdir(reinterpret_cast<const char*>(argument(0)));
    POSIX_CASE(POSIX_SELECT)
      return posix_select(static_cast<int>(argument(0)), reinterpret_cast<fd_set*>(argument(1)),
                          reinterpret_cast<fd_set*>(argument(2)),
                          reinterpret_cast<fd_set*>(argument(3)),
                          reinterpret_cast<struct timeval*>(argument(4)));
    POSIX_CASE(POSIX_LSEEK)
      return posix_lseek(static_cast<int>(argument(0)), static_cast<off_t>(argument(1)),
                         static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_SOCKET)
      return posix_socket(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                          static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_CONNECT)
      return posix_connect(static_cast<int>(argument(0)),
                           reinterpret_cast<struct sockaddr_storage*>(argument(1)), argument(2));
    case POSIX_SEND:
      return posix_send(static_cast<int>(argument(0)), reinterpret_cast<void*>(argument(1)),
                        argument(2), static_cast<int>(argument(3)));
    case POSIX_RECV:
      return posix_recv(static_cast<int>(argument(0)), reinterpret_cast<void*>(argument(1)),
                        argument(2), static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_BIND)
      return posix_bind(static_cast<int>(argument(0)),
                        reinterpret_cast<struct sockaddr_storage*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_LISTEN)
      return posix_listen(static_cast<int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_ACCEPT)
      return posix_accept(static_cast<int>(argument(0)),
                          reinterpret_cast<struct sockaddr_storage*>(argument(1)),
                          reinterpret_cast<socklen_t*>(argument(2)));
    POSIX_CASE(POSIX_ACCEPT4)
      return posix_accept4(
          static_cast<int>(argument(0)), reinterpret_cast<struct sockaddr_storage*>(argument(1)),
          reinterpret_cast<socklen_t*>(argument(2)), static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_RECVFROM)
      return posix_recvfrom(static_cast<int>(argument(0)), reinterpret_cast<void*>(argument(1)),
                            argument(2), static_cast<int>(argument(3)),
                            reinterpret_cast<struct sockaddr_storage*>(argument(4)),
                            reinterpret_cast<socklen_t*>(argument(5)));
    POSIX_CASE(POSIX_SENDTO)
      return posix_sendto(static_cast<int>(argument(0)), reinterpret_cast<void*>(argument(1)),
                          argument(2), static_cast<int>(argument(3)),
                          reinterpret_cast<struct sockaddr_storage*>(argument(4)),
                          static_cast<socklen_t>(argument(5)));
    POSIX_CASE(POSIX_GETTIMEOFDAY)
      return posix_gettimeofday(reinterpret_cast<struct timeval*>(argument(0)),
                                reinterpret_cast<struct timezone*>(argument(1)));
    POSIX_CASE(POSIX_DUP)
      return posix_dup(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_DUP2)
      return posix_dup2(static_cast<int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_DUP3)
      return posix_dup3(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                        static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_LSTAT)
      return posix_lstat(reinterpret_cast<char*>(argument(0)),
                         reinterpret_cast<struct stat*>(argument(1)));
    POSIX_CASE(POSIX_UNLINK)
      return posix_unlink(reinterpret_cast<char*>(argument(0)));
    POSIX_CASE(POSIX_SYMLINK)
      return posix_symlink(reinterpret_cast<char*>(argument(0)),
                           reinterpret_cast<char*>(argument(1)));
    POSIX_CASE(POSIX_FCNTL)
      return posix_fcntl(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                         reinterpret_cast<void*>(argument(2)));
    POSIX_CASE(POSIX_PIPE)
      return posix_pipe(reinterpret_cast<int*>(argument(0)));
    POSIX_CASE(POSIX_PIPE2)
      return posix_pipe2(reinterpret_cast<int*>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_MKDIR)
      return posix_mkdir(reinterpret_cast<const char*>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_RMDIR)
      return posix_rmdir(reinterpret_cast<const char*>(argument(0)));
    case POSIX_GETPWENT:
      return posix_getpwent(reinterpret_cast<passwd*>(argument(0)), static_cast<int>(argument(1)),
                            reinterpret_cast<char*>(argument(2)));
    case POSIX_GETPWNAM:
      return posix_getpwnam(reinterpret_cast<passwd*>(argument(0)),
                            reinterpret_cast<const char*>(argument(1)),
                            reinterpret_cast<char*>(argument(2)));
    POSIX_CASE(POSIX_GETUID)
      return posix_getuid();
    POSIX_CASE(POSIX_GETGID)
      return posix_getgid();
    POSIX_CASE(POSIX_SIGACTION)
#if ARMV7
      if (linuxAbi) {
        if (argument(3) != sizeof(uint64_t)) {
          SYSCALL_ERROR(InvalidArgument);
          return -1;
        }
        return posix_linux_armv7_sigaction(
            static_cast<int>(argument(0)),
            reinterpret_cast<const LinuxArmv7KernelSigaction*>(argument(1)),
            reinterpret_cast<LinuxArmv7KernelSigaction*>(argument(2)));
      }
#endif
#if BITS_64
      if (linuxAbi) {
        if (argument(3) != sizeof(uint64_t)) {
          SYSCALL_ERROR(InvalidArgument);
          return -1;
        }
        return posix_linux_amd64_sigaction(
            static_cast<int>(argument(0)),
            reinterpret_cast<const LinuxAmd64KernelSigaction*>(argument(1)),
            reinterpret_cast<LinuxAmd64KernelSigaction*>(argument(2)));
      }
#endif
      return posix_sigaction(static_cast<int>(argument(0)),
                             reinterpret_cast<const struct sigaction*>(argument(1)),
                             reinterpret_cast<struct sigaction*>(argument(2)));
    case POSIX_SIGNAL:
      return posix_signal(static_cast<int>(argument(0)), reinterpret_cast<void*>(argument(1)));
    case POSIX_RAISE:
      return posix_raise(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_KILL)
      return posix_kill(static_cast<int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_TKILL)
      return posix_tkill(static_cast<int>(argument(0)), static_cast<int>(argument(1)), linuxAbi);
    POSIX_CASE(POSIX_TGKILL)
      return posix_tgkill(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                          static_cast<int>(argument(2)), linuxAbi);
    POSIX_CASE(POSIX_SIGPROCMASK)
      return posix_sigprocmask(static_cast<int>(argument(0)),
                               reinterpret_cast<const void*>(argument(1)),
                               reinterpret_cast<void*>(argument(2)), argument(3), linuxAbi);
    POSIX_CASE(POSIX_RT_SIGSUSPEND)
      return posix_rt_sigsuspend(reinterpret_cast<const uint64_t*>(argument(0)),
                                 static_cast<size_t>(argument(1)));
    POSIX_CASE(POSIX_ALARM)
      return posix_alarm(argument(0));
    case POSIX_SLEEP:
      return posix_sleep(argument(0));
    POSIX_CASE(POSIX_POLL)
      return posix_poll(reinterpret_cast<pollfd*>(argument(0)),
                        static_cast<unsigned int>(argument(1)), static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_PPOLL)
      return posix_ppoll(
          reinterpret_cast<pollfd*>(argument(0)), static_cast<unsigned int>(argument(1)),
          reinterpret_cast<LinuxKernelTimespec*>(argument(2)),
          reinterpret_cast<const uint64_t*>(argument(3)), static_cast<size_t>(argument(4)));
    POSIX_CASE(POSIX_PSELECT6)
      return posix_pselect6(static_cast<int>(argument(0)), reinterpret_cast<fd_set*>(argument(1)),
                            reinterpret_cast<fd_set*>(argument(2)),
                            reinterpret_cast<fd_set*>(argument(3)),
                            reinterpret_cast<LinuxKernelTimespec*>(argument(4)),
                            reinterpret_cast<const LinuxPselectSigsetArgument*>(argument(5)));
    POSIX_CASE(POSIX_EPOLL_CREATE)
      return posix_epoll_create(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_EPOLL_CREATE1)
      return posix_epoll_create1(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_EPOLL_CTL)
      return posix_epoll_ctl(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                             static_cast<int>(argument(2)),
                             reinterpret_cast<const LinuxEpollEvent*>(argument(3)));
    POSIX_CASE(POSIX_EPOLL_WAIT)
      return posix_epoll_wait(static_cast<int>(argument(0)),
                              reinterpret_cast<LinuxEpollEvent*>(argument(1)),
                              static_cast<int>(argument(2)), static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_EPOLL_PWAIT)
      return posix_epoll_pwait(
          static_cast<int>(argument(0)), reinterpret_cast<LinuxEpollEvent*>(argument(1)),
          static_cast<int>(argument(2)), static_cast<int>(argument(3)),
          reinterpret_cast<const void*>(argument(4)), static_cast<size_t>(argument(5)));
    POSIX_CASE(POSIX_EVENTFD)
      return posix_eventfd(static_cast<unsigned int>(argument(0)));
    POSIX_CASE(POSIX_EVENTFD2)
      return posix_eventfd2(static_cast<unsigned int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_SHMGET)
      return posix_shmget(static_cast<int32_t>(argument(0)), argument(1),
                          static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_SHMAT)
      return reinterpret_cast<uintptr_t>(posix_shmat(static_cast<int>(argument(0)),
                                                     reinterpret_cast<const void*>(argument(1)),
                                                     static_cast<int>(argument(2))));
    POSIX_CASE(POSIX_SHMDT)
      return posix_shmdt(reinterpret_cast<const void*>(argument(0)));
    POSIX_CASE(POSIX_SHMCTL)
      return posix_shmctl(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                          reinterpret_cast<void*>(argument(2)));

    POSIX_CASE(POSIX_RT_SIGPENDING)
      return posix_rt_sigpending(reinterpret_cast<uint64_t*>(argument(0)), argument(1));
    POSIX_CASE(POSIX_RT_SIGTIMEDWAIT)
      return posix_rt_sigtimedwait(reinterpret_cast<const uint64_t*>(argument(0)),
                                   reinterpret_cast<LinuxQueuedSiginfo*>(argument(1)),
                                   reinterpret_cast<const LinuxKernelTimespec*>(argument(2)),
                                   argument(3));
    POSIX_CASE(POSIX_RT_SIGQUEUEINFO)
      return posix_rt_sigqueueinfo(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                                   reinterpret_cast<const LinuxQueuedSiginfo*>(argument(2)));
    POSIX_CASE(POSIX_TIMER_CREATE)
      return posix_timer_create(static_cast<int>(argument(0)),
                                reinterpret_cast<const void*>(argument(1)),
                                reinterpret_cast<int*>(argument(2)));
    POSIX_CASE(POSIX_TIMER_SETTIME)
      return posix_timer_settime(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                                 reinterpret_cast<const void*>(argument(2)),
                                 reinterpret_cast<void*>(argument(3)));
    POSIX_CASE(POSIX_TIMER_GETTIME)
      return posix_timer_gettime(static_cast<int>(argument(0)),
                                 reinterpret_cast<void*>(argument(1)));
    POSIX_CASE(POSIX_TIMER_GETOVERRUN)
      return posix_timer_getoverrun(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_TIMER_DELETE)
      return posix_timer_delete(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_CLOCK_SETTIME)
      return posix_clock_settime(static_cast<clockid_t>(argument(0)),
                                 reinterpret_cast<const LinuxKernelTimespec*>(argument(1)));
    POSIX_CASE(POSIX_SIGNALFD)
      return posix_signalfd(static_cast<int>(argument(0)),
                            reinterpret_cast<const uint64_t*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_SIGNALFD4)
      return posix_signalfd4(static_cast<int>(argument(0)),
                             reinterpret_cast<const uint64_t*>(argument(1)), argument(2),
                             static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_TIMERFD_CREATE)
      return posix_timerfd_create(static_cast<int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_TIMERFD_SETTIME)
      return posix_timerfd_settime(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                                   reinterpret_cast<const void*>(argument(2)),
                                   reinterpret_cast<void*>(argument(3)));
    POSIX_CASE(POSIX_TIMERFD_GETTIME)
      return posix_timerfd_gettime(static_cast<int>(argument(0)),
                                   reinterpret_cast<void*>(argument(1)));

    POSIX_CASE(POSIX_SEMGET)
      return posix_semget(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                          static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_SEMOP)
      return posix_semop(static_cast<int>(argument(0)), reinterpret_cast<const void*>(argument(1)),
                         argument(2));
    POSIX_CASE(POSIX_SEMCTL)
      return posix_semctl(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                          static_cast<int>(argument(2)), argument(3));
    POSIX_CASE(POSIX_SEMTIMEDOP)
      return posix_semtimedop(static_cast<int>(argument(0)),
                              reinterpret_cast<const void*>(argument(1)), argument(2),
                              reinterpret_cast<const void*>(argument(3)));
    POSIX_CASE(POSIX_MSGGET)
      return posix_msgget(static_cast<int32_t>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_MSGSND)
      return posix_msgsnd(static_cast<int>(argument(0)), reinterpret_cast<const void*>(argument(1)),
                          argument(2), static_cast<int>(argument(3)));
    POSIX_CASE(POSIX_MSGRCV)
      return posix_msgrcv(static_cast<int>(argument(0)), reinterpret_cast<void*>(argument(1)),
                          argument(2), static_cast<int64_t>(argument(3)),
                          static_cast<int>(argument(4)));
    POSIX_CASE(POSIX_MSGCTL)
      return posix_msgctl(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                          reinterpret_cast<void*>(argument(2)));
    POSIX_CASE(POSIX_MQ_OPEN)
      return posix_mq_open(reinterpret_cast<const char*>(argument(0)),
                           static_cast<int>(argument(1)), static_cast<unsigned>(argument(2)),
                           reinterpret_cast<const LinuxMqAttr*>(argument(3)));
    POSIX_CASE(POSIX_MQ_UNLINK)
      return posix_mq_unlink(reinterpret_cast<const char*>(argument(0)));
    POSIX_CASE(POSIX_MQ_TIMEDSEND)
      return posix_mq_timedsend(static_cast<int>(argument(0)),
                                reinterpret_cast<const char*>(argument(1)), argument(2),
                                static_cast<unsigned>(argument(3)),
                                reinterpret_cast<const LinuxMqTimespec*>(argument(4)));
    POSIX_CASE(POSIX_MQ_TIMEDRECEIVE)
      return posix_mq_timedreceive(static_cast<int>(argument(0)),
                                   reinterpret_cast<char*>(argument(1)), argument(2),
                                   reinterpret_cast<unsigned*>(argument(3)),
                                   reinterpret_cast<const LinuxMqTimespec*>(argument(4)));
    POSIX_CASE(POSIX_MQ_NOTIFY)
      return posix_mq_notify(static_cast<int>(argument(0)),
                             reinterpret_cast<const LinuxMqSigevent*>(argument(1)));
    POSIX_CASE(POSIX_MQ_GETSETATTR)
      return posix_mq_getsetattr(static_cast<int>(argument(0)),
                                 reinterpret_cast<const LinuxMqAttr*>(argument(1)),
                                 reinterpret_cast<LinuxMqAttr*>(argument(2)));

    POSIX_CASE(POSIX_INOTIFY_INIT)
      return posix_inotify_init();
    POSIX_CASE(POSIX_INOTIFY_INIT1)
      return posix_inotify_init1(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_INOTIFY_ADD_WATCH)
      return posix_inotify_add_watch(static_cast<int>(argument(0)),
                                     reinterpret_cast<const char*>(argument(1)),
                                     static_cast<uint32_t>(argument(2)));
    POSIX_CASE(POSIX_INOTIFY_RM_WATCH)
      return posix_inotify_rm_watch(static_cast<int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_RENAME)
      return posix_rename(reinterpret_cast<const char*>(argument(0)),
                          reinterpret_cast<const char*>(argument(1)));
    POSIX_CASE(POSIX_GETCWD)
      return posix_getcwd(reinterpret_cast<char*>(argument(0)), argument(1));
    POSIX_CASE(POSIX_READLINK)
      return posix_readlink(reinterpret_cast<const char*>(argument(0)),
                            reinterpret_cast<char*>(argument(1)),
                            static_cast<unsigned int>(argument(2)));
    POSIX_CASE(POSIX_LINK)
      return posix_link(reinterpret_cast<char*>(argument(0)), reinterpret_cast<char*>(argument(1)));
    case POSIX_ISATTY:
      return posix_isatty(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_MMAP)
#if ARMV7
      if (linuxAbi) {
        return reinterpret_cast<uintptr_t>(posix_mmap(
            reinterpret_cast<void*>(argument(0)), argument(1), static_cast<int>(argument(2)),
            static_cast<int>(argument(3)), static_cast<int>(argument(4)),
            static_cast<off_t>(uint64_t(argument(5)) * PAGE_SIZE)));
      }
#endif
      return reinterpret_cast<uintptr_t>(
          posix_mmap(reinterpret_cast<void*>(argument(0)), argument(1),
                     static_cast<int>(argument(2)), static_cast<int>(argument(3)),
                     static_cast<int>(argument(4)), static_cast<off_t>(argument(5))));
    POSIX_CASE(POSIX_MUNMAP)
      return posix_munmap(reinterpret_cast<void*>(argument(0)), argument(1));
    POSIX_CASE(POSIX_MREMAP)
      return reinterpret_cast<uintptr_t>(
          posix_mremap(reinterpret_cast<void*>(argument(0)), argument(1), argument(2),
                       static_cast<int>(argument(3)), reinterpret_cast<void*>(argument(4))));
    POSIX_CASE(POSIX_MINCORE)
      return posix_mincore(reinterpret_cast<void*>(argument(0)), argument(1),
                           reinterpret_cast<unsigned char*>(argument(2)));
    POSIX_CASE(POSIX_MADVISE)
      return posix_madvise(reinterpret_cast<void*>(argument(0)), argument(1),
                           static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_REMAP_FILE_PAGES)
      return posix_remap_file_pages(reinterpret_cast<void*>(argument(0)), argument(1), argument(2),
                                    argument(3), argument(4));
    POSIX_CASE(POSIX_MLOCK)
      return posix_mlock(reinterpret_cast<const void*>(argument(0)), argument(1), 0);
    POSIX_CASE(POSIX_MLOCK2)
      return posix_mlock(reinterpret_cast<const void*>(argument(0)), argument(1),
                         static_cast<unsigned int>(argument(2)));
    POSIX_CASE(POSIX_MUNLOCK)
      return posix_munlock(reinterpret_cast<const void*>(argument(0)), argument(1));
    POSIX_CASE(POSIX_MLOCKALL)
      return posix_mlockall(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_MUNLOCKALL)
      return posix_munlockall();
    POSIX_CASE(POSIX_MEMFD_CREATE)
      return posix_memfd_create(reinterpret_cast<const char*>(argument(0)),
                                static_cast<unsigned int>(argument(1)));
    POSIX_CASE(POSIX_SENDFILE)
      return posix_sendfile(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                            reinterpret_cast<int64_t*>(argument(2)), argument(3));
    POSIX_CASE(POSIX_COPY_FILE_RANGE)
      return posix_copy_file_range(
          static_cast<int>(argument(0)), reinterpret_cast<int64_t*>(argument(1)),
          static_cast<int>(argument(2)), reinterpret_cast<int64_t*>(argument(3)), argument(4),
          static_cast<unsigned int>(argument(5)));
    POSIX_CASE(POSIX_SPLICE)
      return posix_splice(static_cast<int>(argument(0)), reinterpret_cast<int64_t*>(argument(1)),
                          static_cast<int>(argument(2)), reinterpret_cast<int64_t*>(argument(3)),
                          argument(4), static_cast<unsigned int>(argument(5)));
    POSIX_CASE(POSIX_TEE)
      return posix_tee(static_cast<int>(argument(0)), static_cast<int>(argument(1)), argument(2),
                       static_cast<unsigned int>(argument(3)));
    POSIX_CASE(POSIX_VMSPLICE)
      return posix_vmsplice(static_cast<int>(argument(0)),
                            reinterpret_cast<const struct iovec*>(argument(1)), argument(2),
                            static_cast<unsigned int>(argument(3)));
    POSIX_CASE(POSIX_SHUTDOWN)
      return posix_shutdown(static_cast<int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_ACCESS)
      return posix_access(reinterpret_cast<const char*>(argument(0)),
                          static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_SETSID)
      return posix_setsid();
    POSIX_CASE(POSIX_SETPGID)
      return posix_setpgid(static_cast<int>(argument(0)), static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_GETPGID)
      return posix_getpgid(static_cast<int>(argument(0)));
    POSIX_CASE(POSIX_GETPGRP)
      return posix_getpgrp();
    POSIX_CASE(POSIX_SIGALTSTACK)
      return posix_sigaltstack(reinterpret_cast<const stack_t*>(argument(0)),
                               reinterpret_cast<stack_t*>(argument(1)));

    case POSIX_SYSLOG:
      return posix_syslog(reinterpret_cast<const char*>(argument(0)),
                          static_cast<int>(argument(1)));

    POSIX_CASE(POSIX_FTRUNCATE)
      return posix_ftruncate(static_cast<int>(argument(0)), static_cast<off_t>(argument(1)));

    // Stub warning
    case POSIX_STUBBED:
      // This is the solution to a bug - if the first argument traps
      // (because of demand loading), it MUST trap before we get the log
      // spinlock, else other things will want to write to it and
      // deadlock.
      static char buf[128];
      StringCopyN(buf, reinterpret_cast<char*>(argument(0)), 128);
      WARNING("Using stubbed function '" << buf << "'");
      return 0;

    // POSIX-specific Pedigree system calls
    POSIX_CASE(PEDIGREE_SIGRET)
#if X64
      if (linuxAbi) {
        LinuxAmd64Signal::sigreturn(state);
        return 0;
      }
#endif
      return pedigree_sigret();
    case PEDIGREE_INIT_SIGRET:
      WARNING("POSIX: The 'init sigret' system call is no longer valid.");
      // pedigree_init_sigret();
      return 0;
    POSIX_CASE(POSIX_SCHED_YIELD)
      Scheduler::instance().yield();
      return 0;

    POSIX_CASE(POSIX_NANOSLEEP)
      return posix_nanosleep(reinterpret_cast<struct timespec*>(argument(0)),
                             reinterpret_cast<struct timespec*>(argument(1)),
                             linuxAbi && sizeof(uintptr_t) == sizeof(uint32_t));
    POSIX_CASE(POSIX_CLOCK_GETTIME)
      return posix_clock_gettime(argument(0), reinterpret_cast<struct timespec*>(argument(1)));
    POSIX_CASE(POSIX_CLOCK_GETRES)
      if (linuxAbi) {
        return posix_clock_getres(argument(0), reinterpret_cast<LinuxKernelTimespec*>(argument(1)));
      }
      return posix_clock_getres_native(argument(0),
                                       reinterpret_cast<struct timespec*>(argument(1)));
    POSIX_CASE(POSIX_CLOCK_NANOSLEEP)
      return posix_clock_nanosleep(argument(0), static_cast<int>(argument(1)),
                                   reinterpret_cast<const LinuxKernelTimespec*>(argument(2)),
                                   reinterpret_cast<LinuxKernelTimespec*>(argument(3)));

    POSIX_CASE(POSIX_GETEUID)
      return posix_geteuid();
    POSIX_CASE(POSIX_GETEGID)
      return posix_getegid();
    case POSIX_SETEUID:
      return posix_seteuid(static_cast<uid_t>(argument(0)));
    case POSIX_SETEGID:
      return posix_setegid(static_cast<gid_t>(argument(0)));
    POSIX_CASE(POSIX_SETUID)
      return posix_setuid(static_cast<uid_t>(argument(0)));
    POSIX_CASE(POSIX_SETGID)
      return posix_setgid(static_cast<gid_t>(argument(0)));

    POSIX_CASE(POSIX_CHOWN)
      return posix_chown(reinterpret_cast<const char*>(argument(0)),
                         static_cast<uid_t>(argument(1)), static_cast<gid_t>(argument(2)));
    POSIX_CASE(POSIX_CHMOD)
      return posix_chmod(reinterpret_cast<const char*>(argument(0)),
                         static_cast<mode_t>(argument(1)));
    POSIX_CASE(POSIX_FCHOWN)
      return posix_fchown(static_cast<int>(argument(0)), static_cast<uid_t>(argument(1)),
                          static_cast<gid_t>(argument(2)));
    POSIX_CASE(POSIX_FCHMOD)
      return posix_fchmod(static_cast<int>(argument(0)), static_cast<mode_t>(argument(1)));
    POSIX_CASE(POSIX_FCHDIR)
      return posix_fchdir(static_cast<int>(argument(0)));

    case POSIX_STATVFS:
      return posix_statvfs(reinterpret_cast<const char*>(argument(0)),
                           reinterpret_cast<struct statvfs*>(argument(1)));
    case POSIX_FSTATVFS:
      return posix_fstatvfs(static_cast<int>(argument(0)),
                            reinterpret_cast<struct statvfs*>(argument(1)));

    case PEDIGREE_UNWIND_SIGNAL:
      return pedigree_unwind_signal();

    POSIX_CASE(POSIX_MSYNC)
      return posix_msync(reinterpret_cast<void*>(argument(0)), argument(1),
                         static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_GETPEERNAME)
      return posix_getpeername(static_cast<int>(argument(0)),
                               reinterpret_cast<struct sockaddr_storage*>(argument(1)),
                               reinterpret_cast<socklen_t*>(argument(2)));
    POSIX_CASE(POSIX_GETSOCKNAME)
      return posix_getsockname(static_cast<int>(argument(0)),
                               reinterpret_cast<struct sockaddr_storage*>(argument(1)),
                               reinterpret_cast<socklen_t*>(argument(2)));
    POSIX_CASE(POSIX_FSYNC)
      return posix_fsync(static_cast<int>(argument(0)));

    case POSIX_PTSNAME:
      return console_ptsname(static_cast<int>(argument(0)), reinterpret_cast<char*>(argument(1)));
    POSIX_CASE(POSIX_TTYNAME)
      return console_ttyname(static_cast<int>(argument(0)), reinterpret_cast<char*>(argument(1)));
    case POSIX_TCGETPGRP:
      return posix_tcgetpgrp(static_cast<int>(argument(0)));
    case POSIX_TCSETPGRP:
      return posix_tcsetpgrp(static_cast<int>(argument(0)), static_cast<pid_t>(argument(1)));

    case POSIX_USLEEP:
      return posix_usleep(argument(0));

    POSIX_CASE(POSIX_MPROTECT)
      return posix_mprotect(reinterpret_cast<void*>(argument(0)), argument(1),
                            static_cast<int>(argument(2)));

    case POSIX_REALPATH:
      return posix_realpath(reinterpret_cast<const char*>(argument(0)),
                            reinterpret_cast<char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_TIMES)
      return posix_times(reinterpret_cast<struct tms*>(argument(0)));
    POSIX_CASE(POSIX_GETRUSAGE)
      if (linuxAbi) {
        return posix_linux_getrusage(argument(0), reinterpret_cast<LinuxRusage64*>(argument(1)));
      }
      return posix_getrusage(argument(0), reinterpret_cast<struct rusage*>(argument(1)));
    POSIX_CASE(POSIX_SETSOCKOPT)
      return posix_setsockopt(argument(0), argument(1), argument(2),
                              reinterpret_cast<const void*>(argument(3)), argument(4));
    POSIX_CASE(POSIX_GETSOCKOPT)
      return posix_getsockopt(argument(0), argument(1), argument(2),
                              reinterpret_cast<void*>(argument(3)),
                              reinterpret_cast<socklen_t*>(argument(4)));
    POSIX_CASE(POSIX_GETPPID)
      return posix_getppid();
    POSIX_CASE(POSIX_UTIME)
      return posix_utime(reinterpret_cast<const char*>(argument(0)),
                         reinterpret_cast<const struct utimbuf*>(argument(1)));
    POSIX_CASE(POSIX_UTIMES)
      return posix_utimes(reinterpret_cast<const char*>(argument(0)),
                          reinterpret_cast<const struct timeval*>(argument(1)));
    POSIX_CASE(POSIX_CHROOT)
      return posix_chroot(reinterpret_cast<const char*>(argument(0)));

    case POSIX_GETGRNAM:
      return posix_getgrnam(reinterpret_cast<const char*>(argument(0)),
                            reinterpret_cast<struct group*>(argument(1)));
    case POSIX_GETGRGID:
      return posix_getgrgid(static_cast<gid_t>(argument(0)),
                            reinterpret_cast<struct group*>(argument(1)));
    POSIX_CASE(POSIX_UMASK)
      return posix_umask(static_cast<mode_t>(argument(0)));
    POSIX_CASE(POSIX_WRITEV)
      return posix_writev(static_cast<int>(argument(0)),
                          reinterpret_cast<const struct iovec*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_READV)
      return posix_readv(static_cast<int>(argument(0)),
                         reinterpret_cast<const struct iovec*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_GETDENTS)
      return posix_getdents(static_cast<int>(argument(0)),
                            reinterpret_cast<struct linux_dirent*>(argument(1)),
                            static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_GETTID)
      return posix_gettid(linuxAbi);
    POSIX_CASE(POSIX_SET_TID_ADDRESS)
      return posix_set_tid_address(reinterpret_cast<int*>(argument(0)), linuxAbi);
    POSIX_CASE(POSIX_BRK)
      return posix_brk(argument(0));

    case POSIX_PEDIGREE_CREATE_WAITER:
      return reinterpret_cast<uintptr_t>(posix_pedigree_create_waiter());
    case POSIX_PEDIGREE_DESTROY_WAITER:
      posix_pedigree_destroy_waiter(reinterpret_cast<void*>(argument(0)));
      break;
    case POSIX_PEDIGREE_THREAD_WAIT_FOR:
      return posix_pedigree_thread_wait_for(reinterpret_cast<void*>(argument(0)));
    case POSIX_PEDIGREE_THREAD_TRIGGER:
      return posix_pedigree_thread_trigger(reinterpret_cast<void*>(argument(0)));

    case POSIX_PEDIGREE_GET_INFO_BLOCK:
      return VirtualAddressSpace::getKernelAddressSpace().getGlobalInfoBlock();

    POSIX_CASE(POSIX_SET_TLS_AREA) {
      const uintptr_t p1 = argument(0);
      if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(p1), &p1, sizeof(p1))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      Processor::information().getCurrentThread()->setTlsBase(p1);
#if X64 && !HOSTED
      auto metadata = state.getUserEntryMetadata();
      metadata.fsBase = p1;
      state.setUserEntryMetadata(metadata);
#endif
      return 0;
    }

    POSIX_CASE(POSIX_FUTEX)
      return posix_futex(reinterpret_cast<int*>(argument(0)), static_cast<int>(argument(1)),
                         static_cast<int>(argument(2)), argument(3),
                         reinterpret_cast<int*>(argument(4)), static_cast<int>(argument(5)));
    POSIX_CASE(POSIX_UNAME)
      return posix_uname(reinterpret_cast<struct utsname*>(argument(0)));
    POSIX_CASE(POSIX_ARCH_PRCTL) {
      const uintptr_t p1 = argument(0);
      const uintptr_t p2 = argument(1);
      const int result = posix_arch_prctl(p1, p2);
#if X64 && !HOSTED
      if (!result && (static_cast<int>(p1) == 0x1002 || static_cast<int>(p1) == 0x1001)) {
        // The return frame owns the base even if this syscall was preempted.
        auto metadata = state.getUserEntryMetadata();
        if (static_cast<int>(p1) == 0x1002)  // ARCH_SET_FS.
          metadata.fsBase = p2;
        else
          metadata.gsBase = p2;
        state.setUserEntryMetadata(metadata);
      }
#endif
      return result;
    }
    POSIX_CASE(POSIX_CLONE) {
#if ARM64
      // AArch64 passes TLS before the child TID address.
      return posix_clone(state, argument(0), reinterpret_cast<void*>(argument(1)),
                         reinterpret_cast<int*>(argument(2)), reinterpret_cast<int*>(argument(4)),
                         argument(3), linuxAbi);
#else
      return posix_clone(state, argument(0), reinterpret_cast<void*>(argument(1)),
                         reinterpret_cast<int*>(argument(2)), reinterpret_cast<int*>(argument(3)),
                         argument(4), linuxAbi);
#endif
    }
    POSIX_CASE(POSIX_CLONE3)
      return posix_clone3(state, reinterpret_cast<const LinuxCloneArgs*>(argument(0)), argument(1));
    POSIX_CASE(POSIX_PAUSE)
      return posix_pause();
    POSIX_CASE(POSIX_GETDENTS64)
      return posix_getdents64(static_cast<int>(argument(0)),
                              reinterpret_cast<struct dirent*>(argument(1)),
                              static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_L_SYSLOG)
      return posix_linux_syslog(argument(0), reinterpret_cast<char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_FLOCK)
      return posix_flock(argument(0), argument(1));
    POSIX_CASE(POSIX_OPENAT)
      return posix_openat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2),
                          argument(3));
    POSIX_CASE(POSIX_MKDIRAT)
      return posix_mkdirat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_FCHOWNAT)
      return posix_fchownat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2),
                            argument(3), argument(4));
    POSIX_CASE(POSIX_FUTIMESAT)
      return posix_futimesat(argument(0), reinterpret_cast<const char*>(argument(1)),
                             reinterpret_cast<struct timeval*>(argument(2)));
    POSIX_CASE(POSIX_UNLINKAT)
      return posix_unlinkat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_RENAMEAT)
      return posix_renameat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2),
                            reinterpret_cast<const char*>(argument(3)));
    POSIX_CASE(POSIX_LINKAT)
      return posix_linkat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2),
                          reinterpret_cast<const char*>(argument(3)), argument(4));
    POSIX_CASE(POSIX_SYMLINKAT)
      return posix_symlinkat(reinterpret_cast<const char*>(argument(0)), argument(1),
                             reinterpret_cast<const char*>(argument(2)));
    POSIX_CASE(POSIX_READLINKAT)
      return posix_readlinkat(argument(0), reinterpret_cast<const char*>(argument(1)),
                              reinterpret_cast<char*>(argument(2)), argument(3));
    POSIX_CASE(POSIX_FCHMODAT)
      return posix_fchmodat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2),
                            linuxAbi ? 0 : argument(3));
    POSIX_CASE(POSIX_FACCESSAT)
      return posix_faccessat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2),
                             0);
    POSIX_CASE(POSIX_FACCESSAT2)
      return posix_faccessat(argument(0), reinterpret_cast<const char*>(argument(1)), argument(2),
                             argument(3));
    POSIX_CASE(POSIX_FSTATAT)
      return posix_fstatat(argument(0), reinterpret_cast<const char*>(argument(1)),
                           reinterpret_cast<struct stat*>(argument(2)), argument(3));
    POSIX_CASE(POSIX_SETGROUPS)
      return posix_setgroups(argument(0), reinterpret_cast<const gid_t*>(argument(1)));
    POSIX_CASE(POSIX_GETRLIMIT)
      return posix_getrlimit(argument(0), reinterpret_cast<struct rlimit*>(argument(1)));
    POSIX_CASE(POSIX_GETPRIORITY)
      return posix_getpriority(argument(0), argument(1), linuxAbi);
    POSIX_CASE(POSIX_SETPRIORITY)
      return posix_setpriority(argument(0), argument(1), argument(2));
    POSIX_CASE(POSIX_SETXATTR)
      return posix_setxattr(reinterpret_cast<const char*>(argument(0)),
                            reinterpret_cast<const char*>(argument(1)),
                            reinterpret_cast<const void*>(argument(2)), argument(3), argument(4));
    POSIX_CASE(POSIX_LSETXATTR)
      return posix_lsetxattr(reinterpret_cast<const char*>(argument(0)),
                             reinterpret_cast<const char*>(argument(1)),
                             reinterpret_cast<const void*>(argument(2)), argument(3), argument(4));
    POSIX_CASE(POSIX_FSETXATTR)
      return posix_fsetxattr(argument(0), reinterpret_cast<const char*>(argument(1)),
                             reinterpret_cast<const void*>(argument(2)), argument(3), argument(4));
    POSIX_CASE(POSIX_GETXATTR)
      return posix_getxattr(reinterpret_cast<const char*>(argument(0)),
                            reinterpret_cast<const char*>(argument(1)),
                            reinterpret_cast<void*>(argument(2)), argument(3));
    POSIX_CASE(POSIX_LGETXATTR)
      return posix_lgetxattr(reinterpret_cast<const char*>(argument(0)),
                             reinterpret_cast<const char*>(argument(1)),
                             reinterpret_cast<void*>(argument(2)), argument(3));
    POSIX_CASE(POSIX_FGETXATTR)
      return posix_fgetxattr(argument(0), reinterpret_cast<const char*>(argument(1)),
                             reinterpret_cast<void*>(argument(2)), argument(3));
    POSIX_CASE(POSIX_LISTXATTR)
      return posix_listxattr(reinterpret_cast<const char*>(argument(0)),
                             reinterpret_cast<char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_LLISTXATTR)
      return posix_llistxattr(reinterpret_cast<const char*>(argument(0)),
                              reinterpret_cast<char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_FLISTXATTR)
      return posix_flistxattr(argument(0), reinterpret_cast<char*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_REMOVEXATTR)
      return posix_removexattr(reinterpret_cast<const char*>(argument(0)),
                               reinterpret_cast<const char*>(argument(1)));
    POSIX_CASE(POSIX_LREMOVEXATTR)
      return posix_lremovexattr(reinterpret_cast<const char*>(argument(0)),
                                reinterpret_cast<const char*>(argument(1)));
    POSIX_CASE(POSIX_FANOTIFY_INIT)
      return posix_fanotify_init(argument(0), argument(1));
    POSIX_CASE(POSIX_FANOTIFY_MARK)
      return posix_fanotify_mark(argument(0), argument(1), argument(2), argument(3),
                                 reinterpret_cast<const char*>(argument(4)));
    POSIX_CASE(POSIX_NAME_TO_HANDLE_AT)
      return posix_name_to_handle_at(argument(0), reinterpret_cast<const char*>(argument(1)),
                                     reinterpret_cast<void*>(argument(2)),
                                     reinterpret_cast<int*>(argument(3)), argument(4));
    POSIX_CASE(POSIX_OPEN_BY_HANDLE_AT)
      return posix_open_by_handle_at(argument(0), reinterpret_cast<const void*>(argument(1)),
                                     argument(2));
    POSIX_CASE(POSIX_SETFSUID)
      return posix_setfsuid(argument(0));
    POSIX_CASE(POSIX_SETFSGID)
      return posix_setfsgid(argument(0));
    POSIX_CASE(POSIX_PROCESS_VM_READV)
      return posix_process_vm_readv(argument(0), reinterpret_cast<const iovec*>(argument(1)),
                                    argument(2), reinterpret_cast<const iovec*>(argument(3)),
                                    argument(4), argument(5));
    POSIX_CASE(POSIX_PROCESS_VM_WRITEV)
      return posix_process_vm_writev(argument(0), reinterpret_cast<const iovec*>(argument(1)),
                                     argument(2), reinterpret_cast<const iovec*>(argument(3)),
                                     argument(4), argument(5));
    POSIX_CASE(POSIX_FREMOVEXATTR)
      return posix_fremovexattr(argument(0), reinterpret_cast<const char*>(argument(1)));
    POSIX_CASE(POSIX_MKNOD)
      return posix_mknod(reinterpret_cast<const char*>(argument(0)), argument(1), argument(2));
    POSIX_CASE(POSIX_SETREUID)
      return posix_setreuid(argument(0), argument(1));
    POSIX_CASE(POSIX_SETREGID)
      return posix_setregid(argument(0), argument(1));
    POSIX_CASE(POSIX_SETRESUID)
      return posix_setresuid(argument(0), argument(1), argument(2));
    POSIX_CASE(POSIX_SETRESGID)
      return posix_setresgid(argument(0), argument(1), argument(2));
    POSIX_CASE(POSIX_GETRESUID)
      return posix_getresuid(reinterpret_cast<uid_t*>(argument(0)),
                             reinterpret_cast<uid_t*>(argument(1)),
                             reinterpret_cast<uid_t*>(argument(2)));
    POSIX_CASE(POSIX_GETRESGID)
      return posix_getresgid(reinterpret_cast<gid_t*>(argument(0)),
                             reinterpret_cast<gid_t*>(argument(1)),
                             reinterpret_cast<gid_t*>(argument(2)));
    POSIX_CASE(POSIX_STATFS)
      return posix_statfs(reinterpret_cast<const char*>(argument(0)),
                          reinterpret_cast<struct statfs*>(argument(1)));
    POSIX_CASE(POSIX_FSTATFS)
      return posix_fstatfs(argument(0), reinterpret_cast<struct statfs*>(argument(1)));
    POSIX_CASE(POSIX_UNSHARE)
      return posix_unshare(argument(0));
    POSIX_CASE(POSIX_SETNS)
      return posix_setns(argument(0), argument(1));
    POSIX_CASE(POSIX_SETDOMAINNAME)
      return posix_setdomainname(reinterpret_cast<const char*>(argument(0)), argument(1));
    POSIX_CASE(POSIX_SCHED_SETPARAM)
      return posix_sched_setparam(argument(0), reinterpret_cast<const void*>(argument(1)));
    POSIX_CASE(POSIX_SCHED_GETPARAM)
      return posix_sched_getparam(argument(0), reinterpret_cast<void*>(argument(1)));
    POSIX_CASE(POSIX_SCHED_SETSCHEDULER)
      return posix_sched_setscheduler(argument(0), argument(1),
                                      reinterpret_cast<const void*>(argument(2)));
    POSIX_CASE(POSIX_SCHED_GETSCHEDULER)
      return posix_sched_getscheduler(argument(0));
    POSIX_CASE(POSIX_SCHED_GET_PRIORITY_MAX)
      return posix_sched_get_priority_max(argument(0));
    POSIX_CASE(POSIX_SCHED_GET_PRIORITY_MIN)
      return posix_sched_get_priority_min(argument(0));
    POSIX_CASE(POSIX_SCHED_RR_GET_INTERVAL)
      return posix_sched_rr_get_interval(argument(0), reinterpret_cast<void*>(argument(1)));
    POSIX_CASE(POSIX_SCHED_SETAFFINITY)
      return posix_sched_setaffinity(argument(0), argument(1),
                                     reinterpret_cast<const void*>(argument(2)));
    POSIX_CASE(POSIX_SCHED_GETAFFINITY)
      return posix_sched_getaffinity(argument(0), argument(1),
                                     reinterpret_cast<void*>(argument(2)));
    POSIX_CASE(POSIX_GETCPU)
      return posix_getcpu(reinterpret_cast<unsigned int*>(argument(0)),
                          reinterpret_cast<unsigned int*>(argument(1)));
    POSIX_CASE(POSIX_SETHOSTNAME)
      return posix_sethostname(reinterpret_cast<const char*>(argument(0)), argument(1));
    POSIX_CASE(POSIX_IOPERM)
      return posix_ioperm(argument(0), argument(1), argument(2));
    POSIX_CASE(POSIX_IOPL)
      return posix_iopl(argument(0));
    POSIX_CASE(POSIX_CREAT)
      return posix_open(reinterpret_cast<const char*>(argument(0)), O_WRONLY | O_CREAT | O_TRUNC,
                        argument(1));
    POSIX_CASE(POSIX_SET_ROBUST_LIST)
      return posix_set_robust_list(reinterpret_cast<struct robust_list_head*>(argument(0)),
                                   argument(1), linuxAbi);
    POSIX_CASE(POSIX_GET_ROBUST_LIST)
      return posix_get_robust_list(argument(0),
                                   reinterpret_cast<struct robust_list_head**>(argument(1)),
                                   reinterpret_cast<size_t*>(argument(2)), linuxAbi);
    POSIX_CASE(POSIX_GETGROUPS)
      return posix_getgroups(argument(0), reinterpret_cast<gid_t*>(argument(1)));
    POSIX_CASE(POSIX_MOUNT) {
      return posix_mount(reinterpret_cast<const char*>(argument(0)),
                         reinterpret_cast<const char*>(argument(1)),
                         reinterpret_cast<const char*>(argument(2)), argument(3),
                         reinterpret_cast<const void*>(argument(4)));
    }
    POSIX_CASE(POSIX_UMOUNT2)
      return posix_umount2(reinterpret_cast<const char*>(argument(0)),
                           static_cast<int>(argument(1)));
    POSIX_CASE(POSIX_SETTIMEOFDAY)
      return posix_settimeofday(reinterpret_cast<const struct timeval*>(argument(0)),
                                reinterpret_cast<const struct timezone*>(argument(1)));
    POSIX_CASE(POSIX_SETRLIMIT)
      return posix_setrlimit(argument(0), reinterpret_cast<const struct rlimit*>(argument(1)));
    POSIX_CASE(POSIX_PRLIMIT64)
      return posix_prlimit64(static_cast<int>(argument(0)), static_cast<int>(argument(1)),
                             reinterpret_cast<const LinuxRlimit64*>(argument(2)),
                             reinterpret_cast<LinuxRlimit64*>(argument(3)));
    POSIX_CASE(POSIX_MEMBARRIER)
      return posix_membarrier(static_cast<int>(argument(0)), static_cast<unsigned int>(argument(1)),
                              static_cast<int>(argument(2)));
    POSIX_CASE(POSIX_TIME)
      return posix_time(reinterpret_cast<time_t*>(argument(0)));
    POSIX_CASE(POSIX_GETITIMER)
      return posix_getitimer(argument(0), reinterpret_cast<struct itimerval*>(argument(1)));
    POSIX_CASE(POSIX_SETITIMER)
      return posix_setitimer(argument(0), reinterpret_cast<const struct itimerval*>(argument(1)),
                             reinterpret_cast<struct itimerval*>(argument(2)));
    POSIX_CASE(POSIX_SOCKETPAIR)
      return posix_socketpair(argument(0), argument(1), argument(2),
                              reinterpret_cast<int*>(argument(3)));
    POSIX_CASE(POSIX_SENDMSG)
      return posix_sendmsg(argument(0), reinterpret_cast<const struct msghdr*>(argument(1)),
                           argument(2));
    POSIX_CASE(POSIX_RECVMSG)
      return posix_recvmsg(argument(0), reinterpret_cast<struct msghdr*>(argument(1)), argument(2));
    POSIX_CASE(POSIX_CAPGET)
      return posix_capget(reinterpret_cast<void*>(argument(0)),
                          reinterpret_cast<void*>(argument(1)));
    POSIX_CASE(POSIX_CAPSET)
      return posix_capset(reinterpret_cast<void*>(argument(0)),
                          reinterpret_cast<const void*>(argument(1)));
    POSIX_CASE(POSIX_PRCTL)
      return posix_prctl(argument(0), argument(1), argument(2), argument(3), argument(4));
    POSIX_CASE(POSIX_REBOOT)
      return linuxAbi ? posix_reboot(argument(0), argument(1), argument(2)) : pedigree_reboot();
    POSIX_CASE(POSIX_GETRANDOM)
      return posix_getrandom(reinterpret_cast<void*>(argument(0)), static_cast<size_t>(argument(1)),
                             static_cast<unsigned int>(argument(2)));

    default:
      ERROR("PosixSyscallManager: invalid syscall received: " << Dec << syscallNumber << Hex);
      SYSCALL_ERROR(Unimplemented);
      return -1;
  }
#undef POSIX_CASE

  return 0;
}

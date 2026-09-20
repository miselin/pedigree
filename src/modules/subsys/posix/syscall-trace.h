#ifndef POSIX_SYSCALL_TRACE_H
#define POSIX_SYSCALL_TRACE_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/time/Time.h"

#include "PosixSubsystem.h"
#include "syscalls/translate.h"

namespace {
class SyscallTrace {
 public:
  SyscallTrace(SyscallState& state, Thread& thread) {
    static uint64_t nextId = 0;
    m_Id = __atomic_add_fetch(&nextId, static_cast<uint64_t>(1), __ATOMIC_RELAXED);

    const bool linuxAbi = state.getSyscallService() == linuxCompat;
    const uintptr_t number = state.getSyscallNumber();
    const size_t base = linuxAbi ? 6 : 0;
    uintptr_t arguments[6];
    for (size_t i = 0; i < 6; ++i) {
      arguments[i] = state.getSyscallParameter(base + i);
    }

    NOTICE("STRACE E id=" << Hex << m_Id << " pid=" << thread.getParent()->getId()
                          << " tid=" << thread.getId() << " abi=" << (linuxAbi ? "L" : "P")
                          << " nr=" << number);
    for (size_t i = 0; i < 6; i += 3) {
      NOTICE("STRACE A id=" << Hex << m_Id << " n=" << i << " x=" << arguments[i]
                            << " y=" << arguments[i + 1] << " z=" << arguments[i + 2]);
    }

    if (linuxAbi) {
      const unsigned paths = pathArguments(number);
      for (size_t i = 0; i < 6; ++i) {
        if (paths & (1U << i)) {
          path(i, reinterpret_cast<const char*>(arguments[i]));
        }
      }
    }
  }

  void finish(uintptr_t result, int error, Time::Timestamp elapsed) const {
    // Exec and exit return a deferred action here, not a userspace return value.
    NOTICE("STRACE X id=" << Hex << m_Id << " raw=" << result
                          << " err=" << static_cast<unsigned>(error) << " ns=" << elapsed);
  }

 private:
  static unsigned pathArguments(uintptr_t number) {
    switch (number) {
      case PedigreeLinuxAmd64Syscall_open:
      case PedigreeLinuxAmd64Syscall_stat:
      case PedigreeLinuxAmd64Syscall_lstat:
      case PedigreeLinuxAmd64Syscall_access:
      case PedigreeLinuxAmd64Syscall_execve:
      case PedigreeLinuxAmd64Syscall_chdir:
      case PedigreeLinuxAmd64Syscall_mkdir:
      case PedigreeLinuxAmd64Syscall_rmdir:
      case PedigreeLinuxAmd64Syscall_creat:
      case PedigreeLinuxAmd64Syscall_unlink:
      case PedigreeLinuxAmd64Syscall_readlink:
      case PedigreeLinuxAmd64Syscall_chmod:
      case PedigreeLinuxAmd64Syscall_chown:
      case PedigreeLinuxAmd64Syscall_lchown:
      case PedigreeLinuxAmd64Syscall_utime:
      case PedigreeLinuxAmd64Syscall_utimes:
      case PedigreeLinuxAmd64Syscall_statfs:
      case PedigreeLinuxAmd64Syscall_chroot:
      case PedigreeLinuxAmd64Syscall_truncate:
      case PedigreeLinuxAmd64Syscall_mknod:
      case PedigreeLinuxAmd64Syscall_umount2:
      case PedigreeLinuxAmd64Syscall_swapon:
      case PedigreeLinuxAmd64Syscall_swapoff:
      case PedigreeLinuxAmd64Syscall_acct:
      case PedigreeLinuxAmd64Syscall_setxattr:
      case PedigreeLinuxAmd64Syscall_lsetxattr:
      case PedigreeLinuxAmd64Syscall_getxattr:
      case PedigreeLinuxAmd64Syscall_lgetxattr:
      case PedigreeLinuxAmd64Syscall_listxattr:
      case PedigreeLinuxAmd64Syscall_llistxattr:
      case PedigreeLinuxAmd64Syscall_removexattr:
      case PedigreeLinuxAmd64Syscall_lremovexattr:
        return 1U;
      case PedigreeLinuxAmd64Syscall_rename:
      case PedigreeLinuxAmd64Syscall_link:
      case PedigreeLinuxAmd64Syscall_symlink:
      case PedigreeLinuxAmd64Syscall_pivot_root:
      case PedigreeLinuxAmd64Syscall_mount:
        return (1U << 0) | (1U << 1);
      case PedigreeLinuxAmd64Syscall_openat:
      case PedigreeLinuxAmd64Syscall_mkdirat:
      case PedigreeLinuxAmd64Syscall_fchownat:
      case PedigreeLinuxAmd64Syscall_futimesat:
      case PedigreeLinuxAmd64Syscall_newfstatat:
      case PedigreeLinuxAmd64Syscall_unlinkat:
      case PedigreeLinuxAmd64Syscall_readlinkat:
      case PedigreeLinuxAmd64Syscall_fchmodat:
      case PedigreeLinuxAmd64Syscall_faccessat:
      case PedigreeLinuxAmd64Syscall_faccessat2:
      case PedigreeLinuxAmd64Syscall_execveat:
      case PedigreeLinuxAmd64Syscall_utimensat:
      case PedigreeLinuxAmd64Syscall_statx:
      case PedigreeLinuxAmd64Syscall_fchmodat2:
      case PedigreeLinuxAmd64Syscall_mknodat:
      case PedigreeLinuxAmd64Syscall_name_to_handle_at:
      case PedigreeLinuxAmd64Syscall_inotify_add_watch:
        return 1U << 1;
      case PedigreeLinuxAmd64Syscall_renameat:
      case PedigreeLinuxAmd64Syscall_linkat:
      case PedigreeLinuxAmd64Syscall_renameat2:
        return (1U << 1) | (1U << 3);
      case PedigreeLinuxAmd64Syscall_symlinkat:
        return (1U << 0) | (1U << 2);
      default:
        return 0;
    }
  }

  void path(size_t argument, const char* address) const {
    String copy;
    const auto result = PosixSubsystem::copyUserString(address, copy, 256);
    const char* status = result == PosixSubsystem::UserStringSuccess   ? "ok"
                         : result == PosixSubsystem::UserStringTooLong ? "truncated"
                                                                       : "invalid";
    constexpr char digits[] = "0123456789abcdef";
    // Each chunk fits LOG_LENGTH even with a full-width record identifier.
    for (size_t offset = 0; offset < copy.length(); offset += 32) {
      const size_t remaining = copy.length() - offset;
      const size_t count = remaining < 32 ? remaining : 32;
      char data[65];
      for (size_t i = 0; i < count; ++i) {
        const uint8_t byte = static_cast<uint8_t>(copy[offset + i]);
        data[i * 2] = digits[byte >> 4];
        data[i * 2 + 1] = digits[byte & 15];
      }
      data[count * 2] = '\0';
      NOTICE("STRACE P id=" << Hex << m_Id << " arg=" << argument << " off=" << offset
                            << " data=" << data);
    }
    NOTICE("STRACE S id=" << Hex << m_Id << " arg=" << argument << " status=" << status);
  }

  uint64_t m_Id;
};
}  // namespace

#endif

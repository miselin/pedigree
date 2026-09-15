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

#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/machine/Keyboard.h"
#include "pedigree/kernel/machine/KeymapManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include <FileDescriptor.h>
#include <PosixProcess.h>
#include <PosixSubsystem.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>
#include <utime.h>

#include "DevFs-block.h"
#include "TerminalControl.h"
#include "advisory-lock-syscalls.h"
#include "console-syscalls.h"
#include "eventfd-syscalls.h"
#include "fanotify-syscalls.h"
#include "file-metadata.h"
#include "file-syscalls.h"
#include "inotify-syscalls.h"
#include "memfd-syscalls.h"
#include "metadata-syscalls.h"
#include "modules/subsys/posix/IoEvent.h"
#include "modules/system/console/Console.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/users/Group.h"
#include "modules/system/users/User.h"
#include "modules/system/users/UserManager.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/LockedFile.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/Pipe.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "namespace-file.h"
#include "net-syscalls.h"
#include "pipe-syscalls.h"
#include "signalfd-syscalls.h"
#include "timerfd-syscalls.h"
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>

// Emits a lot of logs in normalisePath to help debug remaps.
#define ENABLE_VERBOSE_NORMALISATION 0

extern int posix_getpid();

// For getdents() (getdents64 uses a compatible struct dirent).
struct linux_dirent {
  long d_ino;
  off_t d_off;
  unsigned short d_reclen;
  char d_name[1];
};

extern DevFs* g_pDevFs;

//
// Syscalls pertaining to files.
//

#define CHECK_FLAG(a, b) (((a) & (b)) == (b))

static PosixProcess* getPosixProcess() {
  Process* pStockProcess = Processor::information().getCurrentThread()->getParent();
  if (pStockProcess->getType() != Process::Posix) {
    return 0;
  }

  PosixProcess* pProcess = static_cast<PosixProcess*>(pStockProcess);
  return pProcess;
}

static bool copyUserString(const char* userString, String& copy) {
  PosixSubsystem::UserStringResult result =
      PosixSubsystem::copyUserString(userString, copy, PATH_MAX);
  if (result == PosixSubsystem::UserStringBadAddress) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if (result == PosixSubsystem::UserStringTooLong) {
    SYSCALL_ERROR(NameTooLong);
    return false;
  }
  return true;
}

File* findFilePath(const String& name, ResolvedPath& result, const FilesystemPathRef& start,
                   bool followFinal) {
  Process* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  return subsystem ? subsystem->findFileRetained(name, result, start, followFinal) : nullptr;
}

File* followFilePath(ResolvedPath& selected) {
  Process* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  return subsystem ? subsystem->followFile(selected) : nullptr;
}

static File* traverseSymlink(File* file, ResolvedPath& selected) {
  if (!file) {
    if (!Processor::information().getCurrentThread()->getErrno())
      SYSCALL_ERROR(DoesNotExist);
    return nullptr;
  }
  if (!file->isSymlink())
    return file;
  if (selected.get() != file) {
    SYSCALL_ERROR(DoesNotExist);
    return nullptr;
  }
  return followFilePath(selected);
}

static bool doChdir(File* dir, ResolvedPath& selected) {
  File* target = traverseSymlink(dir, selected);
  if (!target)
    return false;
  if (!target->isDirectory() || selected.get() != target) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto context = process->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  return view && context && view->changeCwd(context, selected.path());
}

static bool doStat(const char* name, File* file, struct stat* output) {
  return posix_stat_file(name, file, output);
}

// NON-special-case remappings.
static struct Remapping {
  // from must match either completely, or be followed by a "/"
  const char* from;
  const char* to;
  const char* fsname;  // certain remaps are to be reported as custom FS's to
                       // some ABIs
  bool all_abis;       // certain ABIs shouldn't normalise certain paths
  bool on_devfs;       // certain callers care about the result being on devfs
} g_Remappings[] = {
    {"/dev", "/dev", "dev", true, true},
    {"/proc", "/proc", "proc", true, false},
    {"/tmp", "/tmp", "tmpfs", true, false},
    {"/run", "/run", "tmpfs", true, false},
    {"/var/run", "/run", "tmpfs", true, false},
    // Temporary compatibility for packages built against Pedigree's old
    // non-FHS root. PUP extracts through these syscalls, so old package members
    // are installed into the canonical namespace while packages are rebuilt.
    {"/.profile", "/root/.profile", nullptr, true, false},
    {"/.bashrc", "/root/.bashrc", nullptr, true, false},
    {"/support/pup/db", "/var/cache/pup", nullptr, true, false},
    {"/support/pup", "/etc/pup", nullptr, true, false},
    {"/system/initscripts", "/etc/init.d", nullptr, true, false},
    {"/system/modules", "/usr/lib/modules", nullptr, true, false},
    {"/system/include", "/usr/include", nullptr, true, false},
    {"/system/locale", "/usr/share/locale", nullptr, true, false},
    {"/system/keymaps", "/usr/share/keymaps", nullptr, true, false},
    {"/system/fonts", "/usr/share/fonts", nullptr, true, false},
    {"/applications", "/usr/bin", nullptr, true, false},
    {"/libraries", "/usr/lib", nullptr, true, false},
    {"/initscripts", "/etc/init.d", nullptr, true, false},
    {"/config", "/etc", nullptr, true, false},
    {"/support", "/usr/lib/pedigree", nullptr, true, false},
    {"/include", "/usr/include", nullptr, true, false},
    {"/users", "/home", nullptr, true, false},
    {"/fonts", "/usr/share/fonts", nullptr, true, false},
    {"/docs", "/usr/share/doc", nullptr, true, false},
    {"/doc", "/usr/share/doc", nullptr, true, false},
    {nullptr, nullptr, nullptr, false, false},
};

bool normalisePath(String& nameToOpen, const char* name, bool* onDevFs) {
  if (!name || !name[0]) {
    nameToOpen.clear();
    if (onDevFs) {
      *onDevFs = false;
    }
    return true;
  }
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  // Compatibility mappings apply consistently while old PUP packages are
  // still in circulation.
  bool fixFilesystemPaths = true;

  // /dev/tty is special because it can refer to the controlling terminal.
  // Each compatibility rule must match a complete path component, so a
  // legacy name never rewrites a merely similar modern path.
  if (!StringCompare(name, "/dev/tty")) {
    // Get controlling console, unless we have none.
    auto cttyContext = pProcess->acquireCttyContext();
    if (!cttyContext || !cttyContext->file()) {
      if (onDevFs)
        *onDevFs = true;
    }

    nameToOpen.assign(name);
    return true;
  } else {
    // try the remappings
    struct Remapping* remap = g_Remappings;
#if ENABLE_VERBOSE_NORMALISATION
    F_NOTICE("performing remap for '" << name << "'...");
#endif
    bool ok = false;
    while (remap->from != nullptr) {
      if (!(fixFilesystemPaths || remap->all_abis)) {
#if ENABLE_VERBOSE_NORMALISATION
        F_NOTICE(" -> ignoring " << remap->from << " as it is not for the current ABI");
#endif
        ++remap;
        continue;
      }

#if ENABLE_VERBOSE_NORMALISATION
      F_NOTICE(" -> check against " << remap->from);
#endif
      if (!StringCompare(name, remap->from)) {
#if ENABLE_VERBOSE_NORMALISATION
        F_NOTICE(" -> direct remap to " << remap->to);
#endif
        nameToOpen.assign(remap->to);
        ok = true;
        break;
      }

      // does not match directly, so we need to check for a partial match
      if (!StringCompareN(name, remap->from, StringLength(remap->from))) {
#if ENABLE_VERBOSE_NORMALISATION
        F_NOTICE(" -> possibly partial remap");
#endif

        // we have a partial match, but this only OK if the following
        // character is '/' to avoid incorrectly rewriting paths
        if (*(name + StringLength(remap->from)) == '/') {
          // good
          nameToOpen.assign(remap->to);
          nameToOpen += (name + StringLength(remap->from));
#if ENABLE_VERBOSE_NORMALISATION
          F_NOTICE(" -> indirect remap to create path '" << nameToOpen << "'...");
#endif
          ok = true;
          break;
        }

// no good
#if ENABLE_VERBOSE_NORMALISATION
        NOTICE(
            " -> cannot use this remap as it is not actually "
            "matching a path segment");
#endif
      }

      ++remap;
    }

    if (onDevFs && remap) {
      *onDevFs = remap->on_devfs;
    }

    if (!ok) {
      nameToOpen.assign(name);
      return false;
    }

    return true;
  }
}

int posix_close(int fd) {
#if VERBOSE_KERNEL
  F_NOTICE("close(" << fd << ")");
#endif
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

#if !VERBOSE_KERNEL
  F_NOTICE("close(" << fd << ")");
#endif

  // Remove only the generation we acquired. Another thread can close this
  // descriptor and reuse the numeric fd while this path is still running;
  // an unconditional free would then close the replacement by mistake.
  if (!pSubsystem->closeFileDescriptor(fd, pFd)) {
    return 0;
  }

  // If this was a master psuedoterminal, we should unlock it now.
  if (ConsoleManager::instance().isConsole(pFd->getFile())) {
    if (ConsoleManager::instance().isMasterConsole(pFd->getFile())) {
      ConsoleManager::instance().unlockConsole(pFd->getFile());
    }
  }

  return 0;
}

int posix_open(const char* name, int flags, int mode) {
  return posix_openat(AT_FDCWD, name, flags, mode);
}

namespace {
constexpr size_t ScalarIoBounceCapacity = PIPE_BUF_MAX + 1;
constexpr size_t RegularReadBounceCapacity = 64 * 1024;

#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
Process::BenchmarkVmCounter vmLengthCounter(bool unmap, size_t pages) {
  const size_t bucket = pages <= 1     ? 0
                        : pages <= 3   ? 1
                        : pages <= 15  ? 2
                        : pages <= 63  ? 3
                        : pages <= 255 ? 4
                                       : 5;
  const auto first = unmap ? Process::VmMunmapLength1 : Process::VmMmapLength1;
  return static_cast<Process::BenchmarkVmCounter>(static_cast<size_t>(first) + bucket);
}
#endif

UniqueArray<uint8_t> allocateReadBounce(File* file, size_t length, size_t& capacity) {
  const bool diskBackedRegular = file->supportsRegularFileOperations() && !file->isBlockDevice() &&
                                 file->getFilesystem() && file->getFilesystem()->getDisk();
  const size_t limit = diskBackedRegular ? RegularReadBounceCapacity : ScalarIoBounceCapacity;
  capacity = length < limit ? length : limit;
  UniqueArray<uint8_t> bounce = UniqueArray<uint8_t>::allocate(capacity);
  if (!bounce && capacity > ScalarIoBounceCapacity) {
    capacity = ScalarIoBounceCapacity;
    bounce = UniqueArray<uint8_t>::allocate(capacity);
  }
  return bounce;
}

bool checkReadDestination(void* destination, size_t& requested) {
  if (PosixSubsystem::checkUserBuffer(reinterpret_cast<uintptr_t>(destination), requested, 1,
                                      PosixSubsystem::SafeWrite)) {
    return true;
  }
  if (requested <= ScalarIoBounceCapacity) {
    return false;
  }

  // A larger precheck must not reject the valid prefix that smaller reads delivered.
  requested = ScalarIoBounceCapacity;
  return PosixSubsystem::checkUserBuffer(reinterpret_cast<uintptr_t>(destination), requested, 1,
                                         PosixSubsystem::SafeWrite);
}

bool scalarIoRangeDoesNotWrap(const void* buffer, size_t length) {
  if (!length) {
    return true;
  }

  const uintptr_t address = reinterpret_cast<uintptr_t>(buffer);
  return address && length - 1 <= (~static_cast<uintptr_t>(0) - address);
}

bool positionalIoRangeIsValid(off_t offset, size_t length) {
  if (offset < 0 || length > static_cast<size_t>(SSIZE_MAX)) {
    return false;
  }

  const uint64_t location = static_cast<uint64_t>(offset);
  return static_cast<uint64_t>(length) <= static_cast<uint64_t>(INT64_MAX) - location;
}
}  // namespace

int posix_read(int fd, char* ptr, int len) {
  F_NOTICE("read(" << Dec << fd << Hex << ", " << reinterpret_cast<uintptr_t>(ptr) << ", " << len
                   << ")");
  if (len < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // Lookup this process.
  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (pFd->getFile() &&
      ((pFd->getStatusFlags() & O_PATH) || (pFd->getStatusFlags() & O_ACCMODE) == O_WRONLY)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  auto timerFd = pFd->getTimerFdImpl();
  auto signalFd = pFd->getSignalFdImpl();
  if (timerFd || signalFd) {
    const bool canBlock = !(pFd->getStatusFlags() & O_NONBLOCK);
    pFd.reset();
    return timerFd ? timerFd->readToUser(ptr, len, canBlock)
                   : signalFd->readToUser(ptr, len, canBlock);
  }

  SharedPointer<EventFd> eventFd = pFd->getEventFdImpl();
  if (eventFd) {
    if (len < static_cast<int>(sizeof(uint64_t))) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!PosixSubsystem::checkUserBuffer(reinterpret_cast<uintptr_t>(ptr), sizeof(uint64_t), 1,
                                         PosixSubsystem::SafeWrite)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    const bool canBlock = !(pFd->getStatusFlags() & O_NONBLOCK);
    pFd.reset();
    uint64_t value = 0;
    const int result = eventFd->readValue(value, canBlock);
    if (result < 0) {
      return result;
    }
    if (!PosixSubsystem::copyToUser(ptr, &value, sizeof(value))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return result;
  }

  SharedPointer<InotifyInstance> inotify = pFd->getInotifyImpl();
  if (inotify) {
    const size_t length = static_cast<size_t>(len);
    const bool canBlock = !(pFd->getStatusFlags() & O_NONBLOCK);
    pFd.reset();
    return inotify->readEventsToUser(reinterpret_cast<uint8_t*>(ptr), length, canBlock);
  }
  auto fanotify = pFd->getFanotifyImpl();
  if (fanotify) {
    const bool canBlock = !(pFd->getStatusFlags() & O_NONBLOCK);
    pFd.reset();
    return fanotify->readToUser(ptr, static_cast<size_t>(len), canBlock);
  }

  if (pFd->networkImpl) {
    // Need to redirect to socket implementation.
    if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ptr), static_cast<size_t>(len),
                                      PosixSubsystem::SafeWrite)) {
      F_NOTICE("  -> invalid address");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return posix_recv_descriptor(pFd, ptr, len, 0);
  }

  if (!pFd->getFile()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (pFd->getFile()->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }

  if (!len) {
    return 0;
  }

  const size_t length = static_cast<size_t>(len);
  if (!scalarIoRangeDoesNotWrap(ptr, length)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  size_t bounceCapacity = 0;
  UniqueArray<uint8_t> bounce = allocateReadBounce(pFd->getFile(), length, bounceCapacity);
  if (!bounce) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }

  auto readFile = [&](FileDescriptor::PositionGuard* position, int statusFlags) -> int {
    const bool canBlock = !(statusFlags & O_NONBLOCK);
    size_t totalRead = 0;

    while (totalRead < length) {
      if (totalRead && pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
        break;
      }

      const size_t remaining = length - totalRead;
      size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
      char* userDestination = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(ptr) + totalRead);

      // Avoid consuming data for an address which is already known to be
      // unusable. copyToUser repeats this check after a blocking operation.
      if (!checkReadDestination(userDestination, requested)) {
        if (totalRead) {
          pThread->clearInterruption();
          return static_cast<int>(totalRead);
        }
        pThread->clearInterruption();
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      // A nonseekable operation may block once, but must not block again
      // after it has already made progress during this syscall.
      const bool operationCanBlock = canBlock && (position || !totalRead);
      if (!operationCanBlock) {
        const ReadyMask ready = pFd->acquireOpenFileDescription()->queryFileReady(true, false);
        if (!(ready & (ReadyRead | ReadyError | ReadyHangup))) {
          if (totalRead) {
            break;
          }
          pThread->clearInterruption();
          SYSCALL_ERROR(NoMoreProcesses);
          F_NOTICE(" -> async and nothing available to read");
          return -1;
        }
      }

      if (pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
        if (totalRead) {
          break;
        }
        pThread->clearInterruption();
        SYSCALL_ERROR(Interrupted);
        return -1;
      }

      uint64_t amount = 0;
      if (position) {
        amount = pFd->getFile()->read(position->offset(), requested,
                                      reinterpret_cast<uintptr_t>(bounce.get()), operationCanBlock);
      } else {
        amount = pFd->readFile(0, requested, reinterpret_cast<uintptr_t>(bounce.get()),
                               operationCanBlock);
      }
      const bool signalInterrupted =
          pThread->getInterruptionReason() == Thread::InterruptedBySignal;

      if (!amount && pFd->terminalHungUp())
        break;
      if (!amount) {
        if (!totalRead && signalInterrupted) {
          pThread->clearInterruption();
          SYSCALL_ERROR(Interrupted);
          F_NOTICE(" -> interrupted");
          return -1;
        }
        break;
      }

      if (!PosixSubsystem::copyToUser(userDestination, bounce.get(), amount)) {
        if (totalRead) {
          pThread->clearInterruption();
          return static_cast<int>(totalRead);
        }
        pThread->clearInterruption();
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      if (position) {
        position->advanceOffset(amount);
      }
      totalRead += amount;
      if (amount < requested || signalInterrupted ||
          pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
        break;
      }
    }

    pThread->clearInterruption();
    F_NOTICE("    -> " << Dec << totalRead << Hex);
    return static_cast<int>(totalRead);
  };

  pThread->clearInterruption();
  if (pFd->getFile()->isSeekable()) {
    FileDescriptor::PositionGuard position = pFd->lockPosition();
    return readFile(&position, position.statusFlags());
  }

  // A blocking nonseekable read must not hold the OFD metadata mutex needed
  // by a writer using the same O_RDWR description.
  return readFile(nullptr, pFd->getStatusFlags());
}

int posix_write(int fd, char* ptr, int len, bool nocheck) {
  F_NOTICE("write(" << fd << ", " << reinterpret_cast<uintptr_t>(ptr) << ", " << len << ")");
  if (len < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // Lookup this process.
  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (pFd->getFile() &&
      ((pFd->getStatusFlags() & O_PATH) || (pFd->getStatusFlags() & O_ACCMODE) == O_RDONLY)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (pFd->getTimerFdImpl() || pFd->getSignalFdImpl() || pFd->getFanotifyImpl()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  SharedPointer<EventFd> eventFd = pFd->getEventFdImpl();
  if (eventFd) {
    if (len != static_cast<int>(sizeof(uint64_t))) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

    uint64_t value = 0;
    if (nocheck) {
      ForwardMemoryCopy(&value, ptr, sizeof(value));
    } else if (!PosixSubsystem::copyFromUser(&value, ptr, sizeof(value))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    const bool canBlock = !(pFd->getStatusFlags() & O_NONBLOCK);
    pFd.reset();
    return eventFd->writeValue(value, canBlock);
  }

  if (pFd->getInotifyImpl()) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (pFd->networkImpl) {
    // Need to redirect to socket implementation.
    if (!nocheck &&
        !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ptr), static_cast<size_t>(len),
                                      PosixSubsystem::SafeRead)) {
      F_NOTICE("  -> invalid address");
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return posix_send_descriptor(pFd, ptr, len, 0, nocheck);
  }

  if (!pFd->getFile()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (!len) {
    if (pFd->terminalHungUp()) {
      SYSCALL_ERROR(IoError);
      return -1;
    }
    return 0;
  }

  const bool pipeLike = pFd->getFile()->isPipe() || pFd->getFile()->isFifo();
  const size_t length = static_cast<size_t>(len);
  if (!scalarIoRangeDoesNotWrap(ptr, length)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  const size_t bounceCapacity = length < ScalarIoBounceCapacity ? length : ScalarIoBounceCapacity;
  UniqueArray<uint8_t> bounce = UniqueArray<uint8_t>::allocate(bounceCapacity);
  bool deliverPipeSignal = false;

  auto writeFile = [&](FileDescriptor::PositionGuard* position, int statusFlags) -> int {
    const bool canBlock = !(statusFlags & O_NONBLOCK);
    File::WriteGuard writeGuard = pFd->getFile()->lockWrites();
    size_t totalWritten = 0;

    while (totalWritten < length) {
      if (totalWritten && pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
        break;
      }

      const size_t remaining = length - totalWritten;
      const size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
      const char* userSource =
          reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(ptr) + totalWritten);

      if (nocheck) {
        ForwardMemoryCopy(bounce.get(), userSource, requested);
      } else if (!PosixSubsystem::copyFromUser(bounce.get(), userSource, requested)) {
        if (totalWritten) {
          pThread->clearInterruption();
          return static_cast<int>(totalWritten);
        }
        pThread->clearInterruption();
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      if (pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
        if (totalWritten) {
          break;
        }
        pThread->clearInterruption();
        SYSCALL_ERROR(Interrupted);
        return -1;
      }

      pThread->setErrno(0);
      uint64_t amount = 0;
      if (position) {
        uint64_t location = position->offset();
        amount = (statusFlags & O_APPEND)
                     ? writeGuard.append(requested, reinterpret_cast<uintptr_t>(bounce.get()),
                                         location, canBlock)
                     : writeGuard.write(location, requested,
                                        reinterpret_cast<uintptr_t>(bounce.get()), canBlock);
        if (amount) {
          position->setOffset(location + amount);
        }
      } else if (ConsoleManager::instance().isConsole(pFd->getFile())) {
        amount = pFd->writeFile(0, requested, reinterpret_cast<uintptr_t>(bounce.get()), canBlock);
      } else {
        amount =
            writeGuard.write(0, requested, reinterpret_cast<uintptr_t>(bounce.get()), canBlock);
      }
      const bool signalInterrupted =
          pThread->getInterruptionReason() == Thread::InterruptedBySignal;
      const size_t backendError = pThread->getErrno();

      if (!amount) {
        if (totalWritten) {
          pThread->setErrno(0);
          break;
        }
        if (signalInterrupted && !pFd->terminalHungUp()) {
          pThread->clearInterruption();
          SYSCALL_ERROR(Interrupted);
          F_NOTICE(" -> interrupted");
          return -1;
        }
        if (backendError) {
          pThread->clearInterruption();
          deliverPipeSignal = pipeLike && backendError == Error::BrokenPipe;
          return -1;
        }
        if (pipeLike && !Pipe::fromFile(pFd->getFile())->getReaderCount()) {
          pThread->clearInterruption();
          F_NOTICE("  -> write to a broken pipe");
          SYSCALL_ERROR(BrokenPipe);
          deliverPipeSignal = true;
          return -1;
        }
        if (!canBlock) {
          pThread->clearInterruption();
          SYSCALL_ERROR(NoMoreProcesses);
          return -1;
        }
        if (pipeLike) {
          pThread->clearInterruption();
          F_NOTICE("  -> write to a broken pipe");
          SYSCALL_ERROR(BrokenPipe);
          deliverPipeSignal = true;
          return -1;
        }
        break;
      }

      pThread->setErrno(0);
      totalWritten += amount;
      if (amount < requested || signalInterrupted ||
          pThread->getInterruptionReason() == Thread::InterruptedBySignal) {
        break;
      }
    }

    pThread->clearInterruption();
    F_NOTICE("  -> write returns " << totalWritten);
    return static_cast<int>(totalWritten);
  };

  pThread->clearInterruption();
  int result = 0;
  if (pFd->getFile()->isSeekable()) {
    {
      FileDescriptor::PositionGuard position = pFd->lockPosition();
      result = writeFile(&position, position.statusFlags());
    }
  } else {
    // See the matching read path: a blocking nonseekable write must not
    // monopolize the OFD metadata mutex needed by its peer.
    result = writeFile(nullptr, pFd->getStatusFlags());
  }

  if (deliverPipeSignal) {
    pSubsystem->threadException(pThread, Subsystem::Pipe);
  }
  return result;
}

ssize_t posix_pread64(int fd, char* ptr, size_t len, off_t offset) {
  F_NOTICE("pread64(" << Dec << fd << Hex << ", " << reinterpret_cast<uintptr_t>(ptr) << ", " << len
                      << ", " << offset << ")");
  if (!positionalIoRangeIsValid(offset, len)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (!descriptor->getFile() || !descriptor->getFile()->isSeekable()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }
  const int statusFlags = descriptor->getStatusFlags();
  if ((statusFlags & O_PATH) || (statusFlags & O_ACCMODE) == O_WRONLY) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (descriptor->getFile()->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }
  if (!len) {
    return 0;
  }
  if (!scalarIoRangeDoesNotWrap(ptr, len)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  size_t bounceCapacity = 0;
  UniqueArray<uint8_t> bounce = allocateReadBounce(descriptor->getFile(), len, bounceCapacity);
  if (!bounce) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const bool canBlock = !(statusFlags & O_NONBLOCK);
  const uint64_t startingOffset = static_cast<uint64_t>(offset);
  size_t totalRead = 0;

  thread->clearInterruption();
  while (totalRead < len) {
    if (totalRead && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      break;
    }

    const size_t remaining = len - totalRead;
    size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
    char* userDestination = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(ptr) + totalRead);
    if (!checkReadDestination(userDestination, requested)) {
      if (totalRead) {
        thread->clearInterruption();
        return static_cast<ssize_t>(totalRead);
      }
      thread->clearInterruption();
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      if (totalRead) {
        break;
      }
      thread->clearInterruption();
      SYSCALL_ERROR(Interrupted);
      return -1;
    }

    const uint64_t amount = descriptor->getFile()->read(
        startingOffset + totalRead, requested, reinterpret_cast<uintptr_t>(bounce.get()), canBlock);
    const bool signalInterrupted = thread->getInterruptionReason() == Thread::InterruptedBySignal;
    if (!amount) {
      if (!totalRead && signalInterrupted) {
        thread->clearInterruption();
        SYSCALL_ERROR(Interrupted);
        return -1;
      }
      break;
    }

    if (!PosixSubsystem::copyToUser(userDestination, bounce.get(), amount)) {
      if (totalRead) {
        thread->clearInterruption();
        return static_cast<ssize_t>(totalRead);
      }
      thread->clearInterruption();
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    totalRead += amount;
    if (amount < requested || signalInterrupted ||
        thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      break;
    }
  }

  thread->clearInterruption();
  return static_cast<ssize_t>(totalRead);
}

ssize_t posix_pwrite64(int fd, const char* ptr, size_t len, off_t offset) {
  F_NOTICE("pwrite64(" << Dec << fd << Hex << ", " << reinterpret_cast<uintptr_t>(ptr) << ", "
                       << len << ", " << offset << ")");
  if (!positionalIoRangeIsValid(offset, len)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (!descriptor->getFile() || !descriptor->getFile()->isSeekable()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }
  const int statusFlags = descriptor->getStatusFlags();
  if ((statusFlags & O_PATH) || (statusFlags & O_ACCMODE) == O_RDONLY) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (descriptor->getFile()->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }
  if (!len) {
    return 0;
  }
  if (!scalarIoRangeDoesNotWrap(ptr, len)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  const size_t bounceCapacity = len < ScalarIoBounceCapacity ? len : ScalarIoBounceCapacity;
  UniqueArray<uint8_t> bounce = UniqueArray<uint8_t>::allocate(bounceCapacity);
  const bool canBlock = !(statusFlags & O_NONBLOCK);
  const uint64_t startingOffset = static_cast<uint64_t>(offset);
  File::WriteGuard writeGuard = descriptor->getFile()->lockWrites();
  size_t totalWritten = 0;

  thread->clearInterruption();
  while (totalWritten < len) {
    if (totalWritten && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      break;
    }

    const size_t remaining = len - totalWritten;
    const size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
    const char* userSource =
        reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(ptr) + totalWritten);
    if (!PosixSubsystem::copyFromUser(bounce.get(), userSource, requested)) {
      if (totalWritten) {
        thread->clearInterruption();
        return static_cast<ssize_t>(totalWritten);
      }
      thread->clearInterruption();
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      if (totalWritten) {
        break;
      }
      thread->clearInterruption();
      SYSCALL_ERROR(Interrupted);
      return -1;
    }

    thread->setErrno(0);
    const uint64_t amount = writeGuard.write(startingOffset + totalWritten, requested,
                                             reinterpret_cast<uintptr_t>(bounce.get()), canBlock);
    const bool signalInterrupted = thread->getInterruptionReason() == Thread::InterruptedBySignal;
    const size_t backendError = thread->getErrno();
    if (!amount) {
      if (!totalWritten && signalInterrupted) {
        thread->clearInterruption();
        SYSCALL_ERROR(Interrupted);
        return -1;
      }
      if (totalWritten) {
        thread->setErrno(0);
        break;
      }
      if (backendError) {
        thread->clearInterruption();
        return -1;
      }
      if (!canBlock) {
        thread->clearInterruption();
        SYSCALL_ERROR(NoMoreProcesses);
        return -1;
      }
      break;
    }

    thread->setErrno(0);
    totalWritten += amount;
    if (amount < requested || signalInterrupted ||
        thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      break;
    }
  }

  thread->clearInterruption();
  return static_cast<ssize_t>(totalWritten);
}

enum class VectorPayloadValidation {
  Full,
  AddressRange,
  CommitTime,
  BenchmarkEligible,
};

static bool snapshotIoVectors(
    const struct iovec* userVectors, int vectorCount, bool writeOperation,
    UniqueArray<struct iovec>& vectorOwner, size_t& totalLength,
    VectorPayloadValidation payloadValidation = VectorPayloadValidation::Full) {
  constexpr int MaximumIoVectors = 1024;
  if (vectorCount < 0 || vectorCount > MaximumIoVectors) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  totalLength = 0;
  if (!vectorCount) {
    return true;
  }

  vectorOwner = UniqueArray<struct iovec>::allocate(static_cast<size_t>(vectorCount));
  struct iovec* vectors = vectorOwner.get();
  if (!PosixSubsystem::copyFromUser(vectors, userVectors, static_cast<size_t>(vectorCount),
                                    sizeof(struct iovec))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }

  bool validatePayload = payloadValidation != VectorPayloadValidation::CommitTime;
  bool rangeOnly = payloadValidation == VectorPayloadValidation::AddressRange;
#if PEDIGREE_BENCHMARK_VM_ABLATIONS
  Process* process = Processor::information().getCurrentThread()->getParent();
  const bool skipPayloadValidation =
      payloadValidation == VectorPayloadValidation::BenchmarkEligible &&
      process->benchmarkVmAblationEnabled(Process::AblateVectorPayloadValidation);
  if (skipPayloadValidation) {
    rangeOnly = true;
  }
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  size_t skippedPayloadChecks = 0;
#endif
#endif
  const size_t access = writeOperation ? PosixSubsystem::SafeRead : PosixSubsystem::SafeWrite;
  for (int i = 0; i < vectorCount; ++i) {
    if (vectors[i].iov_len > static_cast<size_t>(INT_MAX) - totalLength) {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
#if PEDIGREE_BENCHMARK_VM_ABLATIONS && PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    if (vectors[i].iov_len && skipPayloadValidation) {
      ++skippedPayloadChecks;
    }
#endif
    if (vectors[i].iov_len && validatePayload) {
      const uintptr_t base = reinterpret_cast<uintptr_t>(vectors[i].iov_base);
      const bool valid = rangeOnly
                             ? PosixSubsystem::checkUserAddressRange(base, vectors[i].iov_len, 1)
                             : PosixSubsystem::checkUserBuffer(base, vectors[i].iov_len, 1, access);
      if (!valid) {
        SYSCALL_ERROR(BadAddress);
        return false;
      }
    }
    totalLength += vectors[i].iov_len;
  }
#if PEDIGREE_BENCHMARK_VM_ABLATIONS && PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmAblationVectorPayloadChecksSkipped,
                                    skippedPayloadChecks);
#endif
  return true;
}

static int writeFileVectorElement(Thread* thread, const DescriptorLease& descriptor,
                                  FileDescriptor::PositionGuard* position, int statusFlags,
                                  File::WriteGuard& writeGuard, const void* buffer, size_t length,
                                  bool reportError, bool& signalInterrupted,
                                  bool& deliverPipeSignal) {
  File* file = descriptor->getFile();
  const bool canBlock = !(statusFlags & O_NONBLOCK);

  thread->setErrno(0);
  uint64_t written = 0;
  if (file->isSeekable()) {
    assert(position);
    uint64_t location = position->offset();
    written =
        (statusFlags & O_APPEND)
            ? writeGuard.append(length, reinterpret_cast<uintptr_t>(buffer), location, canBlock)
            : writeGuard.write(location, length, reinterpret_cast<uintptr_t>(buffer), canBlock);
    if (written) {
      position->setOffset(location + written);
    }
  } else if (ConsoleManager::instance().isConsole(file)) {
    written = descriptor->writeFile(0, length, reinterpret_cast<uintptr_t>(buffer), canBlock);
  } else {
    written = writeGuard.write(0, length, reinterpret_cast<uintptr_t>(buffer), canBlock);
  }

  signalInterrupted = thread->getInterruptionReason() == Thread::InterruptedBySignal;
  const size_t backendError = thread->getErrno();
  thread->clearInterruption();
  if (!written && signalInterrupted && !descriptor->terminalHungUp()) {
    if (reportError) {
      SYSCALL_ERROR(Interrupted);
    } else {
      thread->setErrno(0);
    }
    return -1;
  }
  if (!written && backendError) {
    deliverPipeSignal = (file->isPipe() || file->isFifo()) && backendError == Error::BrokenPipe;
    if (!reportError) {
      thread->setErrno(0);
    }
    return -1;
  }
  thread->setErrno(0);

  const bool pipeLike = file->isPipe() || file->isFifo();
  if (!canBlock && !written && length && (!pipeLike || Pipe::fromFile(file)->getReaderCount())) {
    if (reportError) {
      SYSCALL_ERROR(NoMoreProcesses);
    }
    return -1;
  }
  if (pipeLike && !written && length) {
    if (reportError) {
      SYSCALL_ERROR(BrokenPipe);
    }
    deliverPipeSignal = true;
    return -1;
  }
  return static_cast<int>(written);
}

static int readFileVectorElement(Thread* thread, const DescriptorLease& descriptor,
                                 FileDescriptor::PositionGuard* position, int statusFlags,
                                 void* buffer, size_t length, bool reportError,
                                 bool& signalInterrupted) {
  File* file = descriptor->getFile();
  const bool canBlock = !(statusFlags & O_NONBLOCK);

  uint64_t amount = 0;
  if (file->isSeekable()) {
    assert(position);
    amount = file->read(position->offset(), length, reinterpret_cast<uintptr_t>(buffer), canBlock);
  } else {
    amount = descriptor->readFile(0, length, reinterpret_cast<uintptr_t>(buffer), canBlock);
  }

  signalInterrupted = thread->getInterruptionReason() == Thread::InterruptedBySignal;
  thread->clearInterruption();
  if (!amount && signalInterrupted && !descriptor->terminalHungUp()) {
    if (reportError) {
      SYSCALL_ERROR(Interrupted);
    }
    return -1;
  }
  return static_cast<int>(amount);
}

static bool scatterEventFdValue(const struct iovec* vectors, int vectorCount, uint64_t value) {
  size_t copied = 0;
  const uint8_t* source = reinterpret_cast<const uint8_t*>(&value);
  for (int i = 0; i < vectorCount && copied < sizeof(value); ++i) {
    size_t fragment = vectors[i].iov_len;
    if (fragment > sizeof(value) - copied) {
      fragment = sizeof(value) - copied;
    }
    if (fragment && !PosixSubsystem::copyToUser(vectors[i].iov_base, source + copied, fragment)) {
      SYSCALL_ERROR(BadAddress);
      return false;
    }
    copied += fragment;
  }
  return copied == sizeof(value);
}

static int posixWritev(int fd, const struct iovec* iov, int iovcnt, bool suppressAppend) {
  F_NOTICE("writev(" << fd << ", <iov>, " << iovcnt << ")");

  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (!subsystem) {
    return -1;
  }

  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (descriptor->getFile() && ((descriptor->getStatusFlags() & O_PATH) ||
                                (descriptor->getStatusFlags() & O_ACCMODE) == O_RDONLY)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  const VectorPayloadValidation payloadValidation =
      descriptor->getFile() && descriptor->getFile()->isSeekable()
          ? VectorPayloadValidation::AddressRange
          : VectorPayloadValidation::BenchmarkEligible;
  UniqueArray<struct iovec> vectorOwner;
  size_t totalLength = 0;
  if (!snapshotIoVectors(iov, iovcnt, true, vectorOwner, totalLength, payloadValidation)) {
    return -1;
  }
  struct iovec* vectors = vectorOwner.get();

  if (!iovcnt) {
    return 0;
  }
  if (!totalLength) {
    return 0;
  }

  if (descriptor->getTimerFdImpl() || descriptor->getSignalFdImpl() ||
      descriptor->getFanotifyImpl()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  SharedPointer<EventFd> eventFd = descriptor->getEventFdImpl();
  if (eventFd) {
    const bool canBlock = !(descriptor->getStatusFlags() & O_NONBLOCK);
    descriptor.reset();

    int totalWritten = 0;
    for (int i = 0; i < iovcnt; ++i) {
      if (!vectors[i].iov_len) {
        continue;
      }
      if (vectors[i].iov_len != sizeof(uint64_t)) {
        SYSCALL_ERROR(InvalidArgument);
        return totalWritten ? totalWritten : -1;
      }

      uint64_t value = 0;
      if (!PosixSubsystem::copyFromUser(&value, vectors[i].iov_base, sizeof(value))) {
        SYSCALL_ERROR(BadAddress);
        return totalWritten ? totalWritten : -1;
      }

      const int written = eventFd->writeValue(value, canBlock);
      if (written < 0) {
        return totalWritten ? totalWritten : written;
      }
      totalWritten += written;
    }
    return totalWritten;
  }

  if (descriptor->networkImpl) {
    struct msghdr message = {};
    message.msg_iov = vectors;
    message.msg_iovlen = static_cast<size_t>(iovcnt);
    return static_cast<int>(posix_sendmsg_descriptor(descriptor, &message));
  }
  if (!descriptor->getFile()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  bool deliverPipeSignal = false;
  auto writeVector = [&](FileDescriptor::PositionGuard* position, int statusFlags) -> int {
    const bool pipeLike = descriptor->getFile()->isPipe() || descriptor->getFile()->isFifo();
    if (pipeLike && totalLength <= PIPE_BUF_MAX) {
      UniqueArray<uint8_t> aggregate = UniqueArray<uint8_t>::allocate(totalLength);
      size_t offset = 0;
      for (int i = 0; i < iovcnt; ++i) {
        if (vectors[i].iov_len &&
            !PosixSubsystem::copyFromUser(aggregate.get() + offset, vectors[i].iov_base,
                                          vectors[i].iov_len)) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        offset += vectors[i].iov_len;
      }
      File::WriteGuard writeGuard = descriptor->getFile()->lockWrites();
      if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
        thread->clearInterruption();
        SYSCALL_ERROR(Interrupted);
        return -1;
      }
      bool signalInterrupted = false;
      return totalLength ? writeFileVectorElement(thread, descriptor, position, statusFlags,
                                                  writeGuard, aggregate.get(), totalLength, true,
                                                  signalInterrupted, deliverPipeSignal)
                         : 0;
    }

    const size_t bounceCapacity =
        totalLength < ScalarIoBounceCapacity ? totalLength : ScalarIoBounceCapacity;
    UniqueArray<uint8_t> bounce = UniqueArray<uint8_t>::allocate(bounceCapacity);
    File::WriteGuard writeGuard = descriptor->getFile()->lockWrites();
    for (int i = 0; i < iovcnt; ++i) {
      F_NOTICE("writev: iov[" << i << "] is @ " << vectors[i].iov_base << ", " << vectors[i].iov_len
                              << " bytes.");
    }

    int totalWritten = 0;
    int vectorIndex = 0;
    size_t vectorOffset = 0;
    while (static_cast<size_t>(totalWritten) < totalLength) {
      if (totalWritten && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
        thread->clearInterruption();
        return totalWritten;
      }

      const size_t remaining = totalLength - static_cast<size_t>(totalWritten);
      const size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
      size_t gathered = 0;
      while (gathered < requested) {
        while (vectorIndex < iovcnt && vectorOffset == vectors[vectorIndex].iov_len) {
          ++vectorIndex;
          vectorOffset = 0;
        }
        assert(vectorIndex < iovcnt);

        const size_t vectorRemaining = vectors[vectorIndex].iov_len - vectorOffset;
        const size_t fragment =
            vectorRemaining < requested - gathered ? vectorRemaining : requested - gathered;
        const char* userSource = reinterpret_cast<const char*>(
            reinterpret_cast<uintptr_t>(vectors[vectorIndex].iov_base) + vectorOffset);
        if (!PosixSubsystem::copyFromUser(bounce.get() + gathered, userSource, fragment)) {
          thread->clearInterruption();
          if (totalWritten) {
            return totalWritten;
          }
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        gathered += fragment;
        vectorOffset += fragment;
      }

      if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
        thread->clearInterruption();
        if (totalWritten) {
          return totalWritten;
        }
        SYSCALL_ERROR(Interrupted);
        return -1;
      }

      bool signalInterrupted = false;
      const int r = writeFileVectorElement(thread, descriptor, position, statusFlags, writeGuard,
                                           bounce.get(), requested, totalWritten == 0,
                                           signalInterrupted, deliverPipeSignal);
      if (r < 0) {
        return totalWritten ? totalWritten : r;
      }

      totalWritten += r;
      if (static_cast<size_t>(r) < requested || signalInterrupted) {
        return totalWritten;
      }
    }

    return totalWritten;
  };

  thread->clearInterruption();
  int result = 0;
  if (descriptor->getFile()->isSeekable()) {
    {
      FileDescriptor::PositionGuard position = descriptor->lockPosition();
      int statusFlags = position.statusFlags();
      if (suppressAppend) {
        statusFlags &= ~O_APPEND;
      }
      result = writeVector(&position, statusFlags);
    }
  } else {
    // A blocking nonseekable write must not hold the OFD metadata mutex needed
    // by a reader using the same O_RDWR description.
    result = writeVector(nullptr, descriptor->getStatusFlags());
  }

  if (deliverPipeSignal) {
    subsystem->threadException(thread, Subsystem::Pipe);
  }
  return result;
}

int posix_writev(int fd, const struct iovec* iov, int iovcnt) {
  return posixWritev(fd, iov, iovcnt, false);
}

int posix_readv(int fd, const struct iovec* iov, int iovcnt) {
  F_NOTICE("readv(" << fd << ", <iov>, " << iovcnt << ")");

  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (!subsystem) {
    return -1;
  }

  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (descriptor->getFile() && ((descriptor->getStatusFlags() & O_PATH) ||
                                (descriptor->getStatusFlags() & O_ACCMODE) == O_WRONLY)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  auto timerFd = descriptor->getTimerFdImpl();
  auto signalFd = descriptor->getSignalFdImpl();
  auto fanotify = descriptor->getFanotifyImpl();
  const VectorPayloadValidation payloadValidation =
      timerFd || signalFd || fanotify ? VectorPayloadValidation::CommitTime
      : descriptor->getFile() && descriptor->getFile()->isSeekable()
          ? VectorPayloadValidation::AddressRange
          : VectorPayloadValidation::BenchmarkEligible;
  UniqueArray<struct iovec> vectorOwner;
  size_t totalLength = 0;
  // Record readers validate each destination at commit time. A later fault
  // must not suppress complete records copied before it.
  if (!snapshotIoVectors(iov, iovcnt, false, vectorOwner, totalLength, payloadValidation)) {
    return -1;
  }
  struct iovec* vectors = vectorOwner.get();
  if (!iovcnt) {
    return 0;
  }
  if (!totalLength) {
    return 0;
  }

  if (timerFd || signalFd || fanotify) {
    const bool canBlock = !(descriptor->getStatusFlags() & O_NONBLOCK);
    descriptor.reset();
    struct Scatter {
      struct iovec* vectors;
      int count;
      int index = 0;
      size_t offset = 0;
    } scatter{vectors, iovcnt};
    auto copy = [](void* context, const void* data, size_t size) -> bool {
      auto& cursor = *static_cast<Scatter*>(context);
      auto* bytes = static_cast<const uint8_t*>(data);
      while (size) {
        while (cursor.index < cursor.count &&
               cursor.offset == cursor.vectors[cursor.index].iov_len) {
          ++cursor.index;
          cursor.offset = 0;
        }
        if (cursor.index == cursor.count) {
          SYSCALL_ERROR(BadAddress);
          return false;
        }
        const struct iovec& vector = cursor.vectors[cursor.index];
        const size_t available = vector.iov_len - cursor.offset;
        const size_t amount = size < available ? size : available;
        const uintptr_t base = reinterpret_cast<uintptr_t>(vector.iov_base);
        if (cursor.offset > ~uintptr_t(0) - base) {
          SYSCALL_ERROR(BadAddress);
          return false;
        }
        void* destination = reinterpret_cast<void*>(base + cursor.offset);
        if (!PosixSubsystem::copyToUser(destination, bytes, amount)) {
          SYSCALL_ERROR(BadAddress);
          return false;
        }
        cursor.offset += amount;
        bytes += amount;
        size -= amount;
      }
      return true;
    };
    return timerFd    ? timerFd->readWithCopy(totalLength, canBlock, copy, &scatter)
           : signalFd ? signalFd->readWithCopy(totalLength, canBlock, copy, &scatter)
                      : fanotify->readWithCopy(totalLength, canBlock, copy, &scatter);
  }

  SharedPointer<EventFd> eventFd = descriptor->getEventFdImpl();
  if (eventFd) {
    if (totalLength < sizeof(uint64_t)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

    const bool canBlock = !(descriptor->getStatusFlags() & O_NONBLOCK);
    descriptor.reset();
    uint64_t value = 0;
    const int result = eventFd->readValue(value, canBlock);
    if (result < 0) {
      return result;
    }
    return scatterEventFdValue(vectors, iovcnt, value) ? result : -1;
  }

  if (descriptor->networkImpl) {
    struct msghdr message = {};
    message.msg_iov = vectors;
    message.msg_iovlen = static_cast<size_t>(iovcnt);
    return static_cast<int>(posix_recvmsg_descriptor(descriptor, &message));
  }
  if (!descriptor->getFile()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (descriptor->getFile()->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }

  auto readVector = [&](FileDescriptor::PositionGuard* position, int statusFlags) -> int {
    const bool pipeLike = descriptor->getFile()->isPipe() || descriptor->getFile()->isFifo();
    if (pipeLike && totalLength) {
      const size_t readCapacity = totalLength < PIPE_BUF_MAX ? totalLength : PIPE_BUF_MAX;
      UniqueArray<uint8_t> aggregate = UniqueArray<uint8_t>::allocate(readCapacity);
      if (statusFlags & O_NONBLOCK) {
        const ReadyMask ready =
            descriptor->acquireOpenFileDescription()->queryFileReady(true, false);
        if (!(ready & (ReadyRead | ReadyError | ReadyHangup))) {
          SYSCALL_ERROR(NoMoreProcesses);
          return -1;
        }
      }
      if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
        thread->clearInterruption();
        SYSCALL_ERROR(Interrupted);
        return -1;
      }
      bool signalInterrupted = false;
      const int amount =
          readFileVectorElement(thread, descriptor, position, statusFlags, aggregate.get(),
                                readCapacity, true, signalInterrupted);
      if (amount <= 0) {
        return amount;
      }

      size_t copied = 0;
      for (int i = 0; i < iovcnt && copied < static_cast<size_t>(amount); ++i) {
        size_t fragment = vectors[i].iov_len;
        if (fragment > static_cast<size_t>(amount) - copied) {
          fragment = static_cast<size_t>(amount) - copied;
        }
        if (fragment &&
            !PosixSubsystem::copyToUser(vectors[i].iov_base, aggregate.get() + copied, fragment)) {
          if (copied) {
            return static_cast<int>(copied);
          }
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        copied += fragment;
      }
      return static_cast<int>(copied);
    }

    size_t bounceCapacity = 0;
    UniqueArray<uint8_t> bounce =
        allocateReadBounce(descriptor->getFile(), totalLength, bounceCapacity);
    if (!bounce) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    int totalRead = 0;
    for (int i = 0; i < iovcnt; ++i) {
      F_NOTICE("readv: iov[" << i << "] is @ " << vectors[i].iov_base << ", " << vectors[i].iov_len
                             << " bytes.");

      if (!vectors[i].iov_len) {
        continue;
      }

      size_t vectorOffset = 0;
      while (vectorOffset < vectors[i].iov_len) {
        if (totalRead && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
          thread->clearInterruption();
          return totalRead;
        }

        const size_t remaining = vectors[i].iov_len - vectorOffset;
        size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
        void* userDestination = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(vectors[i].iov_base) + vectorOffset);
        if (!checkReadDestination(userDestination, requested)) {
          thread->clearInterruption();
          if (totalRead) {
            return totalRead;
          }
          SYSCALL_ERROR(BadAddress);
          return -1;
        }

        // Once a nonseekable source has produced data, do not turn another
        // bounce chunk into a second blocking operation.
        const bool operationCanBlock = !(statusFlags & O_NONBLOCK) && (position || !totalRead);
        if (!operationCanBlock) {
          const ReadyMask ready =
              descriptor->acquireOpenFileDescription()->queryFileReady(true, false);
          if (!(ready & (ReadyRead | ReadyError | ReadyHangup))) {
            if (totalRead) {
              return totalRead;
            }
            thread->clearInterruption();
            SYSCALL_ERROR(NoMoreProcesses);
            return -1;
          }
        }

        if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
          thread->clearInterruption();
          if (totalRead) {
            return totalRead;
          }
          SYSCALL_ERROR(Interrupted);
          return -1;
        }

        const int elementFlags = operationCanBlock ? statusFlags : statusFlags | O_NONBLOCK;
        bool signalInterrupted = false;
        const int r =
            readFileVectorElement(thread, descriptor, position, elementFlags, bounce.get(),
                                  requested, totalRead == 0, signalInterrupted);
        if (r < 0) {
          return totalRead ? totalRead : r;
        }
        if (!r) {
          return totalRead;
        }

        if (!PosixSubsystem::copyToUser(userDestination, bounce.get(), static_cast<size_t>(r))) {
          thread->clearInterruption();
          if (totalRead) {
            return totalRead;
          }
          SYSCALL_ERROR(BadAddress);
          return -1;
        }

        if (position) {
          position->advanceOffset(static_cast<size_t>(r));
        }
        totalRead += r;
        vectorOffset += static_cast<size_t>(r);
        if (static_cast<size_t>(r) < requested || signalInterrupted) {
          return totalRead;
        }
      }
    }

    return totalRead;
  };

  thread->clearInterruption();
  if (descriptor->getFile()->isSeekable()) {
    FileDescriptor::PositionGuard position = descriptor->lockPosition();
    return readVector(&position, position.statusFlags());
  }

  // See the matching writev path: a blocking nonseekable read must not
  // monopolize the OFD metadata mutex needed by its peer.
  return readVector(nullptr, descriptor->getStatusFlags());
}

namespace {
constexpr int LinuxRwfNoAppend = 0x20;

ssize_t positionalReadVector(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
  UniqueArray<struct iovec> vectorOwner;
  size_t totalLength = 0;
  if (!snapshotIoVectors(iov, iovcnt, false, vectorOwner, totalLength,
                         VectorPayloadValidation::AddressRange)) {
    return -1;
  }
  if (!positionalIoRangeIsValid(offset, totalLength)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (!subsystem) {
    return -1;
  }

  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (!descriptor->getFile() || !descriptor->getFile()->isSeekable()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }

  const int statusFlags = descriptor->getStatusFlags();
  if ((statusFlags & O_PATH) || (statusFlags & O_ACCMODE) == O_WRONLY) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (descriptor->getFile()->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }
  if (!iovcnt || !totalLength) {
    return 0;
  }

  struct iovec* vectors = vectorOwner.get();
  size_t bounceCapacity = 0;
  UniqueArray<uint8_t> bounce =
      allocateReadBounce(descriptor->getFile(), totalLength, bounceCapacity);
  if (!bounce) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const bool canBlock = !(statusFlags & O_NONBLOCK);
  const uint64_t startingOffset = static_cast<uint64_t>(offset);
  size_t totalRead = 0;

  thread->clearInterruption();
  for (int i = 0; i < iovcnt; ++i) {
    size_t vectorOffset = 0;
    while (vectorOffset < vectors[i].iov_len) {
      if (totalRead && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
        thread->clearInterruption();
        return static_cast<ssize_t>(totalRead);
      }

      const size_t remaining = vectors[i].iov_len - vectorOffset;
      size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
      void* userDestination =
          reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(vectors[i].iov_base) + vectorOffset);
      if (!checkReadDestination(userDestination, requested)) {
        thread->clearInterruption();
        if (totalRead) {
          return static_cast<ssize_t>(totalRead);
        }
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
        thread->clearInterruption();
        if (totalRead) {
          return static_cast<ssize_t>(totalRead);
        }
        SYSCALL_ERROR(Interrupted);
        return -1;
      }

      const uint64_t amount =
          descriptor->getFile()->read(startingOffset + totalRead, requested,
                                      reinterpret_cast<uintptr_t>(bounce.get()), canBlock);
      const bool signalInterrupted = thread->getInterruptionReason() == Thread::InterruptedBySignal;
      if (!amount) {
        thread->clearInterruption();
        if (!totalRead && signalInterrupted) {
          SYSCALL_ERROR(Interrupted);
          return -1;
        }
        return static_cast<ssize_t>(totalRead);
      }

      if (!PosixSubsystem::copyToUser(userDestination, bounce.get(), amount)) {
        thread->clearInterruption();
        if (totalRead) {
          return static_cast<ssize_t>(totalRead);
        }
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      totalRead += static_cast<size_t>(amount);
      vectorOffset += static_cast<size_t>(amount);
      if (amount < requested || signalInterrupted ||
          thread->getInterruptionReason() == Thread::InterruptedBySignal) {
        thread->clearInterruption();
        return static_cast<ssize_t>(totalRead);
      }
    }
  }

  thread->clearInterruption();
  return static_cast<ssize_t>(totalRead);
}

ssize_t positionalWriteVector(int fd, const struct iovec* iov, int iovcnt, off_t offset,
                              bool honorAppend) {
  UniqueArray<struct iovec> vectorOwner;
  size_t totalLength = 0;
  if (!snapshotIoVectors(iov, iovcnt, true, vectorOwner, totalLength,
                         VectorPayloadValidation::AddressRange)) {
    return -1;
  }
  if (!positionalIoRangeIsValid(offset, totalLength)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (!subsystem) {
    return -1;
  }

  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (!descriptor->getFile() || !descriptor->getFile()->isSeekable()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }

  const int statusFlags = descriptor->getStatusFlags();
  if ((statusFlags & O_PATH) || (statusFlags & O_ACCMODE) == O_RDONLY) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (descriptor->getFile()->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }
  if (!iovcnt || !totalLength) {
    return 0;
  }

  struct iovec* vectors = vectorOwner.get();
  const size_t bounceCapacity =
      totalLength < ScalarIoBounceCapacity ? totalLength : ScalarIoBounceCapacity;
  UniqueArray<uint8_t> bounce = UniqueArray<uint8_t>::allocate(bounceCapacity);
  const bool canBlock = !(statusFlags & O_NONBLOCK);
  const uint64_t startingOffset = static_cast<uint64_t>(offset);
  const bool append = honorAppend && (statusFlags & O_APPEND);
  File::WriteGuard writeGuard = descriptor->getFile()->lockWrites();
  size_t totalWritten = 0;
  int vectorIndex = 0;
  size_t vectorOffset = 0;

  thread->clearInterruption();
  while (totalWritten < totalLength) {
    if (totalWritten && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      thread->clearInterruption();
      return static_cast<ssize_t>(totalWritten);
    }

    const size_t remaining = totalLength - totalWritten;
    const size_t requested = remaining < bounceCapacity ? remaining : bounceCapacity;
    size_t gathered = 0;
    while (gathered < requested) {
      while (vectorIndex < iovcnt && vectorOffset == vectors[vectorIndex].iov_len) {
        ++vectorIndex;
        vectorOffset = 0;
      }
      assert(vectorIndex < iovcnt);

      const size_t vectorRemaining = vectors[vectorIndex].iov_len - vectorOffset;
      const size_t fragment =
          vectorRemaining < requested - gathered ? vectorRemaining : requested - gathered;
      const void* userSource = reinterpret_cast<const void*>(
          reinterpret_cast<uintptr_t>(vectors[vectorIndex].iov_base) + vectorOffset);
      if (!PosixSubsystem::copyFromUser(bounce.get() + gathered, userSource, fragment)) {
        thread->clearInterruption();
        if (totalWritten) {
          return static_cast<ssize_t>(totalWritten);
        }
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      gathered += fragment;
      vectorOffset += fragment;
    }

    if (thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      thread->clearInterruption();
      if (totalWritten) {
        return static_cast<ssize_t>(totalWritten);
      }
      SYSCALL_ERROR(Interrupted);
      return -1;
    }

    thread->setErrno(0);
    uint64_t location = startingOffset + totalWritten;
    const uint64_t amount =
        append ? writeGuard.append(requested, reinterpret_cast<uintptr_t>(bounce.get()), location,
                                   canBlock)
               : writeGuard.write(location, requested, reinterpret_cast<uintptr_t>(bounce.get()),
                                  canBlock);
    const bool signalInterrupted = thread->getInterruptionReason() == Thread::InterruptedBySignal;
    const size_t backendError = thread->getErrno();
    if (!amount) {
      thread->clearInterruption();
      if (!totalWritten && signalInterrupted) {
        SYSCALL_ERROR(Interrupted);
        return -1;
      }
      if (totalWritten) {
        thread->setErrno(0);
        return static_cast<ssize_t>(totalWritten);
      }
      if (backendError) {
        return -1;
      }
      if (!canBlock) {
        SYSCALL_ERROR(NoMoreProcesses);
        return -1;
      }
      return static_cast<ssize_t>(totalWritten);
    }

    thread->setErrno(0);
    totalWritten += static_cast<size_t>(amount);
    if (amount < requested || signalInterrupted ||
        thread->getInterruptionReason() == Thread::InterruptedBySignal) {
      thread->clearInterruption();
      return static_cast<ssize_t>(totalWritten);
    }
  }

  thread->clearInterruption();
  return static_cast<ssize_t>(totalWritten);
}
}  // namespace

ssize_t posix_preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
  F_NOTICE("preadv(" << fd << ", <iov>, " << iovcnt << ", " << offset << ")");
  return positionalReadVector(fd, iov, iovcnt, offset);
}

ssize_t posix_pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
  F_NOTICE("pwritev(" << fd << ", <iov>, " << iovcnt << ", " << offset << ")");
  return positionalWriteVector(fd, iov, iovcnt, offset, false);
}

ssize_t posix_preadv2(int fd, const struct iovec* iov, int iovcnt, off_t offset, int flags) {
  F_NOTICE("preadv2(" << fd << ", <iov>, " << iovcnt << ", " << offset << ", " << flags << ")");
  if (flags) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  if (offset == -1) {
    return posix_readv(fd, iov, iovcnt);
  }
  return positionalReadVector(fd, iov, iovcnt, offset);
}

ssize_t posix_pwritev2(int fd, const struct iovec* iov, int iovcnt, off_t offset, int flags) {
  F_NOTICE("pwritev2(" << fd << ", <iov>, " << iovcnt << ", " << offset << ", " << flags << ")");
  if (flags & ~LinuxRwfNoAppend) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  if (offset == -1) {
    return posixWritev(fd, iov, iovcnt, flags & LinuxRwfNoAppend);
  }
  return positionalWriteVector(fd, iov, iovcnt, offset, !(flags & LinuxRwfNoAppend));
}

off_t posix_lseek(int file, off_t ptr, int dir) {
  F_NOTICE("lseek(" << file << ", " << ptr << ", " << dir << ")");

  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(file, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (pFd->getStatusFlags() & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (pFd->getTimerFdImpl() || pFd->getSignalFdImpl() || pFd->getFanotifyImpl()) {
    if (dir < SEEK_SET || dir > 4) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    // Linux assigns these anonymous objects noop_llseek: offsets stay zero.
    return 0;
  }

  if (!pFd->getFile()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }

  if (!pFd->getFile()->isSeekable()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }

  FileDescriptor::PositionGuard position = pFd->lockPosition();
  size_t fileSize = pFd->getFile()->getSize();
  switch (dir) {
    case SEEK_SET:
      position.setOffset(ptr);
      break;
    case SEEK_CUR:
      position.advanceOffset(ptr);
      break;
    case SEEK_END:
      position.setOffset(fileSize + ptr);
      break;
  }

  return static_cast<off_t>(position.offset());
}

int posix_link(char* target, char* link) {
  return posix_linkat(AT_FDCWD, target, AT_FDCWD, link, AT_SYMLINK_FOLLOW);
}

int posix_readlink(const char* path, char* buf, unsigned int bufsize) {
  return posix_readlinkat(AT_FDCWD, path, buf, bufsize);
}

int posix_realpath(const char* path, char* buf, size_t bufsize) {
  F_NOTICE("realpath");

  String pathCopy;
  if (!copyUserString(path, pathCopy)) {
    F_NOTICE("realpath -> invalid address");
    return -1;
  }
  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), bufsize,
                                    PosixSubsystem::SafeWrite)) {
    F_NOTICE("realpath -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  String realPath;
  normalisePath(realPath, pathCopy.cstr());
  F_NOTICE("  -> traversing " << realPath);
  Process* process = Processor::information().getCurrentThread()->getParent();
  ResolvedPath fileLease;
  File* f = findFilePath(realPath, fileLease);
  if (!f) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  f = traverseSymlink(f, fileLease);
  if (!f) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  if (!f->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }

  String actualPath;
  auto context = process->acquireFilesystemContext();
  FilesystemContextSnapshot snapshot;
  auto* view = VFS::instance().mountView();
  if (!context || !context->snapshot(snapshot) || !view ||
      !view->formatPath(snapshot, fileLease.path(), actualPath))
    return -1;
  if (actualPath.length() >= bufsize) {
    SYSCALL_ERROR(NameTooLong);
    return -1;
  }

  // File is good, copy it now.
  F_NOTICE("  -> returning " << actualPath);
  if (!PosixSubsystem::copyToUser(buf, actualPath.cstr(), actualPath.length() + 1)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return 0;
}

int posix_unlink(char* name) {
  return posix_unlinkat(AT_FDCWD, name, 0);
}

int posix_symlink(char* target, char* link) {
  return posix_symlinkat(target, AT_FDCWD, link);
}

int posix_rename(const char* source, const char* dst) {
  return posix_renameat(AT_FDCWD, source, AT_FDCWD, dst);
}

int posix_getcwd(char* buf, size_t maxlen) {
  F_NOTICE("getcwd(" << maxlen << ")");

  TerminationDeferral lifetime;
  Process* process = Processor::information().getCurrentThread()->getParent();
  auto context = process->acquireFilesystemContext();
  FilesystemContextSnapshot snapshot;
  if (!context || !context->snapshot(snapshot) || !snapshot.cwd) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  File* cwd = snapshot.cwd->node();
  if (!cwd || !cwd->isDirectory() || Directory::fromFile(cwd)->isDetached()) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  String str;
  auto* view = VFS::instance().mountView();
  if (!view || !view->formatPath(snapshot, snapshot.cwd, str))
    return -1;

  size_t maxLength = str.length();
  if (maxLength >= maxlen) {
    // Too long.
    SYSCALL_ERROR(BadRange);
    return -1;
  }
  if (!PosixSubsystem::copyToUser(buf, str.cstr(), maxLength + 1)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE(" -> " << str);

  return maxLength + 1;  // include null terminator
}

int posix_stat(const char* name, struct stat* st) {
  F_NOTICE("stat(" << name << ") => fstatat");
  return posix_fstatat(AT_FDCWD, name, st, 0);
}

int posix_fstat(int fd, struct stat* st) {
  F_NOTICE("fstat(" << fd << ") => fstatat");
  return posix_fstatat(fd, 0, st, AT_EMPTY_PATH);
}

int posix_lstat(char* name, struct stat* st) {
  F_NOTICE("lstat(" << name << ") => fstatat");
  return posix_fstatat(AT_FDCWD, name, st, AT_SYMLINK_NOFOLLOW);
}

static int getdents_common(int fd,
                           size_t (*set_dent)(const Directory::DirectoryEntryView&, void*, size_t),
                           void* buffer, int count) {
  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd) || !pFd->getFile()) {
    // Error - no such file descriptor.
    F_NOTICE(" -> bad file");
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (pFd->getStatusFlags() & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (!pFd->getFile()->isDirectory()) {
    F_NOTICE(" -> not a directory");
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }

  if (!count) {
    F_NOTICE(" -> count is zero");
    return 0;
  }

  const size_t capacity = static_cast<size_t>(count) < 65536 ? count : 65536;
  UniqueArray<uint8_t> entries = UniqueArray<uint8_t>::allocate(capacity);
  if (!entries) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }

  // Navigate the directory tree.
  Directory* pDirectory = Directory::fromFile(pFd->getFile());
  struct Context {
    size_t (*setDent)(const Directory::DirectoryEntryView&, void*, size_t);
    void* buffer;
    size_t available;
    size_t written;
    bool rejected;
  } context = {set_dent, entries.get(), capacity, 0, false};

  auto emitter = [](void* opaque, const Directory::DirectoryEntryView& entry) -> bool {
    Context* context = reinterpret_cast<Context*>(opaque);
    F_NOTICE(" -> " << entry.name.toString());
    size_t reclen = context->setDent(entry, context->buffer, context->available - context->written);
    if (!reclen) {
      context->rejected = true;
      return false;
    }
    context->buffer = adjust_pointer(context->buffer, reclen);
    context->written += reclen;
    return true;
  };

  FileDescriptor::PositionGuard position = pFd->lockPosition();
  uint64_t cookie = position.offset();
  Directory::ReadStatus status = pDirectory->enumerate(cookie, emitter, &context);

  if (status == Directory::ReadStatus::IoError && !context.written) {
    SYSCALL_ERROR(IoError);
    return -1;
  }
  if (status == Directory::ReadStatus::Stopped && context.rejected && !context.written) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!PosixSubsystem::copyToUser(buffer, entries.get(), context.written)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  position.setOffset(cookie);

  F_NOTICE(" -> " << context.written);
  return context.written;
}

static char getdentsType(Directory::EntryType type) {
  switch (type) {
    case Directory::EntryType::Directory:
      return DT_DIR;
    case Directory::EntryType::Symlink:
      return DT_LNK;
    case Directory::EntryType::Fifo:
      return DT_FIFO;
    case Directory::EntryType::Socket:
      return DT_SOCK;
    case Directory::EntryType::CharacterDevice:
      return DT_CHR;
    case Directory::EntryType::BlockDevice:
      return DT_BLK;
    case Directory::EntryType::Regular:
      return DT_REG;
    case Directory::EntryType::Unknown:
    default:
      return DT_UNKNOWN;
  }
}

static size_t getdents_helper(const Directory::DirectoryEntryView& file, void* buffer,
                              size_t avail) {
  struct linux_dirent* entry = reinterpret_cast<struct linux_dirent*>(buffer);
  char* char_buffer = reinterpret_cast<char*>(buffer);

  size_t filenameLength = file.name.length();
  // dirent struct, filename, null terminator, and d_type
  size_t reclen = (offsetof(struct linux_dirent, d_name) + filenameLength + 2 + sizeof(long) - 1) &
                  ~(sizeof(long) - 1);
  // do we have room for this record?
  if (avail < reclen) {
    // need to call again with more space available
    return 0;
  }

  ByteSet(entry, 0, reclen);
  entry->d_reclen = reclen;
  entry->d_off = file.nextCookie;

  entry->d_ino = file.inode;
  if (!entry->d_ino) {
    entry->d_ino = ~0U;
  }

  MemoryCopy(entry->d_name, file.name.str(), filenameLength);
  entry->d_name[filenameLength] = 0;
  char_buffer[reclen - 2] = 0;
  char_buffer[reclen - 1] = getdentsType(file.type);

  return reclen;
}

static size_t getdents64_helper(const Directory::DirectoryEntryView& file, void* buffer,
                                size_t avail) {
  struct dirent* entry = reinterpret_cast<struct dirent*>(buffer);

  size_t filenameLength = file.name.length();
  size_t reclen = (offsetof(struct dirent, d_name) + filenameLength + 1 + sizeof(uint64_t) - 1) &
                  ~(sizeof(uint64_t) - 1);
  // do we have room for this record?
  if (avail < reclen) {
    // need to call again with more space available
    return 0;
  }

  ByteSet(entry, 0, reclen);
  entry->d_reclen = reclen;
  entry->d_off = file.nextCookie;

  entry->d_ino = file.inode;
  if (!entry->d_ino) {
    entry->d_ino = ~0U;
  }

  MemoryCopy(entry->d_name, file.name.str(), filenameLength);
  entry->d_name[filenameLength] = 0;
  entry->d_type = getdentsType(file.type);

  return reclen;
}

int posix_getdents(int fd, struct linux_dirent* ents, int count) {
  F_NOTICE("getdents(" << fd << ")");
  if (count < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ents), count,
                                    PosixSubsystem::SafeWrite)) {
    F_NOTICE("getdents -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return getdents_common(fd, getdents_helper, ents, count);
}

int posix_getdents64(int fd, struct dirent* ents, int count) {
  F_NOTICE("getdents64(" << fd << ")");
  if (count < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ents), count,
                                    PosixSubsystem::SafeWrite)) {
    F_NOTICE("getdents64 -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return getdents_common(fd, getdents64_helper, ents, count);
}

template <typename T>
static bool copyIoctlInput(const void* buffer, T& value) {
  if (!PosixSubsystem::copyFromUser(&value, buffer, sizeof(value))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  return true;
}

template <typename T>
static int copyIoctlResult(void* buffer, const T& value) {
  if (!PosixSubsystem::copyToUser(buffer, &value, sizeof(value))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_ioctl(int fd, size_t command, void* buf) {
  F_NOTICE("ioctl(" << Dec << fd << ", " << Hex << command << ", "
                    << reinterpret_cast<uintptr_t>(buf) << ")");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease f;
  if (!pSubsystem->acquireFileDescriptor(fd, f)) {
    // Error - no such FD.
    F_NOTICE("  -> ioctl for a file that doesn't exist");
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (f->getStatusFlags() & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (command == FIOCLEX || command == FIONCLEX) {
    f->fdflags = command == FIOCLEX ? f->fdflags | FD_CLOEXEC : f->fdflags & ~FD_CLOEXEC;
    return 0;
  }

  FileDescriptor::TerminalOperation terminalOperation;
  const bool terminalPolicyCommand =
      command == TIOCSCTTY || command == TIOCGPGRP || command == TIOCSPGRP;
  if (!terminalPolicyCommand && !f->acquireTerminalOperation(terminalOperation)) {
    SYSCALL_ERROR(IoError);
    return -1;
  }

  if (command == FIONBIO) {
    int enabled = 0;
    if (!copyIoctlInput(buf, enabled)) {
      return -1;
    }
    if (enabled) {
      f->addStatusFlag(O_NONBLOCK);
    } else {
      f->removeStatusFlag(O_NONBLOCK);
    }
    return 0;
  }
  auto fanotify = f->getFanotifyImpl();
  if (fanotify && command == FIONREAD)
    return copyIoctlResult(buf, fanotify->queuedMetadataBytes());
  if (f->getTimerFdImpl() || f->getSignalFdImpl() || fanotify) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }

  if (!f->getFile()) {
    F_NOTICE("  -> fd " << fd << " is not supposed to be ioctl'd");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (f->getFile()->supports(command)) {
    return f->getFile()->command(command, buf);
  }

  switch (command) {
    // KDGETLED
    case 0x4B31: {
      F_NOTICE(" -> KDGETLED, arg=" << buf);
      const char state = Machine::instance().getKeyboard()->getLedState();
      return copyIoctlResult(buf, state);
    }
      return 0;

    // KDSETLED
    case 0x4B32: {
      F_NOTICE(" -> KDSETLED, arg=" << buf);
      uintptr_t leds = reinterpret_cast<uintptr_t>(buf);
      Machine::instance().getKeyboard()->setLedState(leds);
    }
      return 0;

    case 0x4B33:  // KDGKBTYPE
    {
      F_NOTICE(" -> KDGKBTYPE");
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        // US 101
        return copyIoctlResult(buf, static_cast<unsigned char>(0x02));
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    // KDSETMODE
    case 0x4b3a:
      /// \todo what do we do when switching to graphics mode?
      F_NOTICE(" -> KDSETMODE (stubbed), arg=" << buf);
      if (buf == reinterpret_cast<void*>(1)) {
        g_pDevFs->getTerminalManager().setSystemMode(VirtualTerminalManager::Graphics);
      } else {
        g_pDevFs->getTerminalManager().setSystemMode(VirtualTerminalManager::Text);
      }
      return 0;

    // KDGETMODE
    case 0x4b3b: {
      F_NOTICE(" -> KDGETMODE");
      const int mode =
          g_pDevFs->getTerminalManager().getSystemMode() == VirtualTerminalManager::Graphics ? 1
                                                                                             : 0;
      return copyIoctlResult(buf, mode);
    }
      return 0;

    // KDGKBMODE
    case 0x4b44:
      F_NOTICE(" -> KDGKBMODE (stubbed), arg=" << buf);
      return 0;

    // KDSKBMODE
    case 0x4B45: {
      F_NOTICE(" -> KDSKBMODE, arg=" << buf);

      size_t consoleNumber = 0;
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        ConsoleFile* pConsole = static_cast<ConsoleFile*>(f->getFile());
        consoleNumber = pConsole->getPhysicalConsoleNumber();
        if (consoleNumber == ~0U) {
          ERROR("KDSKBMODE used on something that is not a VT");
          return -1;
        }
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }

      long mode = reinterpret_cast<long>(buf);
      if (mode == 0) {
        g_pDevFs->getTerminalManager().setInputMode(consoleNumber, TextIO::Raw);
      } else {
        g_pDevFs->getTerminalManager().setInputMode(consoleNumber, TextIO::Standard);
      }
    }
      return 0;

    // KDGKBENT
    case 0x4B46: {
      F_NOTICE(" -> KDGKBENT, arg=" << buf);
      POSIX_VERBOSE_LOG("io", " -> KDGKBENT, arg=" << buf);

      struct kbentry entryCopy = {};
      if (!copyIoctlInput(buf, entryCopy)) {
        return -1;
      }
      struct kbentry* kbent = &entryCopy;
      bool shift = kbent->kb_table & 0x1;
      bool altgr = kbent->kb_table & 0x2;
      bool ctrl = kbent->kb_table & 0x4;
      bool alt = kbent->kb_table & 0x8;

      // convert to HID so we can look in the keymap
      KeymapManager::EscapeState escape = KeymapManager::EscapeNone;
      uint8_t keyCode =
          KeymapManager::instance().convertPc102ScancodeToHidKeycode(kbent->kb_index, escape);
      KeymapManager::KeymapEntry* entry =
          KeymapManager::instance().getKeymapEntry(ctrl, shift, alt, altgr, 0, keyCode);
      if (entry) {
        F_NOTICE(" -> no keymap entry for table #" << Dec << kbent->kb_table << " index #" << Hex
                                                   << kbent->kb_index);
        kbent->kb_value = 0xF000 | static_cast<uint16_t>(entry->value & 0xFFFF);
      } else {
        kbent->kb_value = 0;
      }

      POSIX_VERBOSE_LOG("io", " -> val for table #" << Dec << kbent->kb_table << " #" << Hex
                                                    << kbent->kb_index << " is now "
                                                    << kbent->kb_value << "!");
      return copyIoctlResult(buf, entryCopy);
    }
      return 0;

    // KDSKBENT
    case 0x4B47:
      F_NOTICE(" -> KDSKBENT (stubbed), arg=" << buf);
      POSIX_VERBOSE_LOG("io", " -> KDSKBENT (stubbed), arg=" << buf);
      return -1;

    // KDKBDREP
    case 0x4B52:
      F_NOTICE(" -> KDKBDREP (stubbed), arg=" << buf);
      return 0;

    case TCGETS: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        return console_tcgetattr(f, reinterpret_cast<struct termios*>(buf));
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TCSETS: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        return console_tcsetattr(f, TCSANOW, reinterpret_cast<struct termios*>(buf));
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TCSETSW: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        return console_tcsetattr(f, TCSADRAIN, reinterpret_cast<struct termios*>(buf));
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TCSETSF: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        return console_tcsetattr(f, TCSAFLUSH, reinterpret_cast<struct termios*>(buf));
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TIOCGPGRP: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        pid_t pgrp = TerminalControl::foreground(*static_cast<ConsoleFile*>(f->getFile()),
                                                 f->terminalEpoch());
        return pgrp < 0 ? -1 : copyIoctlResult(buf, pgrp);
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TIOCSPGRP: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        pid_t pgrp = 0;
        return copyIoctlInput(buf, pgrp)
                   ? TerminalControl::setForeground(*static_cast<ConsoleFile*>(f->getFile()), pgrp,
                                                    f->terminalEpoch())
                   : -1;
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TCFLSH: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        return console_flush(f->getFile(), 0);
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TIOCGWINSZ: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        F_NOTICE(" -> TIOCGWINSZ");
        struct winsize value = {};
        const int result = console_getwinsize(f->getFile(), &value);
        return result < 0 ? result : copyIoctlResult(buf, value);
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TIOCSWINSZ: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        struct winsize value = {};
        if (!copyIoctlInput(buf, value)) {
          return -1;
        }
        const struct winsize* ws = &value;
        F_NOTICE(" -> TIOCSWINSZ " << Dec << ws->ws_col << "x" << ws->ws_row << Hex);
        return console_setwinsize(f->getFile(), ws);
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TIOCSCTTY: {
      if (ConsoleManager::instance().isConsole(f->getFile())) {
        F_NOTICE(" -> TIOCSCTTY");
        return TerminalControl::attach(*static_cast<ConsoleFile*>(f->getFile()),
                                       reinterpret_cast<uintptr_t>(buf) == 1, false,
                                       f->terminalEpoch());
      } else {
        SYSCALL_ERROR(NotAConsole);
        return -1;
      }
    }

    case TIOCGPTN: {
      F_NOTICE(" -> TIOCGPTN");
      unsigned int result = console_getptn(fd);
      if (result < ~0U) {
        F_NOTICE(" -> ok, returning " << result);
        if (!PosixSubsystem::copyToUser(buf, &result, sizeof(result))) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        return 0;
      } else {
        // console_getptn will set the syscall error
        F_NOTICE(" -> failed!");
        return -1;
      }
    }

    case TIOCSPTLCK: {
      if (ConsoleManager::instance().isConsole(f->getFile()) &&
          ConsoleManager::instance().isMasterConsole(f->getFile())) {
        int locked = 0;
        if (!copyIoctlInput(buf, locked)) {
          return -1;
        }
        if (locked != 0 && locked != 1) {
          SYSCALL_ERROR(InvalidArgument);
          return -1;
        }
        F_NOTICE(" -> TIOCSPTLCK " << locked);
        return ConsoleManager::instance().setPtyLock(f->getFile(), locked != 0) ? 0 : -1;
      }
      SYSCALL_ERROR(NotAConsole);
      return -1;
    }

    // VT_OPENQRY
    case 0x5600: {
      F_NOTICE(" -> VT_OPENQRY (stubbed)");

      size_t newTty = g_pDevFs->getTerminalManager().openInactive();
      const int result = newTty != ~0U ? static_cast<int>(newTty + 1) : -1;
      return copyIoctlResult(buf, result);
    }

    // VT_GETMODE
    case 0x5601: {
      F_NOTICE(" -> VT_GETMODE (stubbed)");

      /// \todo this should actually use the tty number of the file
      /// descriptor
      size_t currentTty = g_pDevFs->getTerminalManager().getCurrentTerminalNumber();

      return copyIoctlResult(buf, g_pDevFs->getTerminalManager().getTerminalMode(currentTty));
    }
      return 0;

    // VT_SETMODE
    case 0x5602: {
      F_NOTICE(" -> VT_SETMODE (stubbed)");

      struct vt_mode mode = {};
      if (!copyIoctlInput(buf, mode)) {
        return -1;
      }

      /// \todo this should actually use the tty number of the file
      /// descriptor
      size_t currentTty = g_pDevFs->getTerminalManager().getCurrentTerminalNumber();
      g_pDevFs->getTerminalManager().setTerminalMode(currentTty, mode);
    }
      return 0;

    // VT_GETSTATE
    case 0x5603: {
      F_NOTICE(" -> VT_GETSTATE (stubbed)");

      return copyIoctlResult(buf, g_pDevFs->getTerminalManager().getState());
    }
      return 0;

    // VT_RELDISP
    case 0x5605: {
      F_NOTICE(" -> VT_RELDISP (stubbed)");

      NOTICE("VT_RELDISP");
      uintptr_t ibuf = reinterpret_cast<uintptr_t>(buf);
      if (ibuf == 0) {
        NOTICE(" -> switch disallowed");
        g_pDevFs->getTerminalManager().reportPermission(VirtualTerminalManager::Disallowed);
      } else if (ibuf == 1) {
        NOTICE(" -> switch allowed");
        g_pDevFs->getTerminalManager().reportPermission(VirtualTerminalManager::Allowed);
      } else {
        NOTICE(" -> switch acknowledged");
      }
    }
      return 0;

    // VT_ACTIVATE
    case 0x5606: {
      uintptr_t ttyNum = reinterpret_cast<uintptr_t>(buf);
      F_NOTICE(" -> VT_ACTIVATE -> " << ttyNum);
      g_pDevFs->getTerminalManager().activate(ttyNum - 1);
    }
      return 0;

    // VT_WAITACTIVE
    case 0x5607:
      // no-op on Pedigree so far
      return 0;
  }

  F_NOTICE("  -> invalid combination of fd " << fd << " and ioctl " << Hex << command);
  SYSCALL_ERROR(InvalidArgument);
  return -1;
}

int posix_chdir(const char* path) {
  F_NOTICE("chdir");

  String pathCopy;
  if (!copyUserString(path, pathCopy)) {
    F_NOTICE("chdir -> invalid address");
    return -1;
  }

  F_NOTICE("chdir(" << pathCopy << ")");

  String realPath;
  normalisePath(realPath, pathCopy.cstr());

  ResolvedPath dirLease;
  File* dir = findFilePath(realPath, dirLease);
  if (!dir) {
    F_NOTICE("Path lookup failed.");
    if (!Processor::information().getCurrentThread()->getErrno())
      SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  return doChdir(dir, dirLease) ? 0 : -1;
}

int posix_dup(int fd) {
  F_NOTICE("dup(" << fd << ")");

  // grab the file descriptor pointer for the passed descriptor
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease f;
  if (!pSubsystem->acquireFileDescriptor(fd, f)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  size_t newFd = pSubsystem->getFd();

  // Copy the descriptor
  FileDescriptor* f2 = new FileDescriptor(*f);
  if ((f->networkImpl && !f2->networkPublished()) ||
      (f->getEventFdImpl() && !f2->eventFdPublished()) ||
      (f->getTimerFdImpl() && !f2->timerFdPublished()) ||
      (f->getSignalFdImpl() && !f2->signalFdPublished())) {
    delete f2;
    pSubsystem->freeFd(newFd);
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  // According to the spec, CLOEXEC is cleared on DUP.
  f2->fdflags &= ~FD_CLOEXEC;
  f2->fd = newFd;
  pSubsystem->addFileDescriptor(newFd, f2);

  return static_cast<int>(newFd);
}

int posix_dup2(int fd1, int fd2) {
  F_NOTICE("dup2(" << fd1 << ", " << fd2 << ")");

  constexpr int MaximumFileDescriptors = 16384;
  if (fd2 < 0 || fd2 >= MaximumFileDescriptors) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;  // EBADF
  }

  // grab the file descriptor pointer for the passed descriptor
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease f;
  if (!pSubsystem->acquireFileDescriptor(fd1, f)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  // POSIX still requires the source to be valid when both numbers match.
  if (fd1 == fd2)
    return fd2;

  // Copy the descriptor.
  //
  // This will also increase the refcount *before* we close the original, else
  // we might accidentally trigger an EOF condition on a pipe! (if the write
  // refcount drops to zero)...
  FileDescriptor* f2 = new FileDescriptor(*f);
  if ((f->networkImpl && !f2->networkPublished()) ||
      (f->getEventFdImpl() && !f2->eventFdPublished()) ||
      (f->getTimerFdImpl() && !f2->timerFdPublished()) ||
      (f->getSignalFdImpl() && !f2->signalFdPublished())) {
    delete f2;
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  // According to the spec, CLOEXEC is cleared on DUP.
  f2->fdflags &= ~FD_CLOEXEC;
  f2->fd = fd2;
  pSubsystem->addFileDescriptor(fd2, f2);

  return fd2;
}

int posix_dup3(int oldfd, int newfd, int flags) {
  F_NOTICE("dup3(" << oldfd << ", " << newfd << ", " << flags << ")");

  if (flags & ~O_CLOEXEC || oldfd == newfd) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  constexpr int MaximumFileDescriptors = 16384;
  if (newfd < 0 || newfd >= MaximumFileDescriptors) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  Process* process = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  const PosixSubsystem::DescriptorDuplicationResult result = subsystem->duplicateFileDescriptor(
      static_cast<size_t>(oldfd), static_cast<size_t>(newfd), flags & O_CLOEXEC);
  if (result == PosixSubsystem::DescriptorDuplicationResult::TargetBusy) {
    SYSCALL_ERROR(DeviceBusy);
    return -1;
  }
  if (result != PosixSubsystem::DescriptorDuplicationResult::Success) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  return newfd;
}

int posix_mkdir(const char* name, int mode) {
  return posix_mkdirat(AT_FDCWD, name, mode);
}

int posix_rmdir(const char* path) {
  return posix_unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

int posix_isatty(int fd) {
  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    ERROR("isatty: no such file descriptor (" << Dec << fd << Hex << ")");
    return 0;
  }

  int result =
      ConsoleManager::instance().isConsole(pFd->getFile()) && !pFd->terminalHungUp() ? 1 : 0;
  NOTICE("isatty(" << fd << ") -> " << result);
  return result;
}

int posix_fcntl(int fd, int cmd, void* arg) {
  /// \todo Same as ioctl, figure out how best to sanitise input addresses
  F_NOTICE("fcntl(" << fd << ", " << cmd << ", " << arg << ")");

  // grab the file descriptor pointer for the passed descriptor
  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease f;
  if (!pSubsystem->acquireFileDescriptor(fd, f)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if ((f->getStatusFlags() & O_PATH) && cmd != F_GETFL && cmd != F_GETFD && cmd != F_SETFD &&
      cmd != F_DUPFD && cmd != F_DUPFD_CLOEXEC) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  switch (cmd) {
#ifdef F_DUPFD_CLOEXEC
    case F_DUPFD_CLOEXEC:
#endif
    case F_DUPFD: {
      constexpr intptr_t MaximumFileDescriptors = 16384;
      const intptr_t minimum = reinterpret_cast<intptr_t>(arg);
      if (minimum < 0 || minimum >= MaximumFileDescriptors) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }

      const size_t fd2 = pSubsystem->getFd(static_cast<size_t>(minimum));
      FileDescriptor* f2 = new FileDescriptor(*f);
      if ((f->networkImpl && !f2->networkPublished()) ||
          (f->getEventFdImpl() && !f2->eventFdPublished()) ||
          (f->getTimerFdImpl() && !f2->timerFdPublished()) ||
          (f->getSignalFdImpl() && !f2->signalFdPublished())) {
        delete f2;
        pSubsystem->freeFd(fd2);
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
      }
#ifdef F_DUPFD_CLOEXEC
      if (cmd == F_DUPFD_CLOEXEC) {
        f2->fdflags |= FD_CLOEXEC;
      } else
#endif
      {
        f2->fdflags &= ~FD_CLOEXEC;
      }
      f2->fd = fd2;
      pSubsystem->addFileDescriptor(fd2, f2);

      return static_cast<int>(fd2);
    }

    case F_GETFD:
      F_NOTICE("  -> get fd flags");
      return f->fdflags;
    case F_SETFD:
      F_NOTICE("  -> set fd flags: " << arg);
      f->fdflags = reinterpret_cast<size_t>(arg);
      return 0;
    case F_GETFL:
      F_NOTICE("  -> get flags " << f->getStatusFlags());
      return f->getStatusFlags();
    case F_SETFL:
      F_NOTICE("  -> set flags " << arg);
      f->setStatusFlags(reinterpret_cast<size_t>(arg));
      F_NOTICE("  -> new flags " << f->getStatusFlags());
      return 0;
    case F_GET_SEALS:
    case F_ADD_SEALS:
      return posix_memfd_fcntl(f, cmd, reinterpret_cast<uintptr_t>(arg));
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_GETLK:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
      return posix_advisory_fcntl(*pSubsystem, fd, f, cmd, arg);
    case F_GETOWN:
      F_NOTICE("  -> F_GETOWN (stubbed)");
      return 0;
    case F_SETOWN:
      /// \todo implement signal management
      F_NOTICE("  -> F_SETOWN");

      if (!f->ioevent) {
        if (f->getFile()) {
          NOTICE("Adding ioevent to thread");
          f->ioevent = new IoEvent(pSubsystem, f->getFile());
          f->getFile()->monitor(pThread, f->ioevent);
        } else {
          /// \todo errno
          ERROR("F_SETOWN on something that can't raise events [fd="
                << fd << " file=" << f->getFile() << " impl=" << f->networkImpl.get() << "!");
          return -1;
        }
      }
      return 0;

    default:
      WARNING("fcntl: unknown control " << cmd << " on fd " << fd);
  }

  SYSCALL_ERROR(Unimplemented);
  return -1;
}

void* posix_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
  TerminationDeferral lifetime;
  F_NOTICE("mmap");
  F_NOTICE("  -> addr=" << addr << ", len=" << len << ", prot=" << prot << ", flags=" << flags
                        << ", fildes=" << fd << ", off=" << off << ".");

  // Get the File object to map
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return MAP_FAILED;
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  const size_t pageSz = PhysicalMemoryManager::getPageSize();
  const uintptr_t pageMask = pageSz - 1;
  const bool fixedNoReplace = flags & MAP_FIXED_NOREPLACE;
  const bool fixed = fixedNoReplace || (flags & MAP_FIXED);
  const MemoryMapManager::Placement placement =
      fixedNoReplace
          ? MemoryMapManager::Placement::FixedNoReplace
          : (fixed ? MemoryMapManager::Placement::FixedReplace : MemoryMapManager::Placement::Hint);

  // Verify the passed length and file offset before rounding either input.
  const int mappingType = flags & (MAP_PRIVATE | MAP_SHARED);
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) ||
      (mappingType != MAP_PRIVATE && mappingType != MAP_SHARED) || !len ||
      len > ~static_cast<size_t>(0) - pageMask ||
      (!(flags & MAP_ANON) && (off < 0 || (static_cast<uint64_t>(off) & pageMask)))) {
    SYSCALL_ERROR(InvalidArgument);
    return MAP_FAILED;
  }
  const size_t roundedLength = (len + pageMask) & ~pageMask;
  if (!(flags & MAP_ANON) && static_cast<uint64_t>(off) > ~static_cast<size_t>(0) - roundedLength) {
    SYSCALL_ERROR(InvalidArgument);
    return MAP_FAILED;
  }
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  pProcess->recordBenchmarkVmCounter(Process::VmMmapCalls);
  pProcess->recordBenchmarkVmCounter(flags & MAP_ANON ? Process::VmMmapAnonymousCalls
                                                      : Process::VmMmapFileCalls);
  const size_t mappingPages = roundedLength / pageSz;
  pProcess->recordBenchmarkVmCounter(Process::VmMmapPages, mappingPages);
  pProcess->recordBenchmarkVmCounter(vmLengthCounter(false, mappingPages));
#endif

  // Sanitise input.
  uintptr_t sanityAddress = reinterpret_cast<uintptr_t>(addr);
  if (fixed) {
    if ((sanityAddress & pageMask) || sanityAddress < va.getUserStart() ||
        sanityAddress >= va.getKernelStart() ||
        roundedLength > va.getKernelStart() - sanityAddress) {
      SYSCALL_ERROR(InvalidArgument);
      F_NOTICE("  -> mmap given invalid fixed address");
      return MAP_FAILED;
    }
  } else if (sanityAddress) {
    sanityAddress &= ~pageMask;
    if (sanityAddress < va.getUserStart() || sanityAddress >= va.getKernelStart() ||
        roundedLength > va.getKernelStart() - sanityAddress) {
      // Invalid non-fixed hints do not make the mapping fail.
      sanityAddress = 0;
    }
  }

  MemoryMapManager::MapStatus mapStatus = MemoryMapManager::MapStatus::NoMemory;
  const MemoryLockMode requestedLock =
      flags & MAP_LOCKED ? MemoryLockMode::Eager : MemoryLockMode::None;
  if (requestedLock != MemoryLockMode::None) {
    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    if (pProcess->getEffectiveUserId() != 0 && !pSubsystem->memoryLockAccount().limit().current) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return MAP_FAILED;
    }
  }

  // Create permission set.
  MemoryMappedObject::Permissions perms;
  if (prot == PROT_NONE) {
    perms = MemoryMappedObject::None;
  } else {
    // Everything implies a readable memory region.
    perms = MemoryMappedObject::Read;
    if (prot & PROT_WRITE)
      perms |= MemoryMappedObject::Write;
    if (prot & PROT_EXEC)
      perms |= MemoryMappedObject::Exec;
  }

  if (flags & MAP_ANON) {
    if (flags & MAP_SHARED) {
      F_NOTICE("  -> failed (MAP_SHARED cannot be used with MAP_ANONYMOUS)");
      SYSCALL_ERROR(InvalidArgument);
      return MAP_FAILED;
    }

    MemoryMappedObject* pObject = MemoryMapManager::instance().mapAnon(
        sanityAddress, len, perms, placement, &mapStatus, requestedLock);
    if (!pObject) {
      if (mapStatus == MemoryMapManager::MapStatus::AddressInUse) {
        SYSCALL_ERROR(FileExists);
      } else if (mapStatus == MemoryMapManager::MapStatus::LockLimit) {
        SYSCALL_ERROR(NoMoreProcesses);
      } else {
        SYSCALL_ERROR(OutOfMemory);
      }
      F_NOTICE("  -> failed (mapAnon)!");
      return MAP_FAILED;
    }

    F_NOTICE("  -> " << sanityAddress);

    if ((flags & MAP_POPULATE) && !(flags & MAP_NONBLOCK)) {
      MemoryMapManager::instance().populateMemory(va, sanityAddress, roundedLength);
    }
    Processor::information().getCurrentThread()->setErrno(0);
    return reinterpret_cast<void*>(sanityAddress);
  } else {
    // Valid file passed?
    DescriptorLease f;
    if (!pSubsystem->acquireFileDescriptor(fd, f)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return MAP_FAILED;
    }

    // Grab the file to map in
    File* fileToMap = f->getFile();
    UtsRef namespaceBacking;
    if (!fileToMap || fileToMap->isDirectory() ||
        posix_uts_file_namespace(fileToMap, namespaceBacking)) {
      SYSCALL_ERROR(NoSuchDevice);
      return MAP_FAILED;
    }

    const int descriptorFlags = f->getStatusFlags();
    const int accessMode = descriptorFlags & O_ACCMODE;
    if (descriptorFlags & O_PATH) {
      SYSCALL_ERROR(BadFileDescriptor);
      return MAP_FAILED;
    }
    if (accessMode == O_WRONLY) {
      SYSCALL_ERROR(PermissionDenied);
      return MAP_FAILED;
    }
    MemoryMappedObject::Permissions maximumPerms =
        MemoryMappedObject::Read | MemoryMappedObject::Write | MemoryMappedObject::Exec;
    if ((flags & MAP_SHARED) && accessMode != O_RDWR) {
      maximumPerms &= ~MemoryMappedObject::Write;
      if (prot & PROT_WRITE) {
        SYSCALL_ERROR(PermissionDenied);
        return MAP_FAILED;
      }
    }

    F_NOTICE("mmap: file name is " << fileToMap->getFullPath());

    bool bCopyOnWrite = (flags & MAP_SHARED) == 0;
    const FileMappingOrigin origin{f->acquireOpenFileDescription()->identity(),
                                   accessMode == O_RDWR, f->openingPath()};
    MemoryMappedObject* pFile = MemoryMapManager::instance().mapFile(
        fileToMap, sanityAddress, roundedLength, perms, off, bCopyOnWrite, placement, &mapStatus,
        maximumPerms, SharedPointer<MappingAttachment>(), requestedLock, origin);
    if (!pFile) {
      if (mapStatus == MemoryMapManager::MapStatus::AddressInUse) {
        SYSCALL_ERROR(FileExists);
      } else if (mapStatus == MemoryMapManager::MapStatus::PolicyDenied) {
        SYSCALL_ERROR(NotEnoughPermissions);
      } else if (mapStatus == MemoryMapManager::MapStatus::TextBusy) {
        SYSCALL_ERROR(TextFileBusy);
      } else if (mapStatus == MemoryMapManager::MapStatus::LockLimit) {
        SYSCALL_ERROR(NoMoreProcesses);
      } else {
        SYSCALL_ERROR(OutOfMemory);
      }
      F_NOTICE("  -> failed (mapFile)!");
      return MAP_FAILED;
    }

    F_NOTICE("  -> " << sanityAddress);

    if ((flags & MAP_POPULATE) && !(flags & MAP_NONBLOCK)) {
      MemoryMapManager::instance().populateMemory(va, sanityAddress, roundedLength);
    }
    Processor::information().getCurrentThread()->setErrno(0);
    return reinterpret_cast<void*>(sanityAddress);
  }
}

int posix_msync(void* p, size_t len, int flags) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(p);
  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  if ((address & pageMask) || (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) ||
      ((flags & MS_ASYNC) && (flags & MS_SYNC))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!len) {
    return 0;
  }
  if (len > ~static_cast<size_t>(0) - pageMask ||
      ((len + pageMask) & ~pageMask) > ~static_cast<uintptr_t>(0) - address) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  int error = 0;
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if ((flags & MS_INVALIDATE) && MemoryMapManager::instance().hasLockedMemory(
                                     Processor::information().getVirtualAddressSpace(), address,
                                     (len + pageMask) & ~pageMask)) {
    SYSCALL_ERROR(DeviceBusy);
    return -1;
  }
  if (!MemoryMapManager::instance().sync(address, len, flags & MS_ASYNC, &error)) {
    syscallError(error);
    return -1;
  }
  return 0;
}

int posix_mprotect(void* p, size_t len, int prot) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(p);
  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  if ((address & pageMask) || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!len) {
    return 0;
  }
  MemoryMappedObject::Permissions permissions = MemoryMappedObject::None;
  if (prot != PROT_NONE) {
    permissions = MemoryMappedObject::Read;
    if (prot & PROT_WRITE) {
      permissions |= MemoryMappedObject::Write;
    }
    if (prot & PROT_EXEC) {
      permissions |= MemoryMappedObject::Exec;
    }
  }
  MemoryMapManager::ProtectStatus status;
  if (!MemoryMapManager::instance().setPermissions(address, len, permissions, &status)) {
    if (status == MemoryMapManager::ProtectStatus::AccessDenied) {
      SYSCALL_ERROR(PermissionDenied);
    } else if (status == MemoryMapManager::ProtectStatus::Unsupported) {
      SYSCALL_ERROR(OperationNotSupported);
    } else if (status == MemoryMapManager::ProtectStatus::TextBusy) {
      SYSCALL_ERROR(TextFileBusy);
    } else {
      SYSCALL_ERROR(OutOfMemory);
    }
    return -1;
  }
  return 0;
}

int posix_munmap(void* addr, size_t len) {
  F_NOTICE("munmap(" << reinterpret_cast<uintptr_t>(addr) << ", " << len << ")");

  const uintptr_t address = reinterpret_cast<uintptr_t>(addr);
  const size_t pageSz = PhysicalMemoryManager::getPageSize();
  const size_t pageMask = pageSz - 1;
  if (!len || (address & pageMask) || len > ~static_cast<size_t>(0) - pageMask) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const size_t roundedLength = (len + pageMask) & ~pageMask;
  if (address > ~static_cast<uintptr_t>(0) - roundedLength) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  Process* process = Processor::information().getCurrentThread()->getParent();
  const size_t mappingPages = roundedLength / pageSz;
  process->recordBenchmarkVmCounter(Process::VmMunmapCalls);
  process->recordBenchmarkVmCounter(Process::VmMunmapPages, mappingPages);
  process->recordBenchmarkVmCounter(vmLengthCounter(true, mappingPages));
#endif

  MemoryMapManager::VmStatus status;
#if PEDIGREE_BENCHMARK_VM_ABLATIONS
  Thread* benchmarkThread = Processor::information().getCurrentThread();
  Process* benchmarkProcess = benchmarkThread->getParent();
  const bool deferTableRetirement =
      benchmarkProcess->benchmarkVmAblationEnabled(Process::AblateTableRetirement);
  if (deferTableRetirement) {
    benchmarkThread->enterBenchmarkVmMunmap();
  }
#endif
  MemoryMapManager::instance().removeAndRelease(address, roundedLength, &status);
#if PEDIGREE_BENCHMARK_VM_ABLATIONS
  if (deferTableRetirement) {
    benchmarkThread->leaveBenchmarkVmMunmap();
  }
#endif
  if (status != MemoryMapManager::VmStatus::Success) {
    if (status == MemoryMapManager::VmStatus::Unsupported)
      SYSCALL_ERROR(OperationNotSupported);
    else
      SYSCALL_ERROR(OutOfMemory);
    return -1;
  }

  return 0;
}

int posix_access(const char* name, int amode) {
  return posix_faccessat(AT_FDCWD, name, amode, 0);
}

int posix_ftruncate(int a, off_t b) {
  F_NOTICE("ftruncate(" << a << ", " << b << ")");

  if (b < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // Grab the File pointer for this file
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(a, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (pFd->getStatusFlags() & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  File* pFile = pFd->getFile();
  if (!pFile) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if ((pFd->getStatusFlags() & O_ACCMODE) == O_RDONLY) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return pFile->resize(static_cast<size_t>(b)) ? 0 : -1;
}

int posix_fsync(int fd) {
  F_NOTICE("fsync(" << fd << ")");

  // Grab the File pointer for this file
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (pFd->getStatusFlags() & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  File* pFile = pFd->getFile();
  if (!pFile) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!pFile->sync()) {
    SYSCALL_ERROR(IoError);
    return -1;
  }

  return 0;
}

EXPORTED_PUBLIC int pedigree_get_mount(char* mount_buf, char* info_buf, size_t n) {
  if (!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(mount_buf), PATH_MAX,
                                     PosixSubsystem::SafeWrite) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(info_buf), PATH_MAX,
                                     PosixSubsystem::SafeWrite))) {
    F_NOTICE("pedigree_get_mount -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  NOTICE("pedigree_get_mount(" << Dec << n << Hex << ")");

  Vector<VFS::MountSnapshot> mounts;
  VFS::instance().getMounts(mounts);

  size_t i = 0;
  for (const auto& mount : mounts) {
    if (i == n) {
      String info;
      if (mount.hasDisk) {
        info = mount.diskParentName;
        if (info.length() && mount.diskName.length()) {
          info += " // ";
        }
        info += mount.diskName;
      } else {
        info.assign("no disk", 8);
      }

      if (!PosixSubsystem::copyToUser(mount_buf, mount.path.cstr(), mount.path.length() + 1) ||
          !PosixSubsystem::copyToUser(info_buf, info.cstr(), info.length() + 1)) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      return 0;
    }
    ++i;
  }

  return -1;
}

int posix_chmod(const char* path, mode_t mode) {
  return posix_fchmodat(AT_FDCWD, path, mode, 0);
}

int posix_chown(const char* path, uid_t owner, gid_t group) {
  return posix_fchownat(AT_FDCWD, path, owner, group, 0);
}

int posix_fchmod(int fd, mode_t mode) {
  DescriptorLease descriptor;
  if (!acquireDescriptor(fd, descriptor) || !descriptor->getFile() ||
      (descriptor->getStatusFlags() & O_PATH)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (mode == static_cast<mode_t>(-1)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  File* file = descriptor->getFile();
  if (file->getFilesystem() && file->getFilesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return -1;
  }
  return posix_chmod_file(file, mode) ? 0 : -1;
}

int posix_fchown(int fd, uid_t owner, gid_t group) {
  DescriptorLease descriptor;
  if (!acquireDescriptor(fd, descriptor) || !descriptor->getFile() ||
      (descriptor->getStatusFlags() & O_PATH)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  File* file = descriptor->getFile();
  if (file->getFilesystem() && file->getFilesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return -1;
  }
  return posix_chown_file(file, owner, group) ? 0 : -1;
}

int posix_fchdir(int fd) {
  F_NOTICE("fchdir(" << fd << ")");

  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  ResolvedPath targetLease;
  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  File* file = pFd->getFile();
  targetLease.retain(pFd->openingPath());
  return doChdir(file, targetLease) ? 0 : -1;
}

static int statvfs_doer(Filesystem* pFs, struct statvfs* userBuffer) {
  if (!pFs) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  struct statvfs value = {};
  struct statvfs* buf = &value;
  /// \todo Get all this data from the Filesystem object
  buf->f_bsize = 4096;
  buf->f_frsize = 512;
  buf->f_blocks = static_cast<fsblkcnt_t>(-1);
  buf->f_bfree = static_cast<fsblkcnt_t>(-1);
  buf->f_bavail = static_cast<fsblkcnt_t>(-1);
  buf->f_files = 0;
  buf->f_ffree = static_cast<fsfilcnt_t>(-1);
  buf->f_favail = static_cast<fsfilcnt_t>(-1);
  buf->f_fsid = 0;
  buf->f_flag = (pFs->isReadOnly() ? ST_RDONLY : 0) | ST_NOSUID;  // No suid in pedigree yet.
  buf->f_namemax = 0;

  if (!PosixSubsystem::copyToUser(userBuffer, &value, sizeof(value))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_fstatvfs(int fd, struct statvfs* buf) {
  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), sizeof(struct statvfs),
                                    PosixSubsystem::SafeWrite)) {
    F_NOTICE("fstatvfs -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("fstatvfs(" << fd << ")");

  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  File* file = pFd->getFile();

  if (!file) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  return statvfs_doer(file->getFilesystem(), buf);
}

int posix_statvfs(const char* path, struct statvfs* buf) {
  String pathCopy;
  if (!copyUserString(path, pathCopy)) {
    F_NOTICE("statvfs -> invalid address");
    return -1;
  }
  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), sizeof(struct statvfs),
                                    PosixSubsystem::SafeWrite)) {
    F_NOTICE("statvfs -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("statvfs(" << pathCopy << ")");

  String realPath;
  normalisePath(realPath, pathCopy.cstr());

  ResolvedPath fileLease;
  File* file = findFilePath(realPath, fileLease);
  if (!file) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  // Symlink traversal
  file = traverseSymlink(file, fileLease);
  if (!file)
    return -1;

  return statvfs_doer(file->getFilesystem(), buf);
}

int posix_utime(const char* path, const struct utimbuf* times) {
  String pathCopy;
  if (!copyUserString(path, pathCopy)) {
    F_NOTICE("utimes -> invalid address");
    return -1;
  }
  struct utimbuf snapshot = {};
  if (times && !PosixSubsystem::copyFromUser(&snapshot, times, sizeof(snapshot))) {
    F_NOTICE("utimes -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (times && (snapshot.actime < 0 || snapshot.modtime < 0 ||
                static_cast<uint64_t>(snapshot.actime) > UINT32_MAX ||
                static_cast<uint64_t>(snapshot.modtime) > UINT32_MAX)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  F_NOTICE("utime(" << pathCopy << ")");

  String realPath;
  normalisePath(realPath, pathCopy.cstr());

  ResolvedPath fileLease;
  File* file = findFilePath(realPath, fileLease);
  if (!file) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  // Symlink traversal
  file = traverseSymlink(file, fileLease);
  if (!file)
    return -1;

  if (!VFS::checkAccess(file, false, true, false)) {
    // checkAccess does a SYSCALL_ERROR for us.
    return -1;
  }

  Time::Timestamp accessTime;
  Time::Timestamp modifyTime;
  if (times) {
    accessTime = snapshot.actime;
    modifyTime = snapshot.modtime;
  } else {
    accessTime = modifyTime = Time::getTime();
  }

  file->setAccessedTime(accessTime);
  file->setModifiedTime(modifyTime);

  return 0;
}

int posix_utimes(const char* path, const struct timeval* times) {
  return posix_futimesat(AT_FDCWD, path, times);
}

int posix_chroot(const char* path) {
  auto* process = Processor::information().getCurrentThread()->getParent();
  if (process->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  String copied;
  if (!copyUserString(path, copied))
    return -1;
  String normalised;
  normalisePath(normalised, copied.cstr());
  ResolvedPath target;
  if (!findFilePath(normalised, target, FilesystemPathRef(), true))
    return -1;
  auto context = process->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  return view && context && view->changeRoot(context, target.path()) ? 0 : -1;
}

int posix_flock(int fd, int operation) {
  F_NOTICE("flock(" << fd << ", " << operation << ")");

  const int lockType = operation & ~LOCK_NB;
  if (lockType != LOCK_SH && lockType != LOCK_EX && lockType != LOCK_UN) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Process* process = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  return posix_advisory_flock(descriptor, operation);
}

static File* check_dirfd(int dirfd, DescriptorLease& descriptor, ResolvedPath& directory,
                         int flags = 0) {
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  auto* view = VFS::instance().mountView();
  if (!subsystem || !view) {
    SYSCALL_ERROR(DoesNotExist);
    return nullptr;
  }
  if (dirfd == AT_FDCWD) {
    auto context = process->acquireFilesystemContext();
    FilesystemContextSnapshot snapshot;
    if (!context || !context->snapshot(snapshot) || !snapshot.cwd) {
      SYSCALL_ERROR(DoesNotExist);
      return nullptr;
    }
    directory.retain(snapshot.cwd);
    return directory.get();
  }
  if (!subsystem->acquireFileDescriptor(dirfd, descriptor) || !descriptor->getFile()) {
    SYSCALL_ERROR(BadFileDescriptor);
    return nullptr;
  }
  File* file = descriptor->getFile();
  auto path = descriptor->openingPath();
  if (!(flags & AT_EMPTY_PATH) && (!file->isDirectory() || !path || !view->attachmentId(path))) {
    SYSCALL_ERROR(NotADirectory);
    return nullptr;
  }
  if (!path && !view->anonymousPath(file, path))
    return nullptr;
  directory.retain(path);
  return file;
}

static bool findParentPath(const String& pathname, const FilesystemPathRef& start,
                           ResolvedPath& result, String& basename) {
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  FilesystemPathRef parent;
  if (!context || !view) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (!view->resolveParent(context, start, pathname, parent, basename))
    return false;
  result.retain(parent);
  return true;
}

static File* findAtPath(int dirfd, const String& pathname, int flags, ResolvedPath& result,
                        DescriptorLease& descriptor) {
  ResolvedPath start;
  File* anchor = check_dirfd(pathname.length() && pathname[0] == '/' ? AT_FDCWD : dirfd, descriptor,
                             start, pathname.length() ? 0 : flags);
  if (!anchor)
    return nullptr;
  if (!pathname.length()) {
    if (!(flags & AT_EMPTY_PATH)) {
      SYSCALL_ERROR(DoesNotExist);
      return nullptr;
    }
    result.retain(start.path());
    return anchor;
  }
  String normalised;
  normalisePath(normalised, pathname.cstr());
  return findFilePath(normalised, result, start.path(), !(flags & AT_SYMLINK_NOFOLLOW));
}

int posix_openat(int dirfd, const char* pathname, int flags, mode_t mode) {
  F_NOTICE("openat");

  String pathnameCopy;
  if (!copyUserString(pathname, pathnameCopy)) {
    F_NOTICE("open -> invalid address");
    return -1;
  }

  ResolvedPath cwdLease;
  DescriptorLease dirDescriptor;
  File* cwd = check_dirfd(pathnameCopy.length() && pathnameCopy[0] == '/' ? AT_FDCWD : dirfd,
                          dirDescriptor, cwdLease);
  if (!cwd)
    return -1;

  const bool pathOnly = flags & O_PATH;
  if (pathOnly)
    flags &= O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC;

  F_NOTICE("openat(" << dirfd << ", " << pathnameCopy << ", " << flags << ", " << Oct << mode
                     << ")");

  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    F_NOTICE("  -> No subsystem for this process!");
    return -1;
  }

  // One of these three must be specified.
  if (!(CHECK_FLAG(flags, O_RDONLY) || CHECK_FLAG(flags, O_RDWR) || CHECK_FLAG(flags, O_WRONLY))) {
    F_NOTICE("One of O_RDONLY, O_WRONLY, or O_RDWR must be passed.");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // verify the filename - don't try to open a dud file
  if (pathnameCopy[0] == 0) {
    F_NOTICE("  -> File does not exist (null path).");
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  PosixProcess* pPosixProcess = getPosixProcess();
  if (pPosixProcess) {
    mode &= ~pPosixProcess->getMask();
  }

  size_t fd = pSubsystem->getFd();

  ResolvedPath fileLease;
  File* file = 0;

  bool openingCtty = false;
  Process::FileContextLease cttyLease;
  String nameToOpen;
  normalisePath(nameToOpen, pathnameCopy.cstr());
  file = findFilePath(nameToOpen, fileLease, cwdLease.path());

  bool bCreated = false;
  if (!file) {
    const size_t lookupError = Processor::information().getCurrentThread()->getErrno();
    if (lookupError && lookupError != Error::DoesNotExist) {
      pSubsystem->freeFd(fd);
      return -1;
    }
    if (flags & O_CREAT) {
      F_NOTICE("  {O_CREAT}");
      ResolvedPath parent;
      String basename;
      bool worked = findParentPath(nameToOpen, cwdLease.path(), parent, basename) &&
                    VFS::instance().mountView()->createFile(parent.path(), basename, mode);
      if (!worked) {
        // createFile should set the error if it fails.
        F_NOTICE("  -> File does not exist (createFile failed)");
        pSubsystem->freeFd(fd);
        return -1;
      }

      file = findFilePath(basename, fileLease, parent.path());
      if (!file) {
        F_NOTICE("  -> File does not exist (O_CREAT failed)");
        if (!Processor::information().getCurrentThread()->getErrno())
          SYSCALL_ERROR(DoesNotExist);
        pSubsystem->freeFd(fd);
        return -1;
      }

      bCreated = true;
    } else {
      F_NOTICE("  -> Does not exist.");
      // Error - not found.
      SYSCALL_ERROR(DoesNotExist);
      pSubsystem->freeFd(fd);
      return -1;
    }
  }

  if (!file) {
    F_NOTICE("  -> File does not exist.");
    SYSCALL_ERROR(DoesNotExist);
    pSubsystem->freeFd(fd);
    return -1;
  }

  if (file->isSymlink() && (flags & O_NOFOLLOW)) {
    if (!pathOnly) {
      SYSCALL_ERROR(LoopExists);
      pSubsystem->freeFd(fd);
      return -1;
    }
  } else {
    file = traverseSymlink(file, fileLease);
  }

  if (!file) {
    if (!Processor::information().getCurrentThread()->getErrno())
      SYSCALL_ERROR(DoesNotExist);
    pSubsystem->freeFd(fd);
    return -1;
  }

  if ((flags & O_DIRECTORY) && !file->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    pSubsystem->freeFd(fd);
    return -1;
  }
  if (pathOnly) {
    auto* descriptor = new FileDescriptor(fileLease.path(), 0, fd, 0, flags);
    if (!descriptor) {
      pSubsystem->freeFd(fd);
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    pSubsystem->addFileDescriptor(fd, descriptor);
    return static_cast<int>(fd);
  }

  if (file->isDirectory() && (flags & (O_WRONLY | O_RDWR))) {
    // Error - is directory.
    F_NOTICE("  -> Is a directory, and O_WRONLY or O_RDWR was specified.");
    SYSCALL_ERROR(IsADirectory);
    pSubsystem->freeFd(fd);
    return -1;
  }

  if ((flags & O_CREAT) && (flags & O_EXCL) && !bCreated) {
    // file exists with O_CREAT and O_EXCL
    F_NOTICE("  -> File exists");
    SYSCALL_ERROR(FileExists);
    pSubsystem->freeFd(fd);
    return -1;
  }

  if (g_pDevFs && g_pDevFs->isControllingTerminalSelector(file)) {
    openingCtty = true;
    file = pProcess->acquireCtty(cttyLease);
    if (!file) {
      pSubsystem->freeFd(fd);
      SYSCALL_ERROR(NoSuchDevice);
      return -1;
    }
    if (ConsoleManager::instance().isMasterConsole(file))
      file = ConsoleManager::instance().getOther(file);
  }

  const bool checkRead = (flags & O_ACCMODE) != O_WRONLY;

  UtsRef namespaceBacking;
  if (posix_uts_file_namespace(file, namespaceBacking) &&
      ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))) {
    SYSCALL_ERROR(PermissionDenied);
    pSubsystem->freeFd(fd);
    return -1;
  }

  // Handle side effects.
  if (ConsoleManager::instance().isPtySlaveLocked(file)) {
    F_NOTICE("  -> PTY slave is locked");
    SYSCALL_ERROR(IoError);
    pSubsystem->freeFd(fd);
    return -1;
  }

  File* newFile = file->open();
  if (!newFile) {
    pSubsystem->freeFd(fd);
    return -1;
  }

  // Check for the desired permissions.
  // Note: we are permitted to create a file that we cannot open for writing
  // again. It will be open for the original mode requested if it was
  // created.
  if (!bCreated) {
    if (!VFS::checkAccess(file, checkRead, flags & (O_WRONLY | O_RDWR | O_TRUNC), false)) {
      // checkAccess does a SYSCALL_ERROR for us.
      F_NOTICE("  -> file access denied.");
      pSubsystem->freeFd(fd);
      return -1;
    }
    // Check for the desired permissions.
    if ((newFile != file) &&
        (!VFS::checkAccess(newFile, checkRead, flags & (O_WRONLY | O_RDWR | O_TRUNC), false))) {
      // checkAccess does a SYSCALL_ERROR for us.
      F_NOTICE("  -> file access denied.");
      pSubsystem->freeFd(fd);
      return -1;
    }
  }

  // ensure we tweak the correct file now
  file = newFile;

  // Check for console (as we have special handling needed here)
  if (ConsoleManager::instance().isConsole(file)) {
    // If a master console, attempt to lock.
    if (ConsoleManager::instance().isMasterConsole(file)) {
      // Lock the master, we now own it.
      // Or, we don't - if someone else has it open for example.
      if (!ConsoleManager::instance().lockConsole(file)) {
        F_NOTICE("Couldn't lock pseudoterminal master");
        SYSCALL_ERROR(DeviceBusy);
        pSubsystem->freeFd(fd);
        return -1;
      }
    } else {
      // Slave - set as controlling unless noctty is set.
      if ((flags & O_NOCTTY) == 0 && !openingCtty) {
        F_NOTICE("  -> setting opened terminal '" << file->getName() << "' to be controlling");
        if (TerminalControl::attach(*static_cast<ConsoleFile*>(file), false, true) < 0) {
          pSubsystem->freeFd(fd);
          return -1;
        }
      }
    }
  }

  // Permissions were OK.
  if ((flags & O_TRUNC) && !file->isDirectory() && !file->isBlockDevice() && !file->isPipe() &&
      !file->isFifo() && file->getFilesystem() != g_pDevFs &&
      !ConsoleManager::instance().isConsole(file)) {
    F_NOTICE("  -> {O_TRUNC}");
    if (!file->resize(0)) {
      pSubsystem->freeFd(fd);
      return -1;
    }
  }

  // Final checks.
  if (file->isFifo()) {
    F_NOTICE("FIFO => checking if we have any readers");

    /// \todo should block until a reader is present if O_NONBLOCK is not
    /// set
    Pipe* pipe = Pipe::fromFile(file);
    if (flags & O_WRONLY) {
      const bool bCanBlock = !(flags & O_NONBLOCK);
      if (!pipe->waitForReader(bCanBlock)) {
        if (!bCanBlock) {
          // FIFO + no readers yet = ENXIO
          F_NOTICE("    -> ENXIO");
          SYSCALL_ERROR(NoSuchDevice);
        } else {
          F_NOTICE("    -> EINTR (fifo)");
          SYSCALL_ERROR(Interrupted);
        }
        pSubsystem->freeFd(fd);
        return -1;
      }

      F_NOTICE("FIFO => successfully observed a reader");
    }
  }

  FileDescriptor* f = fileLease.get() == file
                          ? new FileDescriptor(fileLease.path(), 0, fd, 0, flags)
                          : new FileDescriptor(file, 0, fd, 0, flags);
  if (!f || !f->terminalAvailable()) {
    delete f;
    pSubsystem->freeFd(fd);
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  if (f) {
    pSubsystem->addFileDescriptor(fd, f);
    file->publishEvent(FileEvents::Open);
  }

  F_NOTICE("    -> " << fd);

  return static_cast<int>(fd);
}

int posix_mkdirat(int dirfd, const char* pathname, mode_t mode) {
  String copied;
  if (!copyUserString(pathname, copied))
    return -1;
  ResolvedPath start, parent;
  DescriptorLease descriptor;
  if (!check_dirfd(copied.length() && copied[0] == '/' ? AT_FDCWD : dirfd, descriptor, start))
    return -1;
  String normalised, basename;
  normalisePath(normalised, copied.cstr());
  if (!findParentPath(normalised, start.path(), parent, basename))
    return -1;
  if (!basename.length() || basename == "." || basename == "..") {
    SYSCALL_ERROR(FileExists);
    return -1;
  }
  if (auto* process = getPosixProcess())
    mode &= ~process->getMask();
  return VFS::instance().mountView()->createDirectory(parent.path(), basename, mode) ? 0 : -1;
}

int posix_fchownat(int dirfd, const char* pathname, uid_t owner, gid_t group, int flags) {
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  String copied;
  if ((!pathname && !(flags & AT_EMPTY_PATH)) || (pathname && !copyUserString(pathname, copied))) {
    if (!pathname)
      SYSCALL_ERROR(BadAddress);
    return -1;
  }
  ResolvedPath selected;
  DescriptorLease descriptor;
  File* file = findAtPath(dirfd, copied, flags, selected, descriptor);
  return file && posix_chown_file(file, owner, group) ? 0 : -1;
}

int posix_futimesat(int dirfd, const char* pathname, const struct timeval* times) {
  F_NOTICE("futimesat");

  String pathnameCopy;
  if (!copyUserString(pathname, pathnameCopy)) {
    F_NOTICE("utimes -> invalid address");
    return -1;
  }
  struct timeval snapshot[2] = {};
  if (times && !PosixSubsystem::copyFromUser(snapshot, times, sizeof(snapshot))) {
    F_NOTICE("utimes -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  for (const struct timeval& value : snapshot) {
    if (times && (value.tv_sec < 0 || static_cast<uint64_t>(value.tv_sec) > UINT32_MAX ||
                  value.tv_usec < 0 || value.tv_usec >= 1000000)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
  }

  F_NOTICE("futimesat(" << dirfd << ", " << pathnameCopy << ", " << times << ")");

  String realPath;
  normalisePath(realPath, pathnameCopy.cstr());

  ResolvedPath fileLease;
  DescriptorLease descriptor;
  File* file = findAtPath(dirfd, realPath, 0, fileLease, descriptor);
  if (!file) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  // Symlink traversal
  file = traverseSymlink(file, fileLease);
  if (!file)
    return -1;

  if (!VFS::checkAccess(file, false, true, false)) {
    // checkAccess does a SYSCALL_ERROR for us.
    return -1;
  }

  Time::Timestamp accessTime;
  Time::Timestamp modifyTime;
  if (times) {
    accessTime = snapshot[0].tv_sec;
    modifyTime = snapshot[1].tv_sec;
  } else {
    accessTime = modifyTime = Time::getTime();
  }

  file->setAccessedTime(accessTime);
  file->setModifiedTime(modifyTime);

  return 0;
}

int posix_unlinkat(int dirfd, const char* pathname, int flags) {
  if (flags & ~AT_REMOVEDIR) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  String copied;
  if (!copyUserString(pathname, copied))
    return -1;
  ResolvedPath start, parent, selected;
  DescriptorLease descriptor;
  if (!check_dirfd(copied.length() && copied[0] == '/' ? AT_FDCWD : dirfd, descriptor, start))
    return -1;
  String normalised, basename;
  normalisePath(normalised, copied.cstr());
  LockGuard<Mutex> unixNamespaceGuard(UnixFilesystem::namespaceLock());
  if (!findParentPath(normalised, start.path(), parent, basename))
    return -1;
  if (!basename.length()) {
    syscallError(flags & AT_REMOVEDIR ? Error::DeviceBusy : Error::IsADirectory);
    return -1;
  }
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  VfsMountView::ResolveOptions options;
  options.followFinal = false;
  options.crossFinalMount = false;
  FilesystemPathRef entry;
  if (!VFS::instance().mountView()->resolve(context, parent.path(), basename, options, entry))
    return -1;
  selected.retain(entry);
  File* file = selected.get();
  if (copied.length() && copied[copied.length() - 1] == '/' && !file->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }
  if (file->isDirectory() != !!(flags & AT_REMOVEDIR)) {
    syscallError(file->isDirectory() ? Error::IsADirectory : Error::NotADirectory);
    return -1;
  }
  return VFS::instance().mountView()->remove(parent.path(), basename, file) ? 0 : -1;
}

int posix_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
  String source, destination;
  if (!copyUserString(oldpath, source) || !copyUserString(newpath, destination))
    return -1;
  ResolvedPath oldStart, newStart, oldParent, newParent;
  DescriptorLease oldDescriptor, newDescriptor;
  if (!check_dirfd(source.length() && source[0] == '/' ? AT_FDCWD : olddirfd, oldDescriptor,
                   oldStart) ||
      !check_dirfd(destination.length() && destination[0] == '/' ? AT_FDCWD : newdirfd,
                   newDescriptor, newStart))
    return -1;
  String realSource, realDestination, oldName, newName;
  normalisePath(realSource, source.cstr());
  normalisePath(realDestination, destination.cstr());
  if (!findParentPath(realSource, oldStart.path(), oldParent, oldName) ||
      !findParentPath(realDestination, newStart.path(), newParent, newName))
    return -1;
  LockGuard<Mutex> unixNamespaceGuard(UnixFilesystem::namespaceLock());
  const bool directory = (source.length() && source[source.length() - 1] == '/') ||
                         (destination.length() && destination[destination.length() - 1] == '/');
  return VFS::instance().mountView()->rename(oldParent.path(), oldName, newParent.path(), newName,
                                             false, directory)
             ? 0
             : -1;
}

int posix_linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags) {
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  String source, destination;
  if (!copyUserString(oldpath, source) || !copyUserString(newpath, destination))
    return -1;
  ResolvedPath target, start, parent;
  DescriptorLease sourceDescriptor, destinationDescriptor;
  const int lookupFlags =
      (flags & AT_EMPTY_PATH) | ((flags & AT_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW);
  if (!findAtPath(olddirfd, source, lookupFlags, target, sourceDescriptor))
    return -1;
  if (!check_dirfd(destination.length() && destination[0] == '/' ? AT_FDCWD : newdirfd,
                   destinationDescriptor, start))
    return -1;
  String normalised, basename;
  normalisePath(normalised, destination.cstr());
  if (!findParentPath(normalised, start.path(), parent, basename))
    return -1;
  return VFS::instance().mountView()->createLink(parent.path(), basename, target.path()) ? 0 : -1;
}

int posix_symlinkat(const char* oldpath, int newdirfd, const char* newpath) {
  String value, destination;
  if (!copyUserString(oldpath, value) || !copyUserString(newpath, destination))
    return -1;
  ResolvedPath start, parent;
  DescriptorLease descriptor;
  if (!check_dirfd(destination.length() && destination[0] == '/' ? AT_FDCWD : newdirfd, descriptor,
                   start))
    return -1;
  String normalised, basename;
  normalisePath(normalised, destination.cstr());
  if (!findParentPath(normalised, start.path(), parent, basename))
    return -1;
  return VFS::instance().mountView()->createSymlink(parent.path(), basename, value) ? 0 : -1;
}

int posix_readlinkat(int dirfd, const char* pathname, char* buf, size_t bufsiz) {
  F_NOTICE("readlinkat");

  String pathnameCopy;
  if (!copyUserString(pathname, pathnameCopy)) {
    F_NOTICE("readlink -> invalid address");
    return -1;
  }
  ResolvedPath cwdLease;
  DescriptorLease dirDescriptor;
  File* cwd = check_dirfd(pathnameCopy.length() && pathnameCopy[0] == '/' ? AT_FDCWD : dirfd,
                          dirDescriptor, cwdLease, pathnameCopy.length() ? 0 : AT_EMPTY_PATH);
  if (!cwd)
    return -1;

  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), bufsiz,
                                    PosixSubsystem::SafeWrite)) {
    F_NOTICE("readlink -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("readlinkat(" << dirfd << ", " << pathnameCopy << ", "
                         << reinterpret_cast<uintptr_t>(buf) << ", " << bufsiz << ")");

  String realPath;
  normalisePath(realPath, pathnameCopy.cstr());

  ResolvedPath fileLease;
  File* f = pathnameCopy.length() ? findFilePath(realPath, fileLease, cwdLease.path()) : cwd;
  if (!f) {
    if (!Processor::information().getCurrentThread()->getErrno())
      SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  if (!f->isSymlink()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (buf == 0)
    return -1;

  if (!bufsiz) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  // Symlink target loading is bounded to one PATH_MAX-sized buffer.
  const size_t capacity = bufsiz < PATH_MAX ? bufsiz : PATH_MAX;
  UniqueArray<char> target = UniqueArray<char>::allocate(capacity);
  if (!target) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const int length = Symlink::fromFile(f)->followLink(target.get(), capacity);
  if (length < 0) {
    return length;
  }
  if (!PosixSubsystem::copyToUser(buf, target.get(), static_cast<size_t>(length))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return length;
}

int posix_fchmodat(int dirfd, const char* pathname, mode_t mode, int flags) {
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  String copied;
  if ((!pathname && !(flags & AT_EMPTY_PATH)) || (pathname && !copyUserString(pathname, copied))) {
    if (!pathname)
      SYSCALL_ERROR(BadAddress);
    return -1;
  }
  ResolvedPath selected;
  DescriptorLease descriptor;
  File* file = findAtPath(dirfd, copied, flags, selected, descriptor);
  return file && posix_chmod_file(file, mode) ? 0 : -1;
}

int posix_faccessat(int dirfd, const char* pathname, int mode, int flags) {
  F_NOTICE("faccessat");

  constexpr int validModes = R_OK | W_OK | X_OK;
  constexpr int validFlags = AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH;
  if ((mode & ~validModes) || (flags & ~validFlags)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  String pathnameCopy;
  if (!copyUserString(pathname, pathnameCopy)) {
    F_NOTICE("access -> invalid address");
    return -1;
  }

  F_NOTICE("faccessat(" << dirfd << ", " << pathnameCopy << ", " << mode << ", " << flags << ")");

  if (!pathnameCopy.length() && !(flags & AT_EMPTY_PATH)) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  Process* process = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  FilesystemCredentials accessCredentials;
  if (flags & AT_EACCESS) {
    if (!Process::currentFilesystemCredentials(accessCredentials)) {
      SYSCALL_ERROR(PermissionDenied);
      return -1;
    }
  } else if (process->getType() == Process::Posix) {
    accessCredentials = static_cast<PosixProcess*>(process)->realFilesystemCredentials();
  } else {
    if (!Process::currentFilesystemCredentials(accessCredentials)) {
      SYSCALL_ERROR(PermissionDenied);
      return -1;
    }
    const int64_t uid = process->getUserId(), gid = process->getGroupId();
    if (uid < 0 || gid < 0) {
      SYSCALL_ERROR(PermissionDenied);
      return -1;
    }
    accessCredentials.uid = uid;
    accessCredentials.gid = gid;
  }
  Process::FilesystemAccessScope accessScope(accessCredentials);

  ResolvedPath fileLease;
  DescriptorLease descriptor;
  File* file = findAtPath(dirfd, pathnameCopy, flags, fileLease, descriptor);

  if (!file) {
    F_NOTICE("  -> '" << pathnameCopy << "' does not exist");
    if (!Processor::information().getCurrentThread()->getErrno())
      SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  // If we're only checking for existence, we're done here.
  if (mode == F_OK) {
    F_NOTICE("  -> ok");
    return 0;
  }

  if (!VFS::checkAccess(file, mode & R_OK, mode & W_OK, mode & X_OK, accessCredentials)) {
    // checkAccess does a SYSCALL_ERROR for us.
    F_NOTICE("  -> not ok");
    return -1;
  }

  F_NOTICE("  -> ok");
  return 0;
}

int posix_fstatat(int dirfd, const char* pathname, struct stat* buf, int flags) {
  F_NOTICE("fstatat");
  constexpr int noAutomount = 0x800;
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | noAutomount)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!pathname && !(flags & AT_EMPTY_PATH)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  if (!buf || !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), sizeof(struct stat),
                                            PosixSubsystem::SafeWrite)) {
    F_NOTICE("fstat -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  String pathnameCopy;
  if (pathname) {
    if (!copyUserString(pathname, pathnameCopy)) {
      F_NOTICE("fstat -> invalid address");
      return -1;
    }
  }

  if ((flags & AT_EMPTY_PATH) && !pathnameCopy.length() && dirfd != AT_FDCWD) {
    DescriptorLease descriptor;
    if (!acquireDescriptor(dirfd, descriptor)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    if (descriptor->getTimerFdImpl() || descriptor->getSignalFdImpl() ||
        descriptor->getFanotifyImpl()) {
      // Linux's anonymous descriptor types share a pseudo inode, without a
      // regular-file type or data extent. Keep its identity independent of
      // kernel addresses and outside the VFS's existing short device ids.
      struct stat snapshot = {};
      snapshot.st_dev = 0x10000;
      snapshot.st_ino = 1;
      snapshot.st_mode = 0600;
      snapshot.st_nlink = 1;
      snapshot.st_blksize = PhysicalMemoryManager::getPageSize();
      if (!PosixSubsystem::copyToUser(buf, &snapshot, sizeof(snapshot))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      return 0;
    }
    if (!descriptor->getFile()) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    struct stat snapshot = {};
    if (!doStat(0, descriptor->getFile(), &snapshot))
      return -1;
    if (!PosixSubsystem::copyToUser(buf, &snapshot, sizeof(snapshot))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return 0;
  }

  ResolvedPath fileLease;
  DescriptorLease descriptor;
  File* file = findAtPath(dirfd, pathnameCopy, flags, fileLease, descriptor);
  if (!file)
    return -1;

  struct stat snapshot = {};
  if (!doStat(0, file, &snapshot)) {
    return -1;
  }
  if (!PosixSubsystem::copyToUser(buf, &snapshot, sizeof(snapshot))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("    -> Success");
  return 0;
}

int posix_mknod(const char* pathname, mode_t mode, dev_t dev) {
  return posix_mknodat(AT_FDCWD, pathname, mode, dev);
}

static int do_statfs(File* file, struct statfs* userBuffer) {
  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(userBuffer), sizeof(struct statfs),
                                    PosixSubsystem::SafeWrite)) {
    F_NOTICE(" -> invalid address for buf [" << userBuffer << "]");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  if (!file) {
    F_NOTICE(" -> file does not exist");
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  Filesystem* pFs = file->getFilesystem();

  struct statfs value = {};
  struct statfs* buf = &value;
  /// \todo this is all terrible
  bool bFilled = false;
  if (pFs == g_pDevFs) {
    F_NOTICE(" -> file '" << file->getName() << "' is on devfs");

    // Special handling for devfs
    if (file->getName().compare("pts")) {
      F_NOTICE(" -> filling statfs struct with /dev/pts data");
      ByteSet(buf, 0, sizeof(*buf));
      buf->f_type = 0x1CD1;  // DEVPTS_SUPER_MAGIC
      buf->f_bsize = 4096;
      buf->f_namelen = PATH_MAX;
      buf->f_frsize = 4096;
      bFilled = true;
    }
  }

  if (!bFilled) {
    /// \todo none of this is really correct...
    F_NOTICE(" -> filling statfs struct with ext2 data");
    ByteSet(buf, 0, sizeof(*buf));
    buf->f_type = 0xEF53;  // EXT2_SUPER_MAGIC
    if (pFs->getDisk()) {
      buf->f_bsize = pFs->getDisk()->getBlockSize();
      buf->f_blocks = pFs->getDisk()->getSize() / pFs->getDisk()->getBlockSize();
    } else {
      buf->f_bsize = 4096;
      buf->f_blocks = 0x100000;  // incorrect...
    }
    buf->f_bfree = buf->f_blocks;
    buf->f_bavail = buf->f_blocks;
    buf->f_files = 1234;
    buf->f_ffree = 5678;
    buf->f_namelen = PATH_MAX;
    buf->f_frsize = 0;
  }

  if (!PosixSubsystem::copyToUser(userBuffer, &value, sizeof(value))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  F_NOTICE(" -> ok");
  return 0;
}

int posix_statfs(const char* path, struct statfs* buf) {
  F_NOTICE("statfs");

  String pathCopy;
  if (!copyUserString(path, pathCopy)) {
    F_NOTICE(" -> invalid address for path");
    return -1;
  }

  F_NOTICE("statfs(" << pathCopy << ")");
  String normalisedPath;
  normalisePath(normalisedPath, pathCopy.cstr());
  F_NOTICE(" -> actually performing statfs on " << normalisedPath);
  ResolvedPath fileLease;
  File* file = findFilePath(normalisedPath, fileLease);

  return do_statfs(file, buf);
}

int posix_fstatfs(int fd, struct statfs* buf) {
  F_NOTICE("fstatfs(" << fd << ")");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  return do_statfs(pFd->getFile(), buf);
}

void generate_mtab(String& result) {
  TerminationDeferral lifetime;
  result.clear();
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  Vector<VfsMountView::MountSnapshot> mounts;
  if (!view || !context || !view->snapshotMounts(context, mounts))
    return;
  auto escape = [](const String& value) {
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
        default: {
          const char byte[] = {value[i], 0};
          result += byte;
          break;
        }
      }
    }
    return result;
  };
  for (const auto& mount : mounts) {
    VFS::FilesystemPin pin;
    if (!mount.backing.pin(pin))
      continue;
    String source = escape(pin.filesystem()->getVolumeLabel());
    String path = escape(mount.path);
    String line;
    line.Format("%s %s unknown %s 0 0\n", source.length() ? source.cstr() : "none", path.cstr(),
                pin.filesystem()->isReadOnly() ? "ro" : "rw");
    result += line;
  }
}

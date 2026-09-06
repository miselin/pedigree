/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/utility.h"

#include <fcntl.h>
#include <stddef.h>
#include <termios.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/console/Console.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/ioctl.h>

namespace {
constexpr size_t TerminalDescriptor = 96;
constexpr size_t LinuxTermiosExtent = 36;
constexpr size_t LinuxControlCount = 19;

// Keep the guest musl layout independent of the hosted compiler's libc.
struct MuslAmd64Termios {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[32];
  uint32_t c_ispeed;
  uint32_t c_ospeed;
};

struct LegacyTermios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_cc[20];
  speed_t c_ispeed;
  speed_t c_ospeed;
};

static_assert(sizeof(MuslAmd64Termios) == 60, "musl amd64 termios extent");
static_assert(offsetof(MuslAmd64Termios, c_cc) == 17, "musl amd64 control characters");

struct TermiosContext {
  Process* process;
  ConsoleSlaveFile* console;
  bool getter = false;
  bool setter = false;
  bool extent = false;
  bool legacy = false;
  Atomic<size_t> returned = 0;
};

int termiosWorker(void* parameter) {
  TermiosContext* context = reinterpret_cast<TermiosContext*>(parameter);
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(context->process->getSubsystem());
  Thread* thread = Processor::information().getCurrentThread();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t reservedLength = pageSize * 2;
  uintptr_t address = 0;
  if (!context->process->allocateUserRange(Process::UserRegion::Normal, reservedLength, address)) {
    return 1;
  }
  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping || mappedAddress != address) {
    MemoryMapManager::instance().remove(address, pageSize);
    context->process->freeUserRange(Process::UserRegion::Normal, address, reservedLength);
    return 1;
  }

  subsystem->setAbi(PosixSubsystem::LinuxAbi);
  MuslAmd64Termios* user = reinterpret_cast<MuslAmd64Termios*>(address);
  ByteSet(user, 0xA5, sizeof(*user) + 16);
  context->getter = posix_ioctl(TerminalDescriptor, TCGETS, user) == 0 && user->c_line == 0;
  for (size_t i = 0; i < LinuxControlCount; ++i) {
    context->getter &= user->c_cc[i] == static_cast<uint8_t>(defaultControl[i]);
  }
  for (size_t i = LinuxTermiosExtent; i < sizeof(*user) + 16; ++i) {
    context->getter &= reinterpret_cast<uint8_t*>(user)[i] == 0xA5;
  }

  context->setter = true;
  const size_t commands[] = {TCSETS, TCSETSW, TCSETSF};
  for (size_t command : commands) {
    user->c_line = 0x7F;
    for (size_t i = 0; i < LinuxControlCount; ++i) {
      user->c_cc[i] = static_cast<uint8_t>(0x21 + i);
    }
    context->setter &= posix_ioctl(TerminalDescriptor, command, user) == 0;
    char control[MAX_CONTROL_CHAR] = {};
    ConsoleManager::instance().getControlChars(context->console, control);
    // Inspect the console independently: a shifted setter/getter pair can
    // otherwise round-trip while corrupting VINTR, VEOF, VMIN and VSUSP.
    for (size_t i = 0; i < LinuxControlCount; ++i) {
      context->setter &= static_cast<uint8_t>(control[i]) == static_cast<uint8_t>(0x21 + i);
    }
    context->setter &= control[VINTR] == user->c_cc[VINTR] && control[VEOF] == user->c_cc[VEOF] &&
                       control[VMIN] == user->c_cc[VMIN] && control[VSUSP] == user->c_cc[VSUSP];
  }

  void* exact = reinterpret_cast<void*>(address + pageSize - LinuxTermiosExtent);
  MemoryCopy(exact, user, LinuxTermiosExtent);
  context->extent = posix_ioctl(TerminalDescriptor, TCSETS, exact) == 0 &&
                    posix_ioctl(TerminalDescriptor, TCGETS, exact) == 0;
  void* shortBuffer = reinterpret_cast<void*>(address + pageSize - LinuxTermiosExtent + 1);
  char before[MAX_CONTROL_CHAR] = {};
  char after[MAX_CONTROL_CHAR] = {};
  ConsoleManager::instance().getControlChars(context->console, before);
  thread->setErrno(0);
  context->extent &= posix_ioctl(TerminalDescriptor, TCSETS, shortBuffer) == -1 &&
                     thread->getErrno() == Error::BadAddress;
  ConsoleManager::instance().getControlChars(context->console, after);
  context->extent &= !MemoryCompare(before, after, sizeof(before));
  thread->setErrno(0);
  context->extent &= posix_ioctl(TerminalDescriptor, TCGETS, shortBuffer) == -1 &&
                     thread->getErrno() == Error::BadAddress;

  subsystem->setAbi(PosixSubsystem::PosixAbi);
  LegacyTermios* legacy = reinterpret_cast<LegacyTermios*>(address);
  ByteSet(legacy, 0, sizeof(*legacy));
  legacy->c_lflag = ICANON;
  for (size_t i = 0; i < sizeof(legacy->c_cc); ++i) {
    legacy->c_cc[i] = static_cast<cc_t>(0x41 + i);
  }
  context->legacy = posix_ioctl(TerminalDescriptor, TCSETS, legacy) == 0;
  ConsoleManager::instance().getControlChars(context->console, after);
  for (size_t i = 0; i < sizeof(legacy->c_cc); ++i) {
    context->legacy &= static_cast<cc_t>(after[i]) == legacy->c_cc[i];
  }
  ByteSet(legacy, 0, sizeof(*legacy));
  context->legacy &= posix_ioctl(TerminalDescriptor, TCGETS, legacy) == 0 &&
                     legacy->c_lflag == ICANON && legacy->c_ispeed == 115200 &&
                     legacy->c_ospeed == 115200;
  for (size_t i = 0; i < sizeof(legacy->c_cc); ++i) {
    context->legacy &= legacy->c_cc[i] == static_cast<cc_t>(0x41 + i);
  }

  MemoryMapManager::instance().remove(address, pageSize);
  context->process->freeUserRange(Process::UserRegion::Normal, address, reservedLength);
  context->returned += 1;
  return 0;
}
}  // namespace

bool runHostedTermiosSyscallRegressions(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  ConsoleSlaveFile* console = new ConsoleSlaveFile(0, String("termios-test"), nullptr);
  subsystem->addFileDescriptor(TerminalDescriptor,
                               new FileDescriptor(console, 0, TerminalDescriptor, 0, O_RDWR));
  TermiosContext context{process, console};
  Thread* worker = new Thread(process, termiosWorker, &context, nullptr, false, true, true);
  worker->setName("hosted termios ABI");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool passed = started && joined && context.returned == 1 && context.getter &&
                      context.setter && context.extent && context.legacy;
  delete process;
  delete console;
  if (!passed) {
    ERROR("HOSTED-SYSCALL-TEST: FAIL termios-linux-amd64-abi: getter="
          << context.getter << ", setter=" << context.setter << ", extent=" << context.extent
          << ", legacy=" << context.legacy);
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS termios-linux-amd64-abi");
  return true;
}

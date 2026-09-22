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
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Tree.h"

#include <FileDescriptor.h>
#include <PosixProcess.h>
#include <PosixSubsystem.h>
#include <limits.h>
#include <stddef.h>
#include <termios.h>

#include "TerminalControl.h"
#include "console-syscalls.h"
#include "file-syscalls.h"
#include "logging.h"
#include "modules/system/console/Console.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"
#include <sys/ioctl.h>

#define NCCS_COMPATIBLE 20

struct termios_compatible {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_cc[NCCS_COMPATIBLE];
  speed_t __c_ispeed;
  speed_t __c_ospeed;
};

// TCGETS/TCSETS exchange the Linux kernel prefix, not musl's larger termios.
struct LinuxAmd64Termios {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[19];
};

static_assert(sizeof(LinuxAmd64Termios) == 36, "Linux amd64 termios extent");
static_assert(offsetof(LinuxAmd64Termios, c_cc) == 17, "Linux amd64 control characters");

int posix_tcgetattr(int fd, struct termios* p) {
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  return console_tcgetattr(descriptor, p);
}

int console_tcgetattr(const DescriptorLease& pFd, struct termios* p) {
  F_NOTICE("posix_tcgetattr(" << pFd->fd << ")");

  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for one or both of the processes!");
    return -1;
  }

  FileDescriptor::TerminalOperation operation;
  if (!pFd->acquireTerminalOperation(operation)) {
    SYSCALL_ERROR(IoError);
    return -1;
  }

  if (!ConsoleManager::instance().isConsole(pFd->getFile())) {
    // Error - not a TTY.
    SYSCALL_ERROR(NotAConsole);
    F_NOTICE(" -> ENOTTY");
    return -1;
  }

  termios_compatible attributes = {};
  termios_compatible* pc = &attributes;

  size_t flags;
  ConsoleManager::instance().getAttributes(pFd->getFile(), &flags);

  pc->c_iflag = ((flags & ConsoleManager::IMapNLToCR) ? INLCR : 0) |
                ((flags & ConsoleManager::IMapCRToNL) ? ICRNL : 0) |
                ((flags & ConsoleManager::IIgnoreCR) ? IGNCR : 0) |
                ((flags & ConsoleManager::IStripToSevenBits) ? ISTRIP : 0);
  pc->c_oflag = ((flags & ConsoleManager::OPostProcess) ? OPOST : 0) |
                ((flags & ConsoleManager::OMapCRToNL) ? OCRNL : 0) |
                ((flags & ConsoleManager::OMapNLToCRNL) ? ONLCR : 0) |
                ((flags & ConsoleManager::ONLCausesCR) ? ONLRET : 0);
  pc->c_cflag = CREAD | CS8 | HUPCL | B38400;
  pc->c_lflag = ((flags & ConsoleManager::LEcho) ? ECHO : 0) |
                ((flags & ConsoleManager::LEchoErase) ? ECHOE : 0) |
                ((flags & ConsoleManager::LEchoKill) ? ECHOK : 0) |
                ((flags & ConsoleManager::LEchoNewline) ? ECHONL : 0) |
                ((flags & ConsoleManager::LCookedMode) ? ICANON : 0) |
                ((flags & ConsoleManager::LGenerateEvent) ? ISIG : 0);

  char controlChars[MAX_CONTROL_CHAR] = {0};
  ConsoleManager::instance().getControlChars(pFd->getFile(), controlChars);

  // c_cc is of type cc_t, but we don't want to expose that type to
  // ConsoleManager. By doing this conversion, we can use whatever type we
  // like in the kernel.
  for (size_t i = 0; i < NCCS_COMPATIBLE; ++i)
    pc->c_cc[i] = controlChars[i];

  // ispeed/ospeed
  pc->__c_ispeed = 115200;
  pc->__c_ospeed = 115200;

  bool copied;
  if (pSubsystem->getAbi() == PosixSubsystem::LinuxAbi) {
    LinuxAmd64Termios result = {};
    result.c_iflag = pc->c_iflag;
    result.c_oflag = pc->c_oflag;
    result.c_cflag = pc->c_cflag;
    result.c_lflag = pc->c_lflag;
    for (size_t i = 0; i < sizeof(result.c_cc); ++i) {
      result.c_cc[i] = pc->c_cc[i];
    }
    copied = PosixSubsystem::copyToUser(p, &result, sizeof(result));
  } else {
    // Retain the historical direct-POSIX payload for non-Linux callers.
    copied = PosixSubsystem::copyToUser(p, &attributes, sizeof(attributes));
  }
  if (!copied) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("posix_tcgetattr returns");
  F_NOTICE(" -> {c_iflag=" << pc->c_iflag << ", c_oflag=" << pc->c_oflag
                           << ", c_lflag=" << pc->c_lflag << "}");
  F_NOTICE(" -> {c_cflag=" << pc->c_cflag << "}");
  F_NOTICE(" -> {c_ispeed=" << pc->__c_ispeed << ", c_ospeed=" << pc->__c_ospeed << "}");
  return 0;
}

int posix_tcsetattr(int fd, int optional_actions, struct termios* p) {
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  return console_tcsetattr(descriptor, optional_actions, p);
}

int console_tcsetattr(const DescriptorLease& pFd, int optional_actions, struct termios* p) {
  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for one or both of the processes!");
    return -1;
  }

  termios_compatible attributes = {};
  termios_compatible* pc = &attributes;
  size_t controlCount = NCCS_COMPATIBLE;
  if (pSubsystem->getAbi() == PosixSubsystem::LinuxAbi) {
    LinuxAmd64Termios input = {};
    if (!PosixSubsystem::copyFromUser(&input, p, sizeof(input))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    pc->c_iflag = input.c_iflag;
    pc->c_oflag = input.c_oflag;
    pc->c_cflag = input.c_cflag;
    pc->c_lflag = input.c_lflag;
    controlCount = sizeof(input.c_cc);
    for (size_t i = 0; i < controlCount; ++i) {
      pc->c_cc[i] = input.c_cc[i];
    }
  } else if (!PosixSubsystem::copyFromUser(&attributes, p, sizeof(attributes))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("posix_tcsetattr(" << pFd->fd << ", " << optional_actions << ")");
  F_NOTICE(" -> {c_iflag=" << pc->c_iflag << ", c_oflag=" << pc->c_oflag
                           << ", c_lflag=" << pc->c_lflag << "}");
  F_NOTICE(" -> {c_cflag=" << pc->c_cflag << "}");

  FileDescriptor::TerminalOperation operation;
  if (!pFd->acquireTerminalOperation(operation)) {
    SYSCALL_ERROR(IoError);
    return -1;
  }

  if (!ConsoleManager::instance().isConsole(pFd->getFile())) {
    // Error - not a TTY.
    SYSCALL_ERROR(NotAConsole);
    F_NOTICE(" -> ENOTTY");
    return -1;
  }

  size_t flags = 0;
  if (pc->c_iflag & INLCR)
    flags |= ConsoleManager::IMapNLToCR;
  if (pc->c_iflag & ICRNL)
    flags |= ConsoleManager::IMapCRToNL;
  if (pc->c_iflag & IGNCR)
    flags |= ConsoleManager::IIgnoreCR;
  if (pc->c_iflag & ISTRIP)
    flags |= ConsoleManager::IStripToSevenBits;
  if (pc->c_oflag & OPOST)
    flags |= ConsoleManager::OPostProcess;
  if (pc->c_oflag & OCRNL)
    flags |= ConsoleManager::OMapCRToNL;
  if (pc->c_oflag & ONLCR)
    flags |= ConsoleManager::OMapNLToCRNL;
  if (pc->c_oflag & ONLRET)
    flags |= ConsoleManager::ONLCausesCR;
  if (pc->c_lflag & ECHO)
    flags |= ConsoleManager::LEcho;
  if (pc->c_lflag & ECHOE)
    flags |= ConsoleManager::LEchoErase;
  if (pc->c_lflag & ECHOK)
    flags |= ConsoleManager::LEchoKill;
  if (pc->c_lflag & ECHONL)
    flags |= ConsoleManager::LEchoNewline;
  if (pc->c_lflag & ICANON)
    flags |= ConsoleManager::LCookedMode;
  if (pc->c_lflag & ISIG)
    flags |= ConsoleManager::LGenerateEvent;
  F_NOTICE("TCSETATTR: " << Hex << flags);
  /// \todo Sanity checks.
  ConsoleManager::instance().setAttributes(pFd->getFile(), flags);

  char controlChars[MAX_CONTROL_CHAR] = {0};
  for (size_t i = 0; i < controlCount; ++i)
    controlChars[i] = pc->c_cc[i];
  ConsoleManager::instance().setControlChars(pFd->getFile(), controlChars);

  return 0;
}

int console_getwinsize(File* file, struct winsize* buf) {
  if (!ConsoleManager::instance().isConsole(file)) {
    // Error - not a TTY.
    return -1;
  }

  return ConsoleManager::instance().getWindowSize(file, &buf->ws_row, &buf->ws_col);
}

int console_setwinsize(File* file, const struct winsize* buf) {
  if (!ConsoleManager::instance().isConsole(file)) {
    // Error - not a TTY.
    return -1;
  }

  /// \todo Send SIGWINCH to foreground process group (once we have one)
  return ConsoleManager::instance().setWindowSize(file, buf->ws_row, buf->ws_col);
}

int console_flush(File* file, void* what) {
  if (!ConsoleManager::instance().isConsole(file)) {
    // Error - not a TTY.
    return -1;
  }

  /// \todo handle 'what' parameter
  ConsoleManager::instance().flush(file);
  return 0;
}

int console_ptsname(int fd, char* buf) {
  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for one or both of the processes!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (!ConsoleManager::instance().isConsole(pFd->getFile())) {
    // Error - not a TTY.
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }

  File* slave = pFd->getFile();
  if (ConsoleManager::instance().isMasterConsole(slave)) {
    slave = ConsoleManager::instance().getOther(pFd->getFile());
  } else {
    return -1;
  }

  String path("/dev/");
  path += slave->getName();
  if (!PosixSubsystem::copyToUser(buf, path.cstr(), path.length() + 1)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("ptsname(" << fd << ") -> " << path);
  return 0;
}

int console_ttyname(int fd, char* buf) {
  // Lookup this process.
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for one or both of the processes!");
    return -1;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }

  if (!ConsoleManager::instance().isConsole(pFd->getFile())) {
    // Error - not a TTY.
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }

  auto* tty = static_cast<ConsoleFile*>(pFd->getFile());
  String path(tty->isPtySlave() ? "/dev/pts/" : "/dev/");
  path += tty->getName();
  if (!PosixSubsystem::copyToUser(buf, path.cstr(), path.length() + 1)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  F_NOTICE("ttyname(" << fd << ") -> " << path);
  return 0;
}

int console_setctty(File* file, bool steal) {
  if (!ConsoleManager::instance().isConsole(file)) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }
  return TerminalControl::attach(*static_cast<ConsoleFile*>(file), steal);
}

int console_setctty(int fd, bool steal) {
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (!ConsoleManager::instance().isConsole(descriptor->getFile())) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }
  return TerminalControl::attach(*static_cast<ConsoleFile*>(descriptor->getFile()), steal, false,
                                 descriptor->terminalEpoch());
}

int posix_tcsetpgrp(int fd, pid_t group) {
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (!ConsoleManager::instance().isConsole(descriptor->getFile())) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }
  return TerminalControl::setForeground(*static_cast<ConsoleFile*>(descriptor->getFile()), group,
                                        descriptor->terminalEpoch());
}

pid_t posix_tcgetpgrp(int fd) {
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (!ConsoleManager::instance().isConsole(descriptor->getFile())) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }
  return TerminalControl::foreground(*static_cast<ConsoleFile*>(descriptor->getFile()),
                                     descriptor->terminalEpoch());
}

unsigned int console_getptn(int fd) {
  F_NOTICE("console_getptn(" << fd << ")");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for one or both of the processes!");
    return ~0U;
  }

  DescriptorLease pFd;
  if (!pSubsystem->acquireFileDescriptor(fd, pFd)) {
    // Error - no such file descriptor.
    SYSCALL_ERROR(BadFileDescriptor);
    F_NOTICE(" -> EBADF");
    return ~0U;
  }

  if (!ConsoleManager::instance().isConsole(pFd->getFile())) {
    SYSCALL_ERROR(NotAConsole);
    F_NOTICE(" -> not a console!");
    return ~0U;
  }

  ConsoleFile* pConsole = static_cast<ConsoleFile*>(pFd->getFile());
  size_t result = pConsole->getConsoleNumber();
  if (result == ~0U) {
    // special case, it's a Console attached to a physical terminal instead
    // of a pseudoterminal
    SYSCALL_ERROR(NotAConsole);
    F_NOTICE(" -> unknown console number!");
    return ~0U;
  }
  F_NOTICE(" -> " << result);
  return result;
}

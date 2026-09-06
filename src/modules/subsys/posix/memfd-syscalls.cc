/* Copyright (c) 2026, Pedigree Developers. */
#include "memfd-syscalls.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "memfd-file.h"
#include "modules/system/vfs/VFS.h"

int posix_memfd_create(const char* name, unsigned int flags) {
  if (flags & ~(LinuxMemFd::CloseOnExec | LinuxMemFd::AllowSealing)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  TerminationDeferral lifetime;
  String label;
  const auto copied =
      PosixSubsystem::copyUserString(name, label, LinuxMemFd::MaximumNameLength + 1);
  if (copied != PosixSubsystem::UserStringSuccess) {
    syscallError(copied == PosixSubsystem::UserStringTooLong ? Error::InvalidArgument
                                                             : Error::BadAddress);
    return -1;
  }
  Process* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  FilesystemCredentials credentials;
  if (!Process::currentFilesystemCredentials(credentials)) {
    SYSCALL_ERROR(PermissionDenied);
    return -1;
  }
  String filename("memfd:");
  filename += label;
  auto* file =
      new MemFdFile(filename, flags & LinuxMemFd::AllowSealing, credentials.uid, credentials.gid);
  if (!file) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  // The OFD acquires its VFS lease only from an already tracked file.
  VFS::instance().trackFile(file);
  auto* descriptor = new FileDescriptor(file, 0, 0xFFFFFFFF,
                                        flags & LinuxMemFd::CloseOnExec ? FD_CLOEXEC : 0, O_RDWR);
  if (!descriptor || !descriptor->acquireOpenFileDescription()) {
    delete descriptor;
    VFS::instance().untrackFile(file);
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  DescriptorLease published;
  const size_t fd = subsystem->installFileDescriptor(descriptor, published);
  VFS::instance().untrackFile(file);
  return static_cast<int>(fd);
}

int posix_memfd_fcntl(const DescriptorLease& descriptor, int command, uintptr_t argument) {
  TerminationDeferral lifetime;
  if (!descriptor) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  auto description = descriptor->acquireOpenFileDescription();
  if (!description) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  MemFdFile* file = MemFdFile::fromFile(description->getFile());
  if (!file || (command != F_GET_SEALS && command != F_ADD_SEALS)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const int statusFlags = descriptor->getStatusFlags();
  if (statusFlags & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (command == F_GET_SEALS)
    return file->getSeals();
  const int access = statusFlags & O_ACCMODE;
  if (access != O_WRONLY && access != O_RDWR) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  // Linux's memfd fcntl entry accepts an unsigned int seal mask.
  return file->addSeals(static_cast<unsigned int>(argument));
}

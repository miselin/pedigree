/* Copyright (c) 2026, Pedigree Developers. */
#include "global-sync-syscalls.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"

int posix_syncfs(int fd) {
  int error = 0;
  {
    TerminationDeferral lifetime;
    DescriptorLease descriptor;
    auto* process = Processor::information().getCurrentThread()->getParent();
    auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
    if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor) ||
        (descriptor->getStatusFlags() & O_PATH)) {
      error = Error::BadFileDescriptor;
    } else {
      File* file = descriptor->getFile();
      const auto status = file && file->getFilesystem()
                              ? VFS::instance().syncFilesystem(file->getFilesystem())
                              : Filesystem::SyncStatus::Unsupported;
      switch (status) {
        case Filesystem::SyncStatus::Success:
          break;
        case Filesystem::SyncStatus::Unsupported:
          error = Error::OperationNotSupported;
          break;
        case Filesystem::SyncStatus::NoMemory:
          error = Error::OutOfMemory;
          break;
        case Filesystem::SyncStatus::IoError:
          error = Error::IoError;
          break;
      }
    }
  }
  // Final descriptor release may run backend callbacks; publish the selected
  // result only after those callbacks and filesystem pin retirement finish.
  syscallError(error);
  return error ? -1 : 0;
}

int posix_sync() {
  {
    TerminationDeferral lifetime;
    // syncAll logs each backend failure and still attempts the remaining mounts.
    const auto status = VFS::instance().syncAll();
    (void)status;
  }
  syscallError(0);
  return 0;
}

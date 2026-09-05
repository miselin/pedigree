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

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/subsys/posix/pipe-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/stat.h>
#include <sys/uio.h>

namespace {
constexpr size_t ReadDescriptor = 110;
constexpr size_t WriteDescriptor = 111;
constexpr size_t ReadWriteDescriptor = 112;
constexpr size_t NonBlockingWriteDescriptor = 113;
constexpr size_t LargeFileSize = (static_cast<size_t>(1) << 33) + 513;
constexpr uintptr_t LargeInode = (static_cast<uintptr_t>(1) << 33) + 0x5678;

class ContractFile final : public File {
 public:
  ContractFile(Filesystem* filesystem, File* parent)
      : File(String("contract-file"), 0, 0, 0, LargeInode, filesystem, LargeFileSize, parent),
        readCalls(0),
        writeCalls(0),
        m_Bytes{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'} {}

  size_t getBlockSize() const override {
    return 4096;
  }

  bool sync() override {
    return syncSucceeds;
  }

  size_t readCalls;
  size_t writeCalls;
  size_t backendError = 0;
  uint64_t maximumWrite = ~static_cast<uint64_t>(0);
  bool syncSucceeds = true;

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t offset, uint64_t size, uintptr_t buffer, bool) override {
    ++readCalls;
    if (offset >= sizeof(m_Bytes)) {
      return 0;
    }
    if (size > sizeof(m_Bytes) - offset) {
      size = sizeof(m_Bytes) - offset;
    }
    MemoryCopy(reinterpret_cast<void*>(buffer), m_Bytes + offset, size);
    return size;
  }

  uint64_t writeBytewise(uint64_t offset, uint64_t size, uintptr_t buffer, bool) override {
    ++writeCalls;
    if (backendError) {
      Processor::information().getCurrentThread()->setErrno(backendError);
    }
    if (size > maximumWrite) {
      size = maximumWrite;
    }
    if (offset >= sizeof(m_Bytes)) {
      return 0;
    }
    if (size > sizeof(m_Bytes) - offset) {
      size = sizeof(m_Bytes) - offset;
    }
    MemoryCopy(m_Bytes + offset, reinterpret_cast<void*>(buffer), size);
    return size;
  }

 private:
  char m_Bytes[8];
};

struct ContractContext {
  explicit ContractContext(ContractFile* target)
      : file(target),
        denied(false),
        allowed(false),
        open(false),
        metadata(false),
        writeErrors(false),
        returned(0) {}

  ContractFile* file;
  bool denied;
  bool allowed;
  bool open;
  bool metadata;
  bool writeErrors;
  Atomic<size_t> returned;
};

bool expectBadDescriptor(Thread* thread, int result) {
  return result == -1 && thread->getErrno() == Error::BadFileDescriptor;
}

int contractWorker(void* parameter) {
  ContractContext* context = reinterpret_cast<ContractContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  if (!process->getSpaceAllocator().allocate(pageSize, address)) {
    context->returned += 1;
    return 1;
  }
  uintptr_t mappedAddress = address;
  if (!MemoryMapManager::instance().mapAnon(mappedAddress, pageSize,
                                            MemoryMappedObject::Read | MemoryMappedObject::Write) ||
      mappedAddress != address) {
    MemoryMapManager::instance().remove(address, pageSize);
    process->getSpaceAllocator().free(address, pageSize);
    context->returned += 1;
    return 1;
  }

  char* data = reinterpret_cast<char*>(address);
  struct iovec* vector = reinterpret_cast<struct iovec*>(address + 64);
  struct stat* status = reinterpret_cast<struct stat*>(address + 128);
  char* path = reinterpret_cast<char*>(address + 512);
  int* pipes = reinterpret_cast<int*>(address + 768);
  *data = 'w';
  vector->iov_base = data;
  vector->iov_len = 1;

  bool denied = expectBadDescriptor(thread, posix_read(WriteDescriptor, data, 1));
  denied &= expectBadDescriptor(thread, posix_readv(WriteDescriptor, vector, 1));
  denied &= expectBadDescriptor(thread, posix_write(ReadDescriptor, data, 1));
  denied &= expectBadDescriptor(thread, posix_writev(ReadDescriptor, vector, 1));
  denied &= expectBadDescriptor(thread, posix_read(WriteDescriptor, data, 0));
  denied &= expectBadDescriptor(thread, posix_write(ReadDescriptor, data, 0));
  denied &= expectBadDescriptor(thread, posix_readv(WriteDescriptor, vector, 0));
  denied &= expectBadDescriptor(thread, posix_writev(ReadDescriptor, vector, 0));
  vector->iov_len = 0;
  denied &= expectBadDescriptor(thread, posix_readv(WriteDescriptor, vector, 1));
  denied &= expectBadDescriptor(thread, posix_writev(ReadDescriptor, vector, 1));
  context->denied = denied && !context->file->readCalls && !context->file->writeCalls;

  vector->iov_len = 1;
  bool allowed = posix_read(ReadDescriptor, data, 1) == 1 && *data == 'a';
  allowed &= posix_readv(ReadDescriptor, vector, 1) == 1 && *data == 'b';
  *data = 'w';
  allowed &= posix_write(WriteDescriptor, data, 1) == 1;
  *data = 'v';
  allowed &= posix_writev(WriteDescriptor, vector, 1) == 1;
  allowed &= posix_read(ReadWriteDescriptor, data, 1) == 1 && *data == 'w';
  *data = 'r';
  allowed &= posix_write(ReadWriteDescriptor, data, 1) == 1;
  allowed &= posix_readv(ReadWriteDescriptor, vector, 1) == 1 && *data == 'c';
  *data = 's';
  allowed &= posix_writev(ReadWriteDescriptor, vector, 1) == 1;
  allowed &= posix_read(ReadDescriptor, data, 0) == 0;
  allowed &= posix_write(WriteDescriptor, data, 0) == 0;
  allowed &= posix_readv(ReadDescriptor, vector, 0) == 0;
  allowed &= posix_writev(WriteDescriptor, vector, 0) == 0;
  context->allowed = allowed;

  StringCopy(path, "/contract-write-only");
  thread->setErrno(0);
  const int deniedOpen = posix_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
  bool open = deniedOpen == -1 && thread->getErrno() == Error::PermissionDenied;
  if (deniedOpen >= 0) {
    posix_close(deniedOpen);
  }
  const int writeOpen = posix_openat(AT_FDCWD, path, O_WRONLY | O_CLOEXEC, 0);
  open &= writeOpen >= 0;
  if (writeOpen >= 0) {
    open &= posix_close(writeOpen) == 0;
  }
  StringCopy(path, "/contract-file");
  const int readOpen = posix_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
  open &= readOpen >= 0;
  if (readOpen >= 0) {
    open &= posix_close(readOpen) == 0;
  }
  context->open = open;

  bool metadata = posix_fstat(ReadDescriptor, status) == 0 && S_ISREG(status->st_mode) &&
                  status->st_size == static_cast<off_t>(LargeFileSize) &&
                  status->st_ino == LargeInode && status->st_blksize == 4096 &&
                  status->st_blocks == static_cast<blkcnt_t>(LargeFileSize / 512 + 1);
  if (posix_pipe(pipes) == 0) {
    metadata &= posix_fstat(pipes[0], status) == 0 && S_ISFIFO(status->st_mode);
    metadata &= posix_close(pipes[0]) == 0;
    metadata &= posix_close(pipes[1]) == 0;
  } else {
    metadata = false;
  }
  context->metadata = metadata;

  bool writeErrors = true;
  context->file->backendError = Error::NoSpaceLeftOnDevice;
  context->file->maximumWrite = 0;
  vector->iov_len = 1;
  const size_t writeDescriptors[] = {WriteDescriptor, NonBlockingWriteDescriptor};
  for (size_t descriptor : writeDescriptors) {
    writeErrors &=
        posix_write(descriptor, data, 1) == -1 && thread->getErrno() == Error::NoSpaceLeftOnDevice;
    writeErrors &= posix_pwrite64(descriptor, data, 1, 0) == -1 &&
                   thread->getErrno() == Error::NoSpaceLeftOnDevice;
    writeErrors &= posix_writev(descriptor, vector, 1) == -1 &&
                   thread->getErrno() == Error::NoSpaceLeftOnDevice;
    writeErrors &= posix_pwritev(descriptor, vector, 1, 0) == -1 &&
                   thread->getErrno() == Error::NoSpaceLeftOnDevice;
  }
  context->file->backendError = 0;
  context->file->maximumWrite = ~static_cast<uint64_t>(0);
  thread->setErrno(Error::IoError);
  writeErrors &= posix_write(WriteDescriptor, data, 1) == 1 && thread->getErrno() == 0;
  thread->setErrno(Error::IoError);
  writeErrors &= posix_pwrite64(WriteDescriptor, data, 1, 0) == 1 && thread->getErrno() == 0;
  thread->setErrno(Error::IoError);
  writeErrors &= posix_writev(WriteDescriptor, vector, 1) == 1 && thread->getErrno() == 0;
  thread->setErrno(Error::IoError);
  writeErrors &= posix_pwritev(WriteDescriptor, vector, 1, 0) == 1 && thread->getErrno() == 0;

  context->file->backendError = Error::IoError;
  context->file->maximumWrite = 1;
  vector->iov_len = 2;
  writeErrors &= posix_write(WriteDescriptor, data, 2) == 1 && thread->getErrno() == 0;
  writeErrors &= posix_pwrite64(WriteDescriptor, data, 2, 0) == 1 && thread->getErrno() == 0;
  writeErrors &= posix_writev(WriteDescriptor, vector, 1) == 1 && thread->getErrno() == 0;
  writeErrors &= posix_pwritev(WriteDescriptor, vector, 1, 0) == 1 && thread->getErrno() == 0;
  context->file->backendError = 0;
  context->file->maximumWrite = ~static_cast<uint64_t>(0);
  context->file->syncSucceeds = false;
  writeErrors &= posix_fsync(ReadDescriptor) == -1 && thread->getErrno() == Error::IoError;
  context->file->syncSucceeds = true;
  writeErrors &= posix_fsync(ReadDescriptor) == 0;
  writeErrors &= posix_fsync(-1) == -1 && thread->getErrno() == Error::BadFileDescriptor;
  context->writeErrors = writeErrors;

  MemoryMapManager::instance().remove(address, pageSize);
  process->getSpaceAllocator().free(address, pageSize);
  context->returned += 1;
  return 0;
}
}  // namespace

bool runHostedFileContractRegressions(Process* kernelProcess) {
  Filesystem* priorRoot = VFS::instance().getRootFilesystem();
  if (priorRoot) {
    ERROR("HOSTED-SYSCALL-TEST: FAIL file-contracts: fixture requires an empty root namespace");
    return false;
  }

  UnixFilesystem* filesystem = new UnixFilesystem;
  Filesystem* displacedRoot = VFS::instance().swapRootFilesystemForHostedTest(filesystem);
  File* root = filesystem->getRoot();
  UnixDirectory* directory = static_cast<UnixDirectory*>(Directory::fromFile(root));
  ContractFile* file = new ContractFile(filesystem, root);
  file->setUid(200);
  file->setPermissions(FILE_UR | FILE_UW);
  const bool fileAdded = directory->addEntry(file->getName(), file);
  File* writeOnly = new File(String("contract-write-only"), 0, 0, 0, 2, filesystem, 0, root);
  writeOnly->setUid(200);
  writeOnly->setPermissions(FILE_UW);
  const bool writeOnlyAdded = directory->addEntry(writeOnly->getName(), writeOnly);

  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  subsystem->setAbi(PosixSubsystem::LinuxAbi);
  process->setCwd(root);
  process->setUserId(200);
  process->setEffectiveUserId(200);
  subsystem->addFileDescriptor(ReadDescriptor,
                               new FileDescriptor(file, 0, ReadDescriptor, 0, O_RDONLY));
  subsystem->addFileDescriptor(WriteDescriptor,
                               new FileDescriptor(file, 0, WriteDescriptor, 0, O_WRONLY));
  subsystem->addFileDescriptor(ReadWriteDescriptor,
                               new FileDescriptor(file, 0, ReadWriteDescriptor, 0, O_RDWR));
  subsystem->addFileDescriptor(
      NonBlockingWriteDescriptor,
      new FileDescriptor(file, 0, NonBlockingWriteDescriptor, 0, O_WRONLY | O_NONBLOCK));

  ContractContext context(file);
  Thread* worker = new Thread(process, contractWorker, &context, nullptr, false, true, true);
  worker->setName("hosted file contracts");
  const bool started = fileAdded && writeOnlyAdded && displacedRoot == priorRoot && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  bool passed = started && joined && context.returned == 1 && context.denied && context.allowed &&
                context.open && context.metadata && context.writeErrors;
  subsystem->freeMultipleFds();
  process->setCwd(nullptr);
  delete process;
  Filesystem* removedRoot = VFS::instance().swapRootFilesystemForHostedTest(priorRoot);
  passed = removedRoot == filesystem && passed;
  delete filesystem;

  if (!passed) {
    ERROR("HOSTED-SYSCALL-TEST: FAIL file-contracts: denied="
          << context.denied << ", allowed=" << context.allowed << ", open=" << context.open
          << ", metadata=" << context.metadata << ", writeErrors=" << context.writeErrors);
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS file-contracts");
  return true;
}

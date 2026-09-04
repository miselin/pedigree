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
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Pipe.h"

namespace {
constexpr size_t BounceCapacity = PIPE_BUF_MAX + 1;
constexpr size_t ChunkedLength = BounceCapacity * 2 + 17;
constexpr size_t FaultLength = BounceCapacity * 2;
constexpr int PreservedErrno = 147;

enum class FaultMode { None, WriteAfterFirst, ReadBeforeSecondCopy };

bool closeDescriptor(PosixSubsystem* subsystem, size_t fd) {
  DescriptorLease descriptor;
  return subsystem->acquireFileDescriptor(fd, descriptor) &&
         subsystem->closeFileDescriptor(fd, descriptor);
}

bool allocateUserMapping(Process* process, size_t length, uintptr_t& address) {
  address = 0;
  if (!process->getSpaceAllocator().allocate(length, address)) {
    return false;
  }

  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, length, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping || mappedAddress != address) {
    MemoryMapManager::instance().remove(address, length);
    process->getSpaceAllocator().free(address, length);
    address = 0;
    return false;
  }
  return true;
}

class PositionalProbeFile final : public File {
 public:
  explicit PositionalProbeFile(FaultMode faultMode)
      : File(String("positional-io-probe"), 0, 0, 0, 1, nullptr, 65536, nullptr),
        m_FaultMode(faultMode),
        m_UserBase(0),
        m_MappingLength(0),
        m_PageSize(0),
        m_ReadCalls(0),
        m_WriteCalls(0),
        m_SawRawPointer(false),
        m_ReadOffsets{0, 0, 0, 0},
        m_WriteOffsets{0, 0, 0, 0},
        m_ReadSizes{0, 0, 0, 0},
        m_WriteSizes{0, 0, 0, 0},
        m_FirstWriteValues{0, 0, 0, 0} {}

  void configure(uintptr_t userBase, size_t mappingLength, size_t pageSize) {
    m_UserBase = userBase;
    m_MappingLength = mappingLength;
    m_PageSize = pageSize;
  }

  size_t readCalls() const {
    return m_ReadCalls;
  }

  size_t writeCalls() const {
    return m_WriteCalls;
  }

  uint64_t readOffset(size_t index) const {
    return m_ReadOffsets[index];
  }

  uint64_t writeOffset(size_t index) const {
    return m_WriteOffsets[index];
  }

  size_t readSize(size_t index) const {
    return m_ReadSizes[index];
  }

  size_t writeSize(size_t index) const {
    return m_WriteSizes[index];
  }

  char firstWriteValue(size_t index) const {
    return m_FirstWriteValues[index];
  }

  bool sawRawPointer() const {
    return m_SawRawPointer;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = m_ReadCalls;
    m_ReadCalls += 1;
    recordPointer(buffer);
    if (slot < 4) {
      m_ReadOffsets[slot] = location;
      m_ReadSizes[slot] = size;
    }
    ByteSet(reinterpret_cast<void*>(buffer), static_cast<char>('a' + slot), size);

    if (m_FaultMode == FaultMode::ReadBeforeSecondCopy && slot == 1) {
      MemoryMapManager::instance().setPermissions(
          m_UserBase + m_PageSize, m_MappingLength - m_PageSize, MemoryMappedObject::Read);
    }
    return size;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = m_WriteCalls;
    m_WriteCalls += 1;
    recordPointer(buffer);
    if (slot < 4) {
      m_WriteOffsets[slot] = location;
      m_WriteSizes[slot] = size;
      if (size) {
        m_FirstWriteValues[slot] = *reinterpret_cast<const char*>(buffer);
      }
    }

    if (m_FaultMode == FaultMode::WriteAfterFirst && slot == 0) {
      MemoryMapManager::instance().setPermissions(
          m_UserBase + m_PageSize, m_MappingLength - m_PageSize, MemoryMappedObject::None);
    }
    if (location + size > getSize()) {
      setSize(location + size);
    }
    return size;
  }

 private:
  void recordPointer(uintptr_t buffer) {
    if (buffer >= m_UserBase && buffer < m_UserBase + m_MappingLength) {
      m_SawRawPointer = true;
    }
  }

  FaultMode m_FaultMode;
  uintptr_t m_UserBase;
  size_t m_MappingLength;
  size_t m_PageSize;
  Atomic<size_t> m_ReadCalls;
  Atomic<size_t> m_WriteCalls;
  bool m_SawRawPointer;
  uint64_t m_ReadOffsets[4];
  uint64_t m_WriteOffsets[4];
  size_t m_ReadSizes[4];
  size_t m_WriteSizes[4];
  char m_FirstWriteValues[4];
};

struct PositionalIoContext {
  PositionalIoContext(Process* process, PositionalProbeFile* policyWrite,
                      PositionalProbeFile* policyRead, PositionalProbeFile* faultWrite,
                      PositionalProbeFile* faultRead)
      : process(process),
        policyWrite(policyWrite),
        policyRead(policyRead),
        faultWrite(faultWrite),
        faultRead(faultRead),
        policyWriteResult(-2),
        policyWriteError(0),
        policyReadResult(-2),
        policyReadError(0),
        faultWriteResult(-2),
        faultWriteError(0),
        faultReadResult(-2),
        faultReadError(0),
        negativeResult(-2),
        negativeError(0),
        overflowResult(-2),
        overflowError(0),
        oversizedResult(-2),
        oversizedError(0),
        pipeReadResult(-2),
        pipeReadError(0),
        pipeWriteResult(-2),
        pipeWriteError(0),
        wrongReadResult(-2),
        wrongReadError(0),
        wrongWriteResult(-2),
        wrongWriteError(0),
        badAddressResult(-2),
        badAddressError(0),
        readValues{0, 0, 0},
        setup(false),
        returned(0) {}

  Process* process;
  PositionalProbeFile* policyWrite;
  PositionalProbeFile* policyRead;
  PositionalProbeFile* faultWrite;
  PositionalProbeFile* faultRead;
  ssize_t policyWriteResult;
  int policyWriteError;
  ssize_t policyReadResult;
  int policyReadError;
  ssize_t faultWriteResult;
  int faultWriteError;
  ssize_t faultReadResult;
  int faultReadError;
  ssize_t negativeResult;
  int negativeError;
  ssize_t overflowResult;
  int overflowError;
  ssize_t oversizedResult;
  int oversizedError;
  ssize_t pipeReadResult;
  int pipeReadError;
  ssize_t pipeWriteResult;
  int pipeWriteError;
  ssize_t wrongReadResult;
  int wrongReadError;
  ssize_t wrongWriteResult;
  int wrongWriteError;
  ssize_t badAddressResult;
  int badAddressError;
  char readValues[3];
  bool setup;
  Atomic<size_t> returned;
};

int positionalIoWorker(void* parameter) {
  constexpr int PolicyWriteDescriptor = 90;
  constexpr int PolicyReadDescriptor = 91;
  constexpr int FaultWriteDescriptor = 92;
  constexpr int FaultReadDescriptor = 93;
  constexpr int PipeReadDescriptor = 94;
  constexpr int PipeWriteDescriptor = 95;

  PositionalIoContext* context = reinterpret_cast<PositionalIoContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = pageSize * 4;
  uintptr_t address = 0;
  if (!allocateUserMapping(context->process, mappingLength, address)) {
    context->returned += 1;
    return 1;
  }

  context->policyWrite->configure(address, mappingLength, pageSize);
  context->policyRead->configure(address, mappingLength, pageSize);
  context->faultWrite->configure(address, mappingLength, pageSize);
  context->faultRead->configure(address, mappingLength, pageSize);

  char* userBuffer = reinterpret_cast<char*>(address);
  ByteSet(userBuffer, 'w', mappingLength);
  thread->setErrno(PreservedErrno);
  context->policyWriteResult = posix_pwrite64(PolicyWriteDescriptor, userBuffer, ChunkedLength, 7);
  context->policyWriteError = thread->getErrno();

  ByteSet(userBuffer, 0, mappingLength);
  thread->setErrno(PreservedErrno);
  context->policyReadResult = posix_pread64(PolicyReadDescriptor, userBuffer, ChunkedLength, 19);
  context->policyReadError = thread->getErrno();
  context->readValues[0] = userBuffer[0];
  context->readValues[1] = userBuffer[BounceCapacity];
  context->readValues[2] = userBuffer[BounceCapacity * 2];

  ByteSet(userBuffer, 'f', mappingLength);
  thread->setErrno(PreservedErrno);
  context->faultWriteResult = posix_pwrite64(FaultWriteDescriptor, userBuffer, FaultLength, 31);
  context->faultWriteError = thread->getErrno();
  MemoryMapManager::instance().setPermissions(address, mappingLength,
                                              MemoryMappedObject::Read | MemoryMappedObject::Write);

  ByteSet(userBuffer, 0, mappingLength);
  thread->setErrno(PreservedErrno);
  context->faultReadResult = posix_pread64(FaultReadDescriptor, userBuffer, FaultLength, 41);
  context->faultReadError = thread->getErrno();
  MemoryMapManager::instance().setPermissions(address, mappingLength,
                                              MemoryMappedObject::Read | MemoryMappedObject::Write);

  thread->setErrno(0);
  context->negativeResult = posix_pread64(PolicyReadDescriptor, userBuffer, 1, -1);
  context->negativeError = thread->getErrno();
  thread->setErrno(0);
  context->overflowResult = posix_pwrite64(PolicyWriteDescriptor, userBuffer, 2, INT64_MAX);
  context->overflowError = thread->getErrno();
  thread->setErrno(0);
  context->oversizedResult =
      posix_pread64(PolicyReadDescriptor, userBuffer, static_cast<size_t>(SSIZE_MAX) + 1, 0);
  context->oversizedError = thread->getErrno();

  thread->setErrno(0);
  context->pipeReadResult = posix_pread64(PipeReadDescriptor, userBuffer, 1, 0);
  context->pipeReadError = thread->getErrno();
  thread->setErrno(0);
  context->pipeWriteResult = posix_pwrite64(PipeWriteDescriptor, userBuffer, 1, 0);
  context->pipeWriteError = thread->getErrno();

  thread->setErrno(0);
  context->wrongReadResult = posix_pread64(PolicyWriteDescriptor, userBuffer, 1, 0);
  context->wrongReadError = thread->getErrno();
  thread->setErrno(0);
  context->wrongWriteResult = posix_pwrite64(PolicyReadDescriptor, userBuffer, 1, 0);
  context->wrongWriteError = thread->getErrno();

  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  thread->setErrno(0);
  context->badAddressResult =
      posix_pwrite64(PolicyWriteDescriptor, reinterpret_cast<const char*>(kernelStart), 1, 0);
  context->badAddressError = thread->getErrno();

  MemoryMapManager::instance().remove(address, mappingLength);
  context->process->getSpaceAllocator().free(address, mappingLength);
  context->setup = true;
  context->returned += 1;
  return 0;
}

bool positionalIoSemantics(Process* kernelProcess) {
  constexpr size_t PolicyWriteDescriptor = 90;
  constexpr size_t PolicyReadDescriptor = 91;
  constexpr size_t FaultWriteDescriptor = 92;
  constexpr size_t FaultReadDescriptor = 93;
  constexpr size_t PipeReadDescriptor = 94;
  constexpr size_t PipeWriteDescriptor = 95;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  PositionalProbeFile policyWrite(FaultMode::None);
  PositionalProbeFile policyRead(FaultMode::None);
  PositionalProbeFile faultWrite(FaultMode::WriteAfterFirst);
  PositionalProbeFile faultRead(FaultMode::ReadBeforeSecondCopy);
  subsystem->addFileDescriptor(
      PolicyWriteDescriptor,
      new FileDescriptor(&policyWrite, 123, PolicyWriteDescriptor, 0, O_WRONLY | O_APPEND));
  subsystem->addFileDescriptor(
      PolicyReadDescriptor,
      new FileDescriptor(&policyRead, 147, PolicyReadDescriptor, 0, O_RDONLY | O_APPEND));
  subsystem->addFileDescriptor(
      FaultWriteDescriptor,
      new FileDescriptor(&faultWrite, 173, FaultWriteDescriptor, 0, O_WRONLY | O_APPEND));
  subsystem->addFileDescriptor(
      FaultReadDescriptor, new FileDescriptor(&faultRead, 197, FaultReadDescriptor, 0, O_RDONLY));

  Pipe* pipe = new Pipe;
  subsystem->addFileDescriptor(PipeReadDescriptor,
                               new FileDescriptor(pipe, 0, PipeReadDescriptor, 0, O_RDONLY));
  subsystem->addFileDescriptor(PipeWriteDescriptor,
                               new FileDescriptor(pipe, 0, PipeWriteDescriptor, 0, O_WRONLY));

  PositionalIoContext context(process, &policyWrite, &policyRead, &faultWrite, &faultRead);
  Thread* worker = new Thread(process, positionalIoWorker, &context, nullptr, false, true, true);
  worker->setName("hosted positional I/O semantics");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  DescriptorLease policyWriter;
  DescriptorLease policyReader;
  DescriptorLease faultWriter;
  DescriptorLease faultReader;
  const bool acquired = subsystem->acquireFileDescriptor(PolicyWriteDescriptor, policyWriter) &&
                        subsystem->acquireFileDescriptor(PolicyReadDescriptor, policyReader) &&
                        subsystem->acquireFileDescriptor(FaultWriteDescriptor, faultWriter) &&
                        subsystem->acquireFileDescriptor(FaultReadDescriptor, faultReader);

  bool passed =
      started && joined && context.returned == 1 && context.setup && acquired &&
      context.policyWriteResult == static_cast<ssize_t>(ChunkedLength) &&
      context.policyWriteError == PreservedErrno &&
      context.policyReadResult == static_cast<ssize_t>(ChunkedLength) &&
      context.policyReadError == PreservedErrno && policyWriter->getOffset() == 123 &&
      policyReader->getOffset() == 147 && policyWrite.writeCalls() == 3 &&
      policyWrite.writeOffset(0) == 7 && policyWrite.writeOffset(1) == 7 + BounceCapacity &&
      policyWrite.writeOffset(2) == 7 + BounceCapacity * 2 &&
      policyWrite.writeSize(0) == BounceCapacity && policyWrite.writeSize(1) == BounceCapacity &&
      policyWrite.writeSize(2) == 17 && policyWrite.firstWriteValue(0) == 'w' &&
      policyWrite.firstWriteValue(1) == 'w' && policyWrite.firstWriteValue(2) == 'w' &&
      !policyWrite.sawRawPointer() && policyRead.readCalls() == 3 &&
      policyRead.readOffset(0) == 19 && policyRead.readOffset(1) == 19 + BounceCapacity &&
      policyRead.readOffset(2) == 19 + BounceCapacity * 2 &&
      policyRead.readSize(0) == BounceCapacity && policyRead.readSize(1) == BounceCapacity &&
      policyRead.readSize(2) == 17 && !policyRead.sawRawPointer() && context.readValues[0] == 'a' &&
      context.readValues[1] == 'b' && context.readValues[2] == 'c' &&
      context.faultWriteResult == static_cast<ssize_t>(BounceCapacity) &&
      context.faultWriteError == PreservedErrno &&
      context.faultReadResult == static_cast<ssize_t>(BounceCapacity) &&
      context.faultReadError == PreservedErrno && faultWriter->getOffset() == 173 &&
      faultReader->getOffset() == 197 && faultWrite.writeCalls() == 1 &&
      faultWrite.writeOffset(0) == 31 && !faultWrite.sawRawPointer() &&
      faultRead.readCalls() == 2 && faultRead.readOffset(0) == 41 &&
      faultRead.readOffset(1) == 41 + BounceCapacity && !faultRead.sawRawPointer() &&
      context.negativeResult == -1 && context.negativeError == Error::InvalidArgument &&
      context.overflowResult == -1 && context.overflowError == Error::InvalidArgument &&
      context.oversizedResult == -1 && context.oversizedError == Error::InvalidArgument &&
      context.pipeReadResult == -1 && context.pipeReadError == Error::IllegalSeek &&
      context.pipeWriteResult == -1 && context.pipeWriteError == Error::IllegalSeek &&
      context.wrongReadResult == -1 && context.wrongReadError == Error::BadFileDescriptor &&
      context.wrongWriteResult == -1 && context.wrongWriteError == Error::BadFileDescriptor &&
      context.badAddressResult == -1 && context.badAddressError == Error::BadAddress;

  policyWriter.reset();
  policyReader.reset();
  faultWriter.reset();
  faultReader.reset();
  for (size_t fd = PolicyWriteDescriptor; fd <= PipeWriteDescriptor; ++fd) {
    passed = closeDescriptor(subsystem, fd) && passed;
  }
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL positional-io-semantics: "
        "offset isolation, append override, usercopy, partial progress, or validation regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS positional-io-semantics");
  return true;
}
}  // namespace

bool runHostedPositionalIoRegressions(Process* process) {
  return positionalIoSemantics(process);
}

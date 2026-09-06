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
#include <stdint.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Pipe.h"

namespace {
constexpr size_t BounceCapacity = PIPE_BUF_MAX + 1;
constexpr int PreservedErrno = 173;
constexpr int LinuxRwfNoAppend = 0x20;

enum class FaultMode { None, WriteAfterFirst, ReadBeforeSecondCopy };

bool closeDescriptor(PosixSubsystem* subsystem, size_t fd) {
  DescriptorLease descriptor;
  return subsystem->acquireFileDescriptor(fd, descriptor) &&
         subsystem->closeFileDescriptor(fd, descriptor);
}

bool allocateUserMapping(Process* process, size_t length, uintptr_t& address) {
  address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, length, address)) {
    return false;
  }

  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, length, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping || mappedAddress != address) {
    MemoryMapManager::instance().remove(address, length);
    process->freeUserRange(Process::UserRegion::Normal, address, length);
    address = 0;
    return false;
  }
  return true;
}

class PositionalVectorProbeFile final : public File {
 public:
  explicit PositionalVectorProbeFile(FaultMode faultMode, size_t initialSize = 0)
      : File(String("positional-vector-probe"), 0, 0, 0, 1, nullptr, initialSize, nullptr),
        m_FaultMode(faultMode),
        m_UserBase(0),
        m_MappingLength(0),
        m_PageSize(0),
        m_ReadCalls(0),
        m_WriteCalls(0),
        m_SawRawPointer(false),
        m_ReadOffsets{0, 0, 0, 0},
        m_WriteOffsets{0, 0, 0, 0} {}

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
};

struct PositionalVectorContext {
  PositionalVectorContext(Process* process, PositionalVectorProbeFile* policyWrite,
                          PositionalVectorProbeFile* policyRead,
                          PositionalVectorProbeFile* faultWrite,
                          PositionalVectorProbeFile* faultRead)
      : process(process),
        policyWrite(policyWrite),
        policyRead(policyRead),
        faultWrite(faultWrite),
        faultRead(faultRead),
        results{},
        errors{},
        readValues{},
        setup(false),
        returned(0) {}

  Process* process;
  PositionalVectorProbeFile* policyWrite;
  PositionalVectorProbeFile* policyRead;
  PositionalVectorProbeFile* faultWrite;
  PositionalVectorProbeFile* faultRead;
  ssize_t results[9];
  int errors[9];
  char readValues[4];
  bool setup;
  Atomic<size_t> returned;
};

int positionalVectorWorker(void* parameter) {
  constexpr int PolicyWriteDescriptor = 96;
  constexpr int PolicyReadDescriptor = 97;
  constexpr int FaultWriteDescriptor = 98;
  constexpr int FaultReadDescriptor = 99;

  PositionalVectorContext* context = reinterpret_cast<PositionalVectorContext*>(parameter);
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
  struct iovec smallVectors[2] = {
      {userBuffer, 3},
      {userBuffer + 32, 2},
  };

  ByteSet(userBuffer, 'w', mappingLength);
  thread->setErrno(PreservedErrno);
  context->results[0] =
      posix_pwritev(PolicyWriteDescriptor, smallVectors, 2, static_cast<off_t>(7));
  context->errors[0] = thread->getErrno();

  thread->setErrno(PreservedErrno);
  context->results[1] =
      posix_pwritev2(PolicyWriteDescriptor, smallVectors, 2, static_cast<off_t>(9), 0);
  context->errors[1] = thread->getErrno();

  thread->setErrno(PreservedErrno);
  context->results[2] = posix_pwritev2(PolicyWriteDescriptor, smallVectors, 2,
                                       static_cast<off_t>(11), LinuxRwfNoAppend);
  context->errors[2] = thread->getErrno();

  thread->setErrno(PreservedErrno);
  context->results[3] =
      posix_pwritev2(PolicyWriteDescriptor, smallVectors, 2, -1, LinuxRwfNoAppend);
  context->errors[3] = thread->getErrno();

  ByteSet(userBuffer, 0, mappingLength);
  thread->setErrno(PreservedErrno);
  context->results[4] = posix_preadv(PolicyReadDescriptor, smallVectors, 2, static_cast<off_t>(19));
  context->errors[4] = thread->getErrno();
  context->readValues[0] = userBuffer[0];
  context->readValues[1] = userBuffer[32];

  thread->setErrno(PreservedErrno);
  context->results[5] = posix_preadv2(PolicyReadDescriptor, smallVectors, 2, -1, 0);
  context->errors[5] = thread->getErrno();
  context->readValues[2] = userBuffer[0];
  context->readValues[3] = userBuffer[32];

  struct iovec faultWriteVectors[2] = {
      {userBuffer, BounceCapacity},
      {userBuffer + pageSize * 2, BounceCapacity},
  };
  ByteSet(userBuffer, 'f', mappingLength);
  thread->setErrno(PreservedErrno);
  context->results[6] =
      posix_pwritev(FaultWriteDescriptor, faultWriteVectors, 2, static_cast<off_t>(31));
  context->errors[6] = thread->getErrno();
  MemoryMapManager::instance().setPermissions(address, mappingLength,
                                              MemoryMappedObject::Read | MemoryMappedObject::Write);

  struct iovec faultReadVector = {userBuffer, BounceCapacity * 2};
  ByteSet(userBuffer, 0, mappingLength);
  thread->setErrno(PreservedErrno);
  context->results[7] =
      posix_preadv(FaultReadDescriptor, &faultReadVector, 1, static_cast<off_t>(41));
  context->errors[7] = thread->getErrno();
  MemoryMapManager::instance().setPermissions(address, mappingLength,
                                              MemoryMappedObject::Read | MemoryMappedObject::Write);

  thread->setErrno(0);
  context->results[8] =
      posix_pwritev2(PolicyWriteDescriptor, smallVectors, 2, 0, LinuxRwfNoAppend << 1);
  context->errors[8] = thread->getErrno();

  MemoryMapManager::instance().remove(address, mappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, address, mappingLength);
  context->setup = true;
  context->returned += 1;
  return 0;
}

bool positionalVectorSemantics(Process* kernelProcess) {
  constexpr size_t PolicyWriteDescriptor = 96;
  constexpr size_t PolicyReadDescriptor = 97;
  constexpr size_t FaultWriteDescriptor = 98;
  constexpr size_t FaultReadDescriptor = 99;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  PositionalVectorProbeFile policyWrite(FaultMode::None, 100);
  PositionalVectorProbeFile policyRead(FaultMode::None);
  PositionalVectorProbeFile faultWrite(FaultMode::WriteAfterFirst);
  PositionalVectorProbeFile faultRead(FaultMode::ReadBeforeSecondCopy);
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

  PositionalVectorContext context(process, &policyWrite, &policyRead, &faultWrite, &faultRead);
  Thread* worker =
      new Thread(process, positionalVectorWorker, &context, nullptr, false, true, true);
  worker->setName("hosted positional vector I/O semantics");
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

  bool passed = started && joined && context.returned == 1 && context.setup && acquired;
  for (size_t i = 0; i < 8; ++i) {
    passed = context.results[i] == (i == 6 || i == 7 ? static_cast<ssize_t>(BounceCapacity)
                                                     : static_cast<ssize_t>(5)) &&
             context.errors[i] == PreservedErrno && passed;
  }
  passed = context.results[8] == -1 && context.errors[8] == Error::OperationNotSupported &&
           policyWriter->getOffset() == 128 && policyReader->getOffset() == 152 &&
           faultWriter->getOffset() == 173 && faultReader->getOffset() == 197 &&
           policyWrite.writeCalls() == 4 && policyWrite.writeOffset(0) == 7 &&
           policyWrite.writeOffset(1) == 100 && policyWrite.writeOffset(2) == 11 &&
           policyWrite.writeOffset(3) == 123 && policyRead.readCalls() == 4 &&
           policyRead.readOffset(0) == 19 && policyRead.readOffset(1) == 22 &&
           policyRead.readOffset(2) == 147 && policyRead.readOffset(3) == 150 &&
           context.readValues[0] == 'a' && context.readValues[1] == 'b' &&
           context.readValues[2] == 'c' && context.readValues[3] == 'd' &&
           faultWrite.writeCalls() == 1 && faultWrite.writeOffset(0) == 31 &&
           faultRead.readCalls() == 2 && faultRead.readOffset(0) == 41 &&
           faultRead.readOffset(1) == 41 + BounceCapacity && !policyWrite.sawRawPointer() &&
           !policyRead.sawRawPointer() && !faultWrite.sawRawPointer() &&
           !faultRead.sawRawPointer() && passed;

  policyWriter.reset();
  policyReader.reset();
  faultWriter.reset();
  faultReader.reset();
  for (size_t fd = PolicyWriteDescriptor; fd <= FaultReadDescriptor; ++fd) {
    passed = closeDescriptor(subsystem, fd) && passed;
  }
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL positional-vector-io-semantics: offset isolation, append "
        "policy, partial progress, or usercopy regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS positional-vector-io-semantics");
  return true;
}
}  // namespace

bool runHostedPositionalVectorIoRegressions(Process* process) {
  return positionalVectorSemantics(process);
}

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
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include <fcntl.h>
#include <signal.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Pipe.h"

namespace {
constexpr size_t BounceCapacity = PIPE_BUF_MAX + 1;
constexpr size_t ChunkedWriteLength = BounceCapacity * 2 + 17;
constexpr int PreservedErrno = 123;

bool closeDescriptor(PosixSubsystem* subsystem, size_t fd) {
  DescriptorLease descriptor;
  return subsystem->acquireFileDescriptor(fd, descriptor) &&
         subsystem->closeFileDescriptor(fd, descriptor);
}

class ScalarWriteProbeFile final : public File {
 public:
  ScalarWriteProbeFile()
      : File(String("scalar-write-probe"), 0, 0, 0, 1, nullptr, 0, nullptr),
        m_FirstWriteEntered(0, false),
        m_ReleaseFirstWrite(0, false),
        m_UserSource(nullptr),
        m_WriteCount(0),
        m_RawPointer(false),
        m_Offsets{0, 0, 0, 0},
        m_Sizes{0, 0, 0, 0},
        m_FirstValues{0, 0, 0, 0} {}

  void setUserSource(char* source) {
    m_UserSource = source;
  }

  bool waitForFirstWrite() {
    return m_FirstWriteEntered.acquireForCompletion();
  }

  void releaseFirstWrite() {
    m_ReleaseFirstWrite.release();
  }

  size_t writeCount() const {
    return m_WriteCount;
  }

  uint64_t offset(size_t index) const {
    return m_Offsets[index];
  }

  size_t size(size_t index) const {
    return m_Sizes[index];
  }

  char firstValue(size_t index) const {
    return m_FirstValues[index];
  }

  bool sawRawPointer() const {
    return m_RawPointer;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = (m_WriteCount += 1) - 1;
    if (slot < 4) {
      m_Offsets[slot] = location;
      m_Sizes[slot] = size;
    }

    if (!slot) {
      m_RawPointer = buffer == reinterpret_cast<uintptr_t>(m_UserSource);
      ByteSet(m_UserSource, 'z', ChunkedWriteLength);
    }

    if (slot < 4 && size) {
      m_FirstValues[slot] = *reinterpret_cast<const char*>(buffer);
    }

    if (!slot) {
      m_FirstWriteEntered.release();
      if (!m_ReleaseFirstWrite.acquireForCompletion()) {
        return 0;
      }
    }

    if (location + size > getSize()) {
      setSize(location + size);
    }
    return size;
  }

 private:
  Semaphore m_FirstWriteEntered;
  Semaphore m_ReleaseFirstWrite;
  char* m_UserSource;
  Atomic<size_t> m_WriteCount;
  bool m_RawPointer;
  uint64_t m_Offsets[4];
  size_t m_Sizes[4];
  char m_FirstValues[4];
};

class ScalarReplacementFile final : public File {
 public:
  ScalarReplacementFile()
      : File(String("scalar-replacement"), 0, 0, 0, 2, nullptr, 0, nullptr), m_Writes(0) {}

  size_t writes() const {
    return m_Writes;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t writeBytewise(uint64_t, uint64_t size, uintptr_t, bool) override {
    m_Writes += 1;
    return size;
  }

 private:
  Atomic<size_t> m_Writes;
};

struct ScalarWriteContext {
  ScalarWriteContext(size_t descriptor, ScalarWriteProbeFile* file)
      : descriptor(descriptor), file(file), entered(0), result(-2), error(0), returned(0) {}

  size_t descriptor;
  ScalarWriteProbeFile* file;
  Atomic<size_t> entered;
  int result;
  int error;
  Atomic<size_t> returned;
};

int chunkedScalarWrite(void* parameter) {
  ScalarWriteContext* context = reinterpret_cast<ScalarWriteContext*>(parameter);
  char payload[ChunkedWriteLength];
  ByteSet(payload, 'a', sizeof(payload));
  context->file->setUserSource(payload);
  context->entered += 1;

  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result =
      posix_write(static_cast<int>(context->descriptor), payload, sizeof(payload), false);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

struct AliasWriteContext {
  explicit AliasWriteContext(size_t descriptor)
      : descriptor(descriptor), entered(0), result(-2), error(0), returned(0) {}

  size_t descriptor;
  Atomic<size_t> entered;
  int result;
  int error;
  Atomic<size_t> returned;
};

int scalarAliasWrite(void* parameter) {
  AliasWriteContext* context = reinterpret_cast<AliasWriteContext*>(parameter);
  char value = 'q';
  context->entered += 1;
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result = posix_write(static_cast<int>(context->descriptor), &value, 1, false);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool scalarWriteBounceAndLifetime(Process* kernelProcess) {
  constexpr size_t SourceDescriptor = 82;
  constexpr size_t AliasDescriptor = 83;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  ScalarWriteProbeFile sourceFile;
  ScalarReplacementFile replacementFile;
  FileDescriptor* source = new FileDescriptor(&sourceFile, 0, SourceDescriptor, 0, O_WRONLY);
  FileDescriptor* alias = new FileDescriptor(*source);
  alias->fd = AliasDescriptor;
  subsystem->addFileDescriptor(SourceDescriptor, source);
  subsystem->addFileDescriptor(AliasDescriptor, alias);
  FileDescriptor::OpenFileDescriptionLease description = source->acquireOpenFileDescription();

  ScalarWriteContext sourceContext(SourceDescriptor, &sourceFile);
  AliasWriteContext aliasContext(AliasDescriptor);
  Thread* sourceWorker =
      new Thread(process, chunkedScalarWrite, &sourceContext, nullptr, false, true, true);
  Thread* aliasWorker =
      new Thread(process, scalarAliasWrite, &aliasContext, nullptr, false, true, true);
  sourceWorker->setName("hosted scalar bounce writer");
  aliasWorker->setName("hosted scalar bounce alias");

  const bool sourceStarted = sourceWorker->start();
  const bool firstEntered = sourceStarted && sourceFile.waitForFirstWrite();
  const bool aliasStarted = firstEntered && aliasWorker->start();
  while (aliasStarted && !aliasContext.entered) {
    Scheduler::instance().yield();
  }
  for (size_t attempt = 0; attempt < 32 && aliasStarted && !aliasContext.returned; ++attempt) {
    Scheduler::instance().yield();
  }
  const bool aliasWasSerialized = aliasStarted && !aliasContext.returned;

  const bool sourceClosed = firstEntered && closeDescriptor(subsystem, SourceDescriptor);
  subsystem->addFileDescriptor(
      SourceDescriptor, new FileDescriptor(&replacementFile, 0, SourceDescriptor, 0, O_WRONLY));

  sourceFile.releaseFirstWrite();
  const bool sourceJoined = sourceStarted && sourceWorker->joinForCompletion();
  const bool aliasJoined = aliasStarted && aliasWorker->joinForCompletion();
  if (!sourceStarted) {
    delete sourceWorker;
  }
  if (!aliasStarted) {
    delete aliasWorker;
  }

  bool passed = sourceStarted && firstEntered && aliasStarted && aliasWasSerialized &&
                sourceClosed && sourceJoined && aliasJoined && sourceContext.returned == 1 &&
                sourceContext.result == static_cast<int>(ChunkedWriteLength) &&
                sourceContext.error == PreservedErrno && aliasContext.returned == 1 &&
                aliasContext.result == 1 && aliasContext.error == PreservedErrno &&
                !sourceFile.sawRawPointer() && sourceFile.writeCount() == 4 &&
                sourceFile.offset(0) == 0 && sourceFile.size(0) == BounceCapacity &&
                sourceFile.firstValue(0) == 'a' && sourceFile.offset(1) == BounceCapacity &&
                sourceFile.size(1) == BounceCapacity && sourceFile.firstValue(1) == 'z' &&
                sourceFile.offset(2) == BounceCapacity * 2 && sourceFile.size(2) == 17 &&
                sourceFile.firstValue(2) == 'z' && sourceFile.offset(3) == ChunkedWriteLength &&
                sourceFile.size(3) == 1 && sourceFile.firstValue(3) == 'q' &&
                replacementFile.writes() == 0 && alias->getOffset() == ChunkedWriteLength + 1;

  const bool aliasClosed = closeDescriptor(subsystem, AliasDescriptor);
  const bool replacementClosed = closeDescriptor(subsystem, SourceDescriptor);
  passed = passed && aliasClosed && replacementClosed;
  description.reset();
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL scalar-write-bounce-lifetime: "
        "the scalar write exposed a user pointer, lost its descriptor/OFD, or interleaved chunks");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS scalar-write-bounce-lifetime");
  return true;
}

class FaultingScalarFile final : public File {
 public:
  explicit FaultingScalarFile(bool reading)
      : File(String(reading ? "scalar-read-fault" : "scalar-write-fault"), 0, 0, 0, reading ? 3 : 4,
             nullptr, 0, nullptr),
        m_UserBase(0),
        m_MappingLength(0),
        m_PageSize(0),
        m_Calls(0),
        m_RawPointer(false),
        m_FirstValue(0) {}

  void configure(uintptr_t userBase, size_t mappingLength, size_t pageSize) {
    m_UserBase = userBase;
    m_MappingLength = mappingLength;
    m_PageSize = pageSize;
  }

  size_t calls() const {
    return m_Calls;
  }

  bool sawRawPointer() const {
    return m_RawPointer;
  }

  char firstValue() const {
    return m_FirstValue;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = (m_Calls += 1) - 1;
    const bool raw = buffer >= m_UserBase && buffer < m_UserBase + m_MappingLength;
    m_RawPointer = m_RawPointer || raw;

    if (slot == 1) {
      MemoryMapManager::instance().setPermissions(
          m_UserBase + m_PageSize, m_MappingLength - m_PageSize, MemoryMappedObject::Read);
    }
    if (!raw) {
      ByteSet(reinterpret_cast<void*>(buffer), 'r', size);
    }
    return size;
  }

  uint64_t writeBytewise(uint64_t, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = (m_Calls += 1) - 1;
    const bool raw = buffer >= m_UserBase && buffer < m_UserBase + m_MappingLength;
    m_RawPointer = m_RawPointer || raw;
    if (size) {
      m_FirstValue = *reinterpret_cast<const char*>(buffer);
    }

    if (!slot) {
      MemoryMapManager::instance().setPermissions(
          m_UserBase + m_PageSize, m_MappingLength - m_PageSize, MemoryMappedObject::None);
    }
    return size;
  }

 private:
  uintptr_t m_UserBase;
  size_t m_MappingLength;
  size_t m_PageSize;
  Atomic<size_t> m_Calls;
  bool m_RawPointer;
  char m_FirstValue;
};

struct ScalarFaultContext {
  ScalarFaultContext(Process* process, size_t writeFd, size_t readFd, FaultingScalarFile* writeFile,
                     FaultingScalarFile* readFile)
      : process(process),
        writeFd(writeFd),
        readFd(readFd),
        writeFile(writeFile),
        readFile(readFile),
        setup(false),
        writeResult(-2),
        writeError(0),
        firstWriteFaultResult(-2),
        firstWriteFaultError(0),
        readResult(-2),
        readError(0),
        firstReadFaultResult(-2),
        firstReadFaultError(0),
        returned(0) {}

  Process* process;
  size_t writeFd;
  size_t readFd;
  FaultingScalarFile* writeFile;
  FaultingScalarFile* readFile;
  bool setup;
  int writeResult;
  int writeError;
  int firstWriteFaultResult;
  int firstWriteFaultError;
  int readResult;
  int readError;
  int firstReadFaultResult;
  int firstReadFaultError;
  Atomic<size_t> returned;
};

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

int scalarFaultWorker(void* parameter) {
  ScalarFaultContext* context = reinterpret_cast<ScalarFaultContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = pageSize * 3;
  const size_t transferLength = BounceCapacity * 2;

  uintptr_t writeAddress = 0;
  if (!allocateUserMapping(context->process, mappingLength, writeAddress)) {
    context->returned += 1;
    return 1;
  }
  ByteSet(reinterpret_cast<void*>(writeAddress), 'w', mappingLength);
  context->writeFile->configure(writeAddress, mappingLength, pageSize);
  thread->setErrno(PreservedErrno);
  context->writeResult = posix_write(static_cast<int>(context->writeFd),
                                     reinterpret_cast<char*>(writeAddress), transferLength, false);
  context->writeError = thread->getErrno();

  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  thread->setErrno(0);
  context->firstWriteFaultResult = posix_write(static_cast<int>(context->writeFd),
                                               reinterpret_cast<char*>(kernelStart), 1, false);
  context->firstWriteFaultError = thread->getErrno();
  MemoryMapManager::instance().remove(writeAddress, mappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, writeAddress, mappingLength);

  uintptr_t readAddress = 0;
  if (!allocateUserMapping(context->process, mappingLength, readAddress)) {
    context->returned += 1;
    return 1;
  }
  ByteSet(reinterpret_cast<void*>(readAddress), 0, mappingLength);
  context->readFile->configure(readAddress, mappingLength, pageSize);
  thread->setErrno(PreservedErrno);
  context->readResult = posix_read(static_cast<int>(context->readFd),
                                   reinterpret_cast<char*>(readAddress), transferLength);
  context->readError = thread->getErrno();

  thread->setErrno(0);
  context->firstReadFaultResult =
      posix_read(static_cast<int>(context->readFd), reinterpret_cast<char*>(kernelStart), 1);
  context->firstReadFaultError = thread->getErrno();
  MemoryMapManager::instance().remove(readAddress, mappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, readAddress, mappingLength);

  context->setup = true;
  context->returned += 1;
  return 0;
}

bool scalarUsercopyFaultProgress(Process* kernelProcess) {
  constexpr size_t WriteDescriptor = 84;
  constexpr size_t ReadDescriptor = 85;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  FaultingScalarFile writeFile(false);
  FaultingScalarFile readFile(true);
  FileDescriptor* writer = new FileDescriptor(&writeFile, 0, WriteDescriptor, 0, O_WRONLY);
  FileDescriptor* reader = new FileDescriptor(&readFile, 0, ReadDescriptor, 0, O_RDONLY);
  subsystem->addFileDescriptor(WriteDescriptor, writer);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  FileDescriptor::OpenFileDescriptionLease writeDescription = writer->acquireOpenFileDescription();
  FileDescriptor::OpenFileDescriptionLease readDescription = reader->acquireOpenFileDescription();

  ScalarFaultContext context(process, WriteDescriptor, ReadDescriptor, &writeFile, &readFile);
  Thread* worker = new Thread(process, scalarFaultWorker, &context, nullptr, false, true, true);
  worker->setName("hosted scalar usercopy fault progress");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  bool passed = started && joined && context.returned == 1 && context.setup &&
                context.writeResult == static_cast<int>(BounceCapacity) &&
                context.writeError == PreservedErrno && context.firstWriteFaultResult == -1 &&
                context.firstWriteFaultError == Error::BadAddress && writeFile.calls() == 1 &&
                !writeFile.sawRawPointer() && writeFile.firstValue() == 'w' &&
                writer->getOffset() == BounceCapacity &&
                context.readResult == static_cast<int>(BounceCapacity) &&
                context.readError == PreservedErrno && context.firstReadFaultResult == -1 &&
                context.firstReadFaultError == Error::BadAddress && readFile.calls() == 2 &&
                !readFile.sawRawPointer() && reader->getOffset() == BounceCapacity;

  passed = closeDescriptor(subsystem, WriteDescriptor) &&
           closeDescriptor(subsystem, ReadDescriptor) && passed;
  writeDescription.reset();
  readDescription.reset();
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL scalar-usercopy-fault-progress: "
        "first/later EFAULT handling exposed pointers or lost partial offset semantics");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS scalar-usercopy-fault-progress");
  return true;
}

class SnapshotPipe final : public Pipe {
 public:
  SnapshotPipe()
      : Pipe(String(""), 0, 0, 0, 0, nullptr, 0, nullptr, true),
        m_Entered(0, false),
        m_UserSource(nullptr),
        m_Armed(false),
        m_RawPointer(false),
        m_FirstValue(0) {}

  void arm(char* source) {
    m_UserSource = source;
    m_Armed = true;
  }

  bool waitUntilEntered() {
    return m_Entered.acquireForCompletion();
  }

  bool sawRawPointer() const {
    return m_RawPointer;
  }

  char firstValue() const {
    return m_FirstValue;
  }

 protected:
  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                         bool canBlock) override {
    if (m_Armed) {
      m_Armed = false;
      m_RawPointer = buffer == reinterpret_cast<uintptr_t>(m_UserSource);
      ByteSet(m_UserSource, 'z', PIPE_BUF_MAX);
      if (size) {
        m_FirstValue = *reinterpret_cast<const char*>(buffer);
      }
      m_Entered.release();
    }
    return Pipe::writeBytewise(location, size, buffer, canBlock);
  }

 private:
  Semaphore m_Entered;
  char* m_UserSource;
  bool m_Armed;
  bool m_RawPointer;
  char m_FirstValue;
};

struct ScalarPipeContext {
  ScalarPipeContext(size_t writeFd, SnapshotPipe* pipe)
      : writeFd(writeFd),
        pipe(pipe),
        atomicComplete(0, false),
        runLargeWrite(0, false),
        atomicResult(-2),
        largeResult(-2),
        largeError(0),
        returned(0) {}

  size_t writeFd;
  SnapshotPipe* pipe;
  Semaphore atomicComplete;
  Semaphore runLargeWrite;
  int atomicResult;
  int largeResult;
  int largeError;
  Atomic<size_t> returned;
};

int scalarPipeWriter(void* parameter) {
  ScalarPipeContext* context = reinterpret_cast<ScalarPipeContext*>(parameter);
  char atomicPayload[PIPE_BUF_MAX];
  ByteSet(atomicPayload, 'a', sizeof(atomicPayload));
  context->pipe->arm(atomicPayload);
  context->atomicResult =
      posix_write(static_cast<int>(context->writeFd), atomicPayload, sizeof(atomicPayload), false);
  context->atomicComplete.release();

  if (!context->runLargeWrite.acquireForCompletion()) {
    context->returned += 1;
    return 1;
  }

  char largePayload[PIPE_BUF_MAX + 1];
  ByteSet(largePayload, 'l', sizeof(largePayload));
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->largeResult =
      posix_write(static_cast<int>(context->writeFd), largePayload, sizeof(largePayload), false);
  context->largeError = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool scalarPipeSnapshotAndLargePartial(Process* kernelProcess) {
  constexpr size_t ReadDescriptor = 86;
  constexpr size_t WriteDescriptor = 87;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  SnapshotPipe* pipe = new SnapshotPipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  char fill[PIPE_BUF_MAX];
  ByteSet(fill, 'f', sizeof(fill));
  const bool filled =
      writer->write(sizeof(fill), reinterpret_cast<uintptr_t>(fill), true) == sizeof(fill);

  ScalarPipeContext context(WriteDescriptor, pipe);
  Thread* worker = new Thread(process, scalarPipeWriter, &context, nullptr, false, true, true);
  worker->setName("hosted scalar pipe bounce writer");
  const bool started = filled && worker->start();
  const bool entered = started && pipe->waitUntilEntered();

  char drain[PIPE_BUF_MAX];
  const bool initialDrained =
      entered &&
      reader->read(sizeof(drain), reinterpret_cast<uintptr_t>(drain), true) == sizeof(drain);
  const bool atomicCompleted = initialDrained && context.atomicComplete.acquireForCompletion();
  const bool atomicDrained =
      atomicCompleted &&
      reader->read(sizeof(drain), reinterpret_cast<uintptr_t>(drain), true) == sizeof(drain);
  bool atomicContents = atomicDrained;
  for (size_t i = 0; i < sizeof(drain) && atomicContents; ++i) {
    atomicContents = drain[i] == 'a';
  }

  const bool refilled =
      atomicDrained &&
      writer->write(sizeof(fill), reinterpret_cast<uintptr_t>(fill), true) == sizeof(fill);
  char byte = 0;
  const bool oneByteFreed =
      refilled && reader->read(1, reinterpret_cast<uintptr_t>(&byte), true) == 1;
  writer->addStatusFlag(O_NONBLOCK);
  context.runLargeWrite.release();

  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool finalDrained =
      joined &&
      reader->read(sizeof(drain), reinterpret_cast<uintptr_t>(drain), true) == sizeof(drain);

  bool passed = started && entered && initialDrained && atomicCompleted && atomicDrained &&
                atomicContents && refilled && oneByteFreed && joined && finalDrained &&
                context.returned == 1 && context.atomicResult == PIPE_BUF_MAX &&
                !pipe->sawRawPointer() && pipe->firstValue() == 'a' && context.largeResult == 1 &&
                context.largeError == PreservedErrno && drain[PIPE_BUF_MAX - 1] == 'l';

  passed = closeDescriptor(subsystem, WriteDescriptor) &&
           closeDescriptor(subsystem, ReadDescriptor) && passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL scalar-pipe-bounce: "
        "PIPE_BUF snapshot atomicity or the 4097-byte nonblocking partial write regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS scalar-pipe-bounce");
  return true;
}

Atomic<size_t> g_ScalarSigpipeHandlerCalls(0);
File* g_ScalarSigpipeTarget = nullptr;

void scalarSigpipeHandler(size_t) {
  File::WriteGuard guard = g_ScalarSigpipeTarget->lockWrites();
  g_ScalarSigpipeHandlerCalls += 1;
}

struct ScalarSigpipeContext {
  explicit ScalarSigpipeContext(size_t descriptor)
      : descriptor(descriptor), result(-2), error(0), returned(0) {}

  size_t descriptor;
  int result;
  int error;
  Atomic<size_t> returned;
};

int scalarSigpipeWriter(void* parameter) {
  ScalarSigpipeContext* context = reinterpret_cast<ScalarSigpipeContext*>(parameter);
  char value = 's';
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result =
      posix_write(static_cast<int>(context->descriptor), &value, sizeof(value), false);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool scalarSigpipeAfterWriteGuard(Process* kernelProcess) {
  constexpr size_t WriteDescriptor = 88;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  subsystem->addFileDescriptor(WriteDescriptor,
                               new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY));

  PosixSubsystem::SignalHandler* handler = new PosixSubsystem::SignalHandler;
  handler->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(&scalarSigpipeHandler), SIGPIPE);
  subsystem->setSignalHandler(SIGPIPE, handler);

  g_ScalarSigpipeHandlerCalls = 0;
  g_ScalarSigpipeTarget = pipe;
  ScalarSigpipeContext context(WriteDescriptor);
  Thread* worker = new Thread(process, scalarSigpipeWriter, &context, nullptr, false, true, true);
  worker->setName("hosted scalar SIGPIPE guard release");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  g_ScalarSigpipeTarget = nullptr;

  bool passed = started && joined && context.returned == 1 && context.result == -1 &&
                context.error == Error::BrokenPipe && g_ScalarSigpipeHandlerCalls == 1;
  passed = closeDescriptor(subsystem, WriteDescriptor) && passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL scalar-sigpipe-after-write-guard: "
        "SIGPIPE ran before scalar write serialization was released");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS scalar-sigpipe-after-write-guard");
  return true;
}
}  // namespace

bool runHostedScalarIoRegressions(Process* process) {
  return scalarWriteBounceAndLifetime(process) && scalarUsercopyFaultProgress(process) &&
         scalarPipeSnapshotAndLargePartial(process) && scalarSigpipeAfterWriteGuard(process);
}

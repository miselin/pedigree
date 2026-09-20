/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/machine/Disk.h"
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
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Pipe.h"

namespace {
constexpr size_t BounceCapacity = PIPE_BUF_MAX + 1;
constexpr size_t FirstVectorLength = BounceCapacity + 5;
constexpr size_t SecondVectorLength = BounceCapacity - 3;
constexpr size_t VectorWriteLength = FirstVectorLength + SecondVectorLength;
constexpr int PreservedErrno = 123;

bool closeDescriptor(PosixSubsystem* subsystem, size_t descriptor) {
  DescriptorLease lease;
  return subsystem->acquireFileDescriptor(descriptor, lease) &&
         subsystem->closeFileDescriptor(descriptor, lease);
}

bool pointsInto(uintptr_t pointer, const void* base, size_t length) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  return length && pointer >= start && pointer < start + length;
}

class VectorWriteProbeFile final : public File {
 public:
  VectorWriteProbeFile()
      : File(String("vector-write-bounce"), 0, 0, 0, 1, nullptr, 0, nullptr),
        m_FirstWriteEntered(0, false),
        m_ReleaseFirstWrite(0, false),
        m_FirstSource(nullptr),
        m_SecondSource(nullptr),
        m_WriteCount(0),
        m_RawPointer(false),
        m_SecondChunkBeforeBoundary(0),
        m_SecondChunkAfterBoundary(0),
        m_Offsets{0, 0, 0, 0},
        m_Sizes{0, 0, 0, 0},
        m_FirstValues{0, 0, 0, 0} {}

  void setSources(char* first, char* second) {
    m_FirstSource = first;
    m_SecondSource = second;
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

  char secondChunkBeforeBoundary() const {
    return m_SecondChunkBeforeBoundary;
  }

  char secondChunkAfterBoundary() const {
    return m_SecondChunkAfterBoundary;
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
      m_FirstValues[slot] = size ? *reinterpret_cast<const char*>(buffer) : 0;
    }
    m_RawPointer = m_RawPointer || pointsInto(buffer, m_FirstSource, FirstVectorLength) ||
                   pointsInto(buffer, m_SecondSource, SecondVectorLength);
    if (slot == 1 && size > 5) {
      m_SecondChunkBeforeBoundary = reinterpret_cast<const char*>(buffer)[4];
      m_SecondChunkAfterBoundary = reinterpret_cast<const char*>(buffer)[5];
    }

    if (!slot) {
      // Only the already-copied first chunk may retain its old value.
      ByteSet(m_FirstSource, 'z', FirstVectorLength);
      ByteSet(m_SecondSource, 'y', SecondVectorLength);
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
  char* m_FirstSource;
  char* m_SecondSource;
  Atomic<size_t> m_WriteCount;
  bool m_RawPointer;
  char m_SecondChunkBeforeBoundary;
  char m_SecondChunkAfterBoundary;
  uint64_t m_Offsets[4];
  size_t m_Sizes[4];
  char m_FirstValues[4];
};

class VectorReplacementFile final : public File {
 public:
  VectorReplacementFile()
      : File(String("vector-write-replacement"), 0, 0, 0, 2, nullptr, 0, nullptr), m_Writes(0) {}

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

struct VectorWriteContext {
  VectorWriteContext(size_t descriptor, VectorWriteProbeFile* file)
      : descriptor(descriptor), file(file), entered(0), result(-2), error(0), returned(0) {}

  size_t descriptor;
  VectorWriteProbeFile* file;
  Atomic<size_t> entered;
  int result;
  int error;
  Atomic<size_t> returned;
};

int chunkedVectorWrite(void* parameter) {
  VectorWriteContext* context = reinterpret_cast<VectorWriteContext*>(parameter);
  char first[FirstVectorLength];
  char second[SecondVectorLength];
  ByteSet(first, 'a', sizeof(first));
  ByteSet(second, 'b', sizeof(second));
  context->file->setSources(first, second);
  context->entered += 1;

  struct iovec vectors[2] = {{first, sizeof(first)}, {second, sizeof(second)}};
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result = posix_writev(static_cast<int>(context->descriptor), vectors, 2);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

int vectorAliasWrite(void* parameter) {
  VectorWriteContext* context = reinterpret_cast<VectorWriteContext*>(parameter);
  char value = 'q';
  context->entered += 1;
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result = posix_write(static_cast<int>(context->descriptor), &value, 1, false);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool vectorWriteBounceAndLifetime(Process* kernelProcess) {
  constexpr size_t SourceDescriptor = 90;
  constexpr size_t AliasDescriptor = 91;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  VectorWriteProbeFile sourceFile;
  VectorReplacementFile replacementFile;
  FileDescriptor* source = new FileDescriptor(&sourceFile, 0, SourceDescriptor, 0, O_WRONLY);
  FileDescriptor* alias = new FileDescriptor(*source);
  alias->fd = AliasDescriptor;
  subsystem->addFileDescriptor(SourceDescriptor, source);
  subsystem->addFileDescriptor(AliasDescriptor, alias);
  FileDescriptor::OpenFileDescriptionLease description = source->acquireOpenFileDescription();

  VectorWriteContext sourceContext(SourceDescriptor, &sourceFile);
  VectorWriteContext aliasContext(AliasDescriptor, &sourceFile);
  Thread* sourceWorker =
      new Thread(process, chunkedVectorWrite, &sourceContext, nullptr, false, true, true);
  Thread* aliasWorker =
      new Thread(process, vectorAliasWrite, &aliasContext, nullptr, false, true, true);
  sourceWorker->setName("hosted vector bounce writer");
  aliasWorker->setName("hosted vector bounce alias");

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
                sourceContext.result == static_cast<int>(VectorWriteLength) &&
                sourceContext.error == PreservedErrno && aliasContext.returned == 1 &&
                aliasContext.result == 1 && aliasContext.error == PreservedErrno &&
                !sourceFile.sawRawPointer() && sourceFile.writeCount() == 4 &&
                sourceFile.offset(0) == 0 && sourceFile.size(0) == BounceCapacity &&
                sourceFile.firstValue(0) == 'a' && sourceFile.offset(1) == BounceCapacity &&
                sourceFile.size(1) == BounceCapacity && sourceFile.firstValue(1) == 'z' &&
                sourceFile.secondChunkBeforeBoundary() == 'z' &&
                sourceFile.secondChunkAfterBoundary() == 'y' &&
                sourceFile.offset(2) == BounceCapacity * 2 && sourceFile.size(2) == 2 &&
                sourceFile.firstValue(2) == 'y' && sourceFile.offset(3) == VectorWriteLength &&
                sourceFile.size(3) == 1 && sourceFile.firstValue(3) == 'q' &&
                replacementFile.writes() == 0 && alias->getOffset() == VectorWriteLength + 1;

  passed = closeDescriptor(subsystem, AliasDescriptor) &&
           closeDescriptor(subsystem, SourceDescriptor) && passed;
  description.reset();
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vector-write-bounce-lifetime: "
        "writev exposed a user pointer, interleaved chunks, or switched descriptor generations");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vector-write-bounce-lifetime");
  return true;
}

char vectorPattern(size_t offset) {
  return static_cast<char>('A' + (offset % 23));
}

class VectorReadProbeFile final : public File {
 public:
  VectorReadProbeFile()
      : File(String("vector-read-bounce"), 0, 0, 0, 3, nullptr, 0, nullptr),
        m_FirstDestination(nullptr),
        m_FirstLength(0),
        m_SecondDestination(nullptr),
        m_SecondLength(0),
        m_ReadCount(0),
        m_RawPointer(false),
        m_Offsets{0, 0, 0},
        m_Sizes{0, 0, 0} {}

  void setDestinations(void* first, size_t firstLength, void* second, size_t secondLength) {
    m_FirstDestination = first;
    m_FirstLength = firstLength;
    m_SecondDestination = second;
    m_SecondLength = secondLength;
  }

  size_t readCount() const {
    return m_ReadCount;
  }

  bool sawRawPointer() const {
    return m_RawPointer;
  }

  uint64_t offset(size_t index) const {
    return m_Offsets[index];
  }

  size_t size(size_t index) const {
    return m_Sizes[index];
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = (m_ReadCount += 1) - 1;
    if (slot < 3) {
      m_Offsets[slot] = location;
      m_Sizes[slot] = size;
    }
    m_RawPointer = m_RawPointer || pointsInto(buffer, m_FirstDestination, m_FirstLength) ||
                   pointsInto(buffer, m_SecondDestination, m_SecondLength);
    for (size_t i = 0; i < size; ++i) {
      reinterpret_cast<char*>(buffer)[i] = vectorPattern(location + i);
    }
    return size;
  }

 private:
  void* m_FirstDestination;
  size_t m_FirstLength;
  void* m_SecondDestination;
  size_t m_SecondLength;
  Atomic<size_t> m_ReadCount;
  bool m_RawPointer;
  uint64_t m_Offsets[3];
  size_t m_Sizes[3];
};

struct VectorReadContext {
  VectorReadContext(size_t descriptor, VectorReadProbeFile* file)
      : descriptor(descriptor),
        file(file),
        contentsValid(false),
        result(-2),
        error(0),
        returned(0) {}

  size_t descriptor;
  VectorReadProbeFile* file;
  bool contentsValid;
  int result;
  int error;
  Atomic<size_t> returned;
};

int chunkedVectorRead(void* parameter) {
  constexpr size_t FirstLength = BounceCapacity + 7;
  constexpr size_t SecondLength = 13;
  VectorReadContext* context = reinterpret_cast<VectorReadContext*>(parameter);
  char first[FirstLength];
  char second[SecondLength];
  ByteSet(first, 0, sizeof(first));
  ByteSet(second, 0, sizeof(second));
  context->file->setDestinations(first, sizeof(first), second, sizeof(second));

  struct iovec vectors[2] = {{first, sizeof(first)}, {second, sizeof(second)}};
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result = posix_readv(static_cast<int>(context->descriptor), vectors, 2);
  context->error = thread->getErrno();

  bool valid = true;
  for (size_t i = 0; i < sizeof(first) && valid; ++i) {
    valid = first[i] == vectorPattern(i);
  }
  for (size_t i = 0; i < sizeof(second) && valid; ++i) {
    valid = second[i] == vectorPattern(sizeof(first) + i);
  }
  context->contentsValid = valid;
  context->returned += 1;
  return 0;
}

bool vectorReadBounceAndScatter(Process* kernelProcess) {
  constexpr size_t Descriptor = 92;
  constexpr size_t FirstLength = BounceCapacity + 7;
  constexpr size_t TotalLength = FirstLength + 13;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  VectorReadProbeFile file;
  FileDescriptor* descriptor = new FileDescriptor(&file, 0, Descriptor, 0, O_RDONLY);
  subsystem->addFileDescriptor(Descriptor, descriptor);
  FileDescriptor::OpenFileDescriptionLease description = descriptor->acquireOpenFileDescription();

  VectorReadContext context(Descriptor, &file);
  Thread* worker = new Thread(process, chunkedVectorRead, &context, nullptr, false, true, true);
  worker->setName("hosted vector bounce reader");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  bool passed = started && joined && context.returned == 1 &&
                context.result == static_cast<int>(TotalLength) &&
                context.error == PreservedErrno && context.contentsValid && !file.sawRawPointer() &&
                file.readCount() == 3 && file.offset(0) == 0 && file.size(0) == BounceCapacity &&
                file.offset(1) == BounceCapacity && file.size(1) == 7 &&
                file.offset(2) == FirstLength && file.size(2) == 13 &&
                descriptor->getOffset() == TotalLength;
  passed = closeDescriptor(subsystem, Descriptor) && passed;
  description.reset();
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vector-read-bounce-scatter: "
        "readv exposed a user pointer or scattered bounce chunks incorrectly");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vector-read-bounce-scatter");
  return true;
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

class VectorReadDisk final : public Disk {
 public:
  bool pin(uint64_t) override {
    return false;
  }
  void unpin(uint64_t) override {}
};

class VectorReadFilesystem final : public Filesystem {
 public:
  bool initialise(Disk* disk) override {
    m_pDisk = disk;
    return true;
  }
  File* getRoot() const override {
    return nullptr;
  }
  const String& getVolumeLabel() const override {
    return m_Label;
  }

 protected:
  bool createFile(File*, const String&, uint32_t) override {
    return false;
  }
  bool createDirectory(File*, const String&, uint32_t) override {
    return false;
  }
  bool createSymlink(File*, const String&, const String&) override {
    return false;
  }
  bool removeNode(File*, const String&, File*) override {
    return false;
  }

 private:
  String m_Label;
};

constexpr size_t DiskReadCapacity = 64 * 1024;
enum class DiskVectorReadMode { Complete, Short, PrefixFault, CopyFault };

class DiskVectorReadFile final : public File {
 public:
  explicit DiskVectorReadFile(Filesystem* filesystem)
      : File(String("disk-vector-read"), 0, 0, 0, 3, filesystem, DiskReadCapacity * 3, nullptr),
        m_Data(UniqueArray<char>::allocate(DiskReadCapacity * 3)) {
    for (size_t i = 0; i < DiskReadCapacity * 3; ++i) {
      m_Data.get()[i] = vectorPattern(i);
    }
  }

  size_t readCount() const {
    return m_ReadCount;
  }

  void denyWritesAfterFirstRead(uintptr_t address, size_t length) {
    m_DenyAddress = address;
    m_DenyLength = length;
  }

 protected:
  Mutex& dataMutationLock() override {
    // Each File::read owns this guard once, including reads served by its cache.
    if (!m_ReadCount++ && m_DenyLength) {
      MemoryMapManager::instance().setPermissions(m_DenyAddress, m_DenyLength,
                                                  MemoryMappedObject::Read);
    }
    return File::dataMutationLock();
  }

  uintptr_t readBlock(uint64_t location) override {
    return reinterpret_cast<uintptr_t>(m_Data.get() + location);
  }

  bool pinBlock(uint64_t) override {
    return true;
  }

 private:
  UniqueArray<char> m_Data;
  size_t m_ReadCount = 0;
  uintptr_t m_DenyAddress = 0;
  size_t m_DenyLength = 0;
};

struct DiskVectorReadContext {
  Process* process;
  DiskVectorReadFile* file;
  DiskVectorReadMode mode;
  bool positional;
  ssize_t result = -2;
  int error = 0;
  bool valid = false;
};

int diskVectorReader(void* parameter) {
  auto& context = *static_cast<DiskVectorReadContext*>(parameter);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = DiskReadCapacity * 2 + pageSize * 3;
  uintptr_t address = 0;
  if (!allocateUserMapping(context.process, mappingLength, address)) {
    return 1;
  }
  auto* first = reinterpret_cast<char*>(address);
  const bool prefixFault = context.mode == DiskVectorReadMode::PrefixFault;
  const size_t firstLength = prefixFault ? 13 : DiskReadCapacity + 7;
  const size_t secondLength = prefixFault ? DiskReadCapacity * 2 : 13;
  auto* second = first + (prefixFault ? pageSize : DiskReadCapacity + pageSize);
  ByteSet(first, 0, mappingLength);
  if (context.mode == DiskVectorReadMode::Short) {
    context.file->setSize((context.positional ? 29 : 17) + 23);
  } else if (prefixFault) {
    // The vector snapshot passes; the next large precheck must still deliver
    // the prefix that the previous 4097-byte chunks could copy.
    const uintptr_t denied = reinterpret_cast<uintptr_t>(second) + pageSize * 2;
    context.file->denyWritesAfterFirstRead(denied, address + mappingLength - denied);
  } else if (context.mode == DiskVectorReadMode::CopyFault) {
    const uintptr_t denied = address + pageSize * 2;
    context.file->denyWritesAfterFirstRead(denied, address + mappingLength - denied);
  }

  struct iovec vectors[2] = {{first, firstLength}, {second, secondLength}};
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context.result =
      context.positional ? posix_preadv(92, vectors, 2, 29) : posix_readv(92, vectors, 2);
  context.error = thread->getErrno();

  const size_t firstCopied = context.mode == DiskVectorReadMode::CopyFault ? 0
                             : context.mode == DiskVectorReadMode::Short   ? 23
                                                                           : firstLength;
  const size_t secondCopied = prefixFault                                    ? BounceCapacity
                              : context.mode == DiskVectorReadMode::Complete ? secondLength
                                                                             : 0;
  const size_t start = context.positional ? 29 : 17;
  bool valid = true;
  for (size_t i = 0; i < firstLength && valid; ++i) {
    valid = first[i] == (i < firstCopied ? vectorPattern(start + i) : 0);
  }
  for (size_t i = 0; i < secondLength && valid; ++i) {
    valid = second[i] == (i < secondCopied ? vectorPattern(start + firstCopied + i) : 0);
  }
  context.valid = valid;
  MemoryMapManager::instance().remove(address, mappingLength);
  context.process->freeUserRange(Process::UserRegion::Normal, address, mappingLength);
  return 0;
}

bool diskVectorReadCase(Process* kernelProcess, DiskVectorReadMode mode, bool positional) {
  Process* process = new Process(kernelProcess);
  auto* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  VectorReadDisk disk;
  VectorReadFilesystem filesystem;
  filesystem.initialise(&disk);
  DiskVectorReadFile file(&filesystem);
  auto* descriptor = new FileDescriptor(&file, 17, 92, 0, O_RDONLY);
  subsystem->addFileDescriptor(92, descriptor);
  auto description = descriptor->acquireOpenFileDescription();

  DiskVectorReadContext context{process, &file, mode, positional};
  Thread* worker = new Thread(process, diskVectorReader, &context, nullptr, false, true, true);
  worker->setName("hosted disk vector reader");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const ssize_t expected = mode == DiskVectorReadMode::CopyFault     ? -1
                           : mode == DiskVectorReadMode::Short       ? 23
                           : mode == DiskVectorReadMode::PrefixFault ? 13 + BounceCapacity
                                                                     : DiskReadCapacity + 20;
  const size_t calls = mode == DiskVectorReadMode::Complete      ? 3
                       : mode == DiskVectorReadMode::PrefixFault ? 2
                                                                 : 1;
  const uint64_t finalOffset = positional || expected < 0 ? 17 : 17 + expected;
  bool passed = started && joined && context.valid && context.result == expected &&
                context.error == (expected < 0 ? Error::BadAddress : PreservedErrno) &&
                file.readCount() == calls && descriptor->getOffset() == finalOffset;
  passed = closeDescriptor(subsystem, 92) && passed;
  description.reset();
  delete process;
  if (!passed) {
    ERROR("HOSTED-SYSCALL-TEST: FAIL disk-vector-read-batching: mode="
          << static_cast<int>(mode) << " positional=" << positional << " result=" << context.result
          << " errno=" << context.error << " calls=" << file.readCount());
  }
  return passed;
}

bool diskVectorReadBatching(Process* kernelProcess) {
  const bool positions[] = {false, true};
  const DiskVectorReadMode modes[] = {DiskVectorReadMode::Complete, DiskVectorReadMode::Short,
                                      DiskVectorReadMode::PrefixFault,
                                      DiskVectorReadMode::CopyFault};
  for (bool positional : positions) {
    for (DiskVectorReadMode mode : modes) {
      if (!diskVectorReadCase(kernelProcess, mode, positional)) {
        return false;
      }
    }
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS disk-vector-read-batching");
  return true;
}

class FaultingVectorFile final : public File {
 public:
  explicit FaultingVectorFile(bool reading)
      : File(String(reading ? "vector-read-fault" : "vector-write-fault"), 0, 0, 0, reading ? 4 : 5,
             nullptr, 0, nullptr),
        m_UserBase(0),
        m_PageSize(0),
        m_Calls(0),
        m_RawPointer(false) {}

  void configure(uintptr_t userBase, size_t pageSize) {
    m_UserBase = userBase;
    m_PageSize = pageSize;
  }

  size_t calls() const {
    return m_Calls;
  }

  bool sawRawPointer() const {
    return m_RawPointer;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = (m_Calls += 1) - 1;
    m_RawPointer =
        m_RawPointer || pointsInto(buffer, reinterpret_cast<void*>(m_UserBase), m_PageSize * 2);
    if (slot == 1) {
      MemoryMapManager::instance().setPermissions(m_UserBase + m_PageSize, m_PageSize,
                                                  MemoryMappedObject::Read);
    }
    ByteSet(reinterpret_cast<void*>(buffer), 'r', size);
    return size;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = (m_Calls += 1) - 1;
    m_RawPointer =
        m_RawPointer || pointsInto(buffer, reinterpret_cast<void*>(m_UserBase), m_PageSize * 2);
    if (!slot) {
      MemoryMapManager::instance().setPermissions(m_UserBase + m_PageSize, m_PageSize,
                                                  MemoryMappedObject::None);
    }
    if (location + size > getSize()) {
      setSize(location + size);
    }
    return size;
  }

 private:
  uintptr_t m_UserBase;
  size_t m_PageSize;
  Atomic<size_t> m_Calls;
  bool m_RawPointer;
};

struct VectorFaultContext {
  VectorFaultContext(Process* process, size_t writeFd, size_t readFd, FaultingVectorFile* writeFile,
                     FaultingVectorFile* readFile)
      : process(process),
        writeFd(writeFd),
        readFd(readFd),
        writeFile(writeFile),
        readFile(readFile),
        setup(false),
        firstReadPageValid(false),
        writeResult(-2),
        writeError(0),
        firstWriteFaultResult(-2),
        firstWriteFaultError(0),
        readResult(-2),
        readError(0),
        firstReadFaultResult(-2),
        firstReadFaultError(0),
        validationSemantics(false),
        returned(0) {}

  Process* process;
  size_t writeFd;
  size_t readFd;
  FaultingVectorFile* writeFile;
  FaultingVectorFile* readFile;
  bool setup;
  bool firstReadPageValid;
  int writeResult;
  int writeError;
  int firstWriteFaultResult;
  int firstWriteFaultError;
  int readResult;
  int readError;
  int firstReadFaultResult;
  int firstReadFaultError;
  bool validationSemantics;
  Atomic<size_t> returned;
};

int vectorFaultWorker(void* parameter) {
  VectorFaultContext* context = reinterpret_cast<VectorFaultContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = pageSize * 2;

  uintptr_t writeAddress = 0;
  if (!allocateUserMapping(context->process, mappingLength, writeAddress)) {
    context->returned += 1;
    return 1;
  }
  ByteSet(reinterpret_cast<void*>(writeAddress), 'w', mappingLength);
  context->writeFile->configure(writeAddress, pageSize);
  struct iovec writeVectors[2] = {{reinterpret_cast<void*>(writeAddress), pageSize},
                                  {reinterpret_cast<void*>(writeAddress + pageSize), pageSize}};
  thread->setErrno(PreservedErrno);
  context->writeResult = posix_writev(static_cast<int>(context->writeFd), writeVectors, 2);
  context->writeError = thread->getErrno();

  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  struct iovec badVector = {reinterpret_cast<void*>(kernelStart), 1};
  thread->setErrno(0);
  context->firstWriteFaultResult = posix_writev(static_cast<int>(context->writeFd), &badVector, 1);
  context->firstWriteFaultError = thread->getErrno();
  MemoryMapManager::instance().remove(writeAddress, mappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, writeAddress, mappingLength);

  const size_t progressMappingLength = pageSize * 3;
  uintptr_t writeProgressAddress = 0;
  if (!allocateUserMapping(context->process, progressMappingLength, writeProgressAddress)) {
    context->returned += 1;
    return 1;
  }
  ByteSet(reinterpret_cast<void*>(writeProgressAddress), 'p', progressMappingLength);
  MemoryMapManager::instance().setPermissions(writeProgressAddress + pageSize * 2, pageSize,
                                              MemoryMappedObject::None);
  context->writeFile->configure(writeProgressAddress, pageSize);
  struct iovec writeProgressVectors[2] = {
      {reinterpret_cast<void*>(writeProgressAddress), BounceCapacity},
      {reinterpret_cast<void*>(writeProgressAddress + pageSize * 2), 1}};
  thread->setErrno(PreservedErrno);
  const int writeProgressResult =
      posix_writev(static_cast<int>(context->writeFd), writeProgressVectors, 2);
  const int writeProgressError = thread->getErrno();

  struct iovec inaccessibleWriteVector = {
      reinterpret_cast<void*>(writeProgressAddress + pageSize * 2), 1};
  thread->setErrno(0);
  const int inaccessibleWriteResult =
      posix_writev(static_cast<int>(context->writeFd), &inaccessibleWriteVector, 1);
  const int inaccessibleWriteError = thread->getErrno();

  struct iovec laterInvalidWriteVectors[2] = {{reinterpret_cast<void*>(writeProgressAddress), 1},
                                              {reinterpret_cast<void*>(kernelStart), 1}};
  thread->setErrno(0);
  const int laterInvalidWriteResult =
      posix_writev(static_cast<int>(context->writeFd), laterInvalidWriteVectors, 2);
  const int laterInvalidWriteError = thread->getErrno();

  const struct iovec* invalidVectorArray = reinterpret_cast<const struct iovec*>(kernelStart);
  thread->setErrno(0);
  const int invalidWriteArrayResult =
      posix_writev(static_cast<int>(context->writeFd), invalidVectorArray, 1);
  const int invalidWriteArrayError = thread->getErrno();

  struct iovec zeroLengthInvalidVector = {reinterpret_cast<void*>(kernelStart), 0};
  thread->setErrno(PreservedErrno);
  const int zeroLengthWriteResult =
      posix_writev(static_cast<int>(context->writeFd), &zeroLengthInvalidVector, 1);
  const int zeroLengthWriteError = thread->getErrno();

  thread->setErrno(0);
  const int badWriteDescriptorResult = posix_writev(-1, invalidVectorArray, 1);
  const int badWriteDescriptorError = thread->getErrno();

  bool validationSemantics =
      writeProgressResult == static_cast<int>(BounceCapacity) &&
      writeProgressError == PreservedErrno && inaccessibleWriteResult == -1 &&
      inaccessibleWriteError == Error::BadAddress && laterInvalidWriteResult == -1 &&
      laterInvalidWriteError == Error::BadAddress && invalidWriteArrayResult == -1 &&
      invalidWriteArrayError == Error::BadAddress && zeroLengthWriteResult == 0 &&
      zeroLengthWriteError == PreservedErrno && badWriteDescriptorResult == -1 &&
      badWriteDescriptorError == Error::BadFileDescriptor;

  MemoryMapManager::instance().remove(writeProgressAddress, progressMappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, writeProgressAddress,
                                  progressMappingLength);

  uintptr_t readAddress = 0;
  if (!allocateUserMapping(context->process, mappingLength, readAddress)) {
    context->returned += 1;
    return 1;
  }
  ByteSet(reinterpret_cast<void*>(readAddress), 0, mappingLength);
  context->readFile->configure(readAddress, pageSize);
  struct iovec readVectors[2] = {{reinterpret_cast<void*>(readAddress), pageSize},
                                 {reinterpret_cast<void*>(readAddress + pageSize), pageSize}};
  thread->setErrno(PreservedErrno);
  context->readResult = posix_readv(static_cast<int>(context->readFd), readVectors, 2);
  context->readError = thread->getErrno();
  bool firstPageValid = true;
  for (size_t i = 0; i < pageSize && firstPageValid; ++i) {
    firstPageValid = reinterpret_cast<char*>(readAddress)[i] == 'r';
  }
  context->firstReadPageValid = firstPageValid;

  thread->setErrno(0);
  context->firstReadFaultResult = posix_readv(static_cast<int>(context->readFd), &badVector, 1);
  context->firstReadFaultError = thread->getErrno();
  MemoryMapManager::instance().remove(readAddress, mappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, readAddress, mappingLength);

  uintptr_t readProgressAddress = 0;
  if (!allocateUserMapping(context->process, progressMappingLength, readProgressAddress)) {
    context->returned += 1;
    return 1;
  }
  ByteSet(reinterpret_cast<void*>(readProgressAddress), 0, progressMappingLength);
  MemoryMapManager::instance().setPermissions(readProgressAddress + pageSize * 2, pageSize,
                                              MemoryMappedObject::Read);
  context->readFile->configure(readProgressAddress, pageSize);
  struct iovec readProgressVectors[2] = {
      {reinterpret_cast<void*>(readProgressAddress), pageSize},
      {reinterpret_cast<void*>(readProgressAddress + pageSize * 2), 1}};
  thread->setErrno(PreservedErrno);
  const int readProgressResult =
      posix_readv(static_cast<int>(context->readFd), readProgressVectors, 2);
  const int readProgressError = thread->getErrno();
  bool readProgressContentsValid = true;
  for (size_t i = 0; i < pageSize && readProgressContentsValid; ++i) {
    readProgressContentsValid = reinterpret_cast<char*>(readProgressAddress)[i] == 'r';
  }

  struct iovec inaccessibleReadVector = {
      reinterpret_cast<void*>(readProgressAddress + pageSize * 2), 1};
  thread->setErrno(0);
  const int inaccessibleReadResult =
      posix_readv(static_cast<int>(context->readFd), &inaccessibleReadVector, 1);
  const int inaccessibleReadError = thread->getErrno();

  struct iovec laterInvalidReadVectors[2] = {{reinterpret_cast<void*>(readProgressAddress), 1},
                                             {reinterpret_cast<void*>(kernelStart), 1}};
  thread->setErrno(0);
  const int laterInvalidReadResult =
      posix_readv(static_cast<int>(context->readFd), laterInvalidReadVectors, 2);
  const int laterInvalidReadError = thread->getErrno();

  thread->setErrno(0);
  const int invalidReadArrayResult =
      posix_readv(static_cast<int>(context->readFd), invalidVectorArray, 1);
  const int invalidReadArrayError = thread->getErrno();

  thread->setErrno(PreservedErrno);
  const int zeroLengthReadResult =
      posix_readv(static_cast<int>(context->readFd), &zeroLengthInvalidVector, 1);
  const int zeroLengthReadError = thread->getErrno();

  thread->setErrno(0);
  const int badReadDescriptorResult = posix_readv(-1, invalidVectorArray, 1);
  const int badReadDescriptorError = thread->getErrno();

  validationSemantics =
      readProgressResult == static_cast<int>(pageSize) && readProgressError == PreservedErrno &&
      readProgressContentsValid && inaccessibleReadResult == -1 &&
      inaccessibleReadError == Error::BadAddress && laterInvalidReadResult == -1 &&
      laterInvalidReadError == Error::BadAddress && invalidReadArrayResult == -1 &&
      invalidReadArrayError == Error::BadAddress && zeroLengthReadResult == 0 &&
      zeroLengthReadError == PreservedErrno && badReadDescriptorResult == -1 &&
      badReadDescriptorError == Error::BadFileDescriptor && validationSemantics;

  MemoryMapManager::instance().remove(readProgressAddress, progressMappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, readProgressAddress,
                                  progressMappingLength);

  context->validationSemantics = validationSemantics;
  context->setup = true;
  context->returned += 1;
  return 0;
}

bool vectorUsercopyFaultProgress(Process* kernelProcess) {
  constexpr size_t WriteDescriptor = 93;
  constexpr size_t ReadDescriptor = 94;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  FaultingVectorFile writeFile(false);
  FaultingVectorFile readFile(true);
  FileDescriptor* writer = new FileDescriptor(&writeFile, 0, WriteDescriptor, 0, O_WRONLY);
  FileDescriptor* reader = new FileDescriptor(&readFile, 0, ReadDescriptor, 0, O_RDONLY);
  subsystem->addFileDescriptor(WriteDescriptor, writer);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  FileDescriptor::OpenFileDescriptionLease writeDescription = writer->acquireOpenFileDescription();
  FileDescriptor::OpenFileDescriptionLease readDescription = reader->acquireOpenFileDescription();

  VectorFaultContext context(process, WriteDescriptor, ReadDescriptor, &writeFile, &readFile);
  Thread* worker = new Thread(process, vectorFaultWorker, &context, nullptr, false, true, true);
  worker->setName("hosted vector usercopy fault progress");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  bool passed = started && joined && context.returned == 1 && context.setup &&
                context.writeResult == static_cast<int>(BounceCapacity) &&
                context.writeError == PreservedErrno && context.firstWriteFaultResult == -1 &&
                context.firstWriteFaultError == Error::BadAddress && writeFile.calls() == 2 &&
                !writeFile.sawRawPointer() && writer->getOffset() == BounceCapacity * 2 &&
                context.readResult == static_cast<int>(pageSize) &&
                context.readError == PreservedErrno && context.firstReadPageValid &&
                context.firstReadFaultResult == -1 &&
                context.firstReadFaultError == Error::BadAddress && readFile.calls() == 3 &&
                !readFile.sawRawPointer() && reader->getOffset() == pageSize * 2 &&
                context.validationSemantics;
  passed = closeDescriptor(subsystem, WriteDescriptor) &&
           closeDescriptor(subsystem, ReadDescriptor) && passed;
  writeDescription.reset();
  readDescription.reset();
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vector-usercopy-fault-progress: "
        "writev/readv validation, partial progress, or offsets regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vector-usercopy-fault-progress");
  return true;
}

struct VectorPipeFaultContext {
  VectorPipeFaultContext(Process* process, size_t readFd, size_t writeFd)
      : process(process),
        readFd(readFd),
        writeFd(writeFd),
        writeResult(-2),
        writeError(0),
        readResult(-2),
        readError(0),
        setup(false),
        returned(0) {}

  Process* process;
  size_t readFd;
  size_t writeFd;
  int writeResult;
  int writeError;
  int readResult;
  int readError;
  bool setup;
  Atomic<size_t> returned;
};

int vectorPipeFaultWorker(void* parameter) {
  VectorPipeFaultContext* context = reinterpret_cast<VectorPipeFaultContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = pageSize * 2;
  uintptr_t address = 0;
  if (!allocateUserMapping(context->process, mappingLength, address)) {
    context->returned += 1;
    return 1;
  }

  ByteSet(reinterpret_cast<void*>(address), 'v', mappingLength);
  MemoryMapManager::instance().setPermissions(address + pageSize, pageSize,
                                              MemoryMappedObject::None);
  struct iovec writeVectors[2] = {{reinterpret_cast<void*>(address), 1},
                                  {reinterpret_cast<void*>(address + pageSize), 1}};
  thread->setErrno(0);
  context->writeResult = posix_writev(static_cast<int>(context->writeFd), writeVectors, 2);
  context->writeError = thread->getErrno();

  struct iovec readVectors[2] = {{reinterpret_cast<void*>(address + 16), 1},
                                 {reinterpret_cast<void*>(address + pageSize), 1}};
  thread->setErrno(0);
  context->readResult = posix_readv(static_cast<int>(context->readFd), readVectors, 2);
  context->readError = thread->getErrno();

  MemoryMapManager::instance().remove(address, mappingLength);
  context->process->freeUserRange(Process::UserRegion::Normal, address, mappingLength);
  context->setup = true;
  context->returned += 1;
  return 0;
}

bool vectorPipePayloadPrevalidation(Process* kernelProcess) {
  constexpr size_t ReadDescriptor = 99;
  constexpr size_t WriteDescriptor = 100;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY | O_NONBLOCK);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY | O_NONBLOCK);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  char contents[2] = {'x', 'y'};
  const bool filled = writer->write(sizeof(contents), reinterpret_cast<uintptr_t>(contents),
                                    true) == sizeof(contents);
  VectorPipeFaultContext context(process, ReadDescriptor, WriteDescriptor);
  Thread* worker = new Thread(process, vectorPipeFaultWorker, &context, nullptr, false, true, true);
  worker->setName("hosted vector pipe payload prevalidation");
  const bool started = filled && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  char drained[4] = {};
  const uint64_t drainResult =
      joined ? reader->read(sizeof(drained), reinterpret_cast<uintptr_t>(drained), false) : 0;
  bool passed = started && joined && context.returned == 1 && context.setup &&
                context.writeResult == -1 && context.writeError == Error::BadAddress &&
                context.readResult == -1 && context.readError == Error::BadAddress &&
                drainResult == sizeof(contents) && drained[0] == 'x' && drained[1] == 'y';
  passed = closeDescriptor(subsystem, WriteDescriptor) &&
           closeDescriptor(subsystem, ReadDescriptor) && passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vector-pipe-payload-prevalidation: "
        "faulting vectors changed pipe contents");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vector-pipe-payload-prevalidation");
  return true;
}

class InterruptingVectorFile final : public File {
 public:
  InterruptingVectorFile()
      : File(String("vector-interruption"), 0, 0, 0, 6, nullptr, 0, nullptr),
        m_MakeProgress(false),
        m_Calls(0) {}

  void setMakeProgress(bool makeProgress) {
    m_MakeProgress = makeProgress;
  }

  size_t calls() const {
    return m_Calls;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t, bool) override {
    m_Calls += 1;
    Processor::information().getCurrentThread()->setInterruptionReason(Thread::InterruptedBySignal);
    if (!m_MakeProgress) {
      return 0;
    }
    const size_t amount = size < 3 ? size : 3;
    if (location + amount > getSize()) {
      setSize(location + amount);
    }
    return amount;
  }

 private:
  bool m_MakeProgress;
  Atomic<size_t> m_Calls;
};

struct VectorSignalContext {
  VectorSignalContext(size_t descriptor, InterruptingVectorFile* file)
      : descriptor(descriptor),
        file(file),
        noProgressResult(-2),
        noProgressError(0),
        partialResult(-2),
        partialError(0),
        returned(0) {}

  size_t descriptor;
  InterruptingVectorFile* file;
  int noProgressResult;
  int noProgressError;
  int partialResult;
  int partialError;
  Atomic<size_t> returned;
};

int vectorSignalWorker(void* parameter) {
  VectorSignalContext* context = reinterpret_cast<VectorSignalContext*>(parameter);
  char first[3] = {'a', 'b', 'c'};
  char second[2] = {'d', 'e'};
  struct iovec vectors[2] = {{first, sizeof(first)}, {second, sizeof(second)}};
  Thread* thread = Processor::information().getCurrentThread();

  context->file->setMakeProgress(false);
  thread->setErrno(PreservedErrno);
  context->noProgressResult = posix_writev(static_cast<int>(context->descriptor), vectors, 2);
  context->noProgressError = thread->getErrno();

  context->file->setMakeProgress(true);
  thread->setErrno(PreservedErrno);
  context->partialResult = posix_writev(static_cast<int>(context->descriptor), vectors, 2);
  context->partialError = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool vectorSignalProgress(Process* kernelProcess) {
  constexpr size_t Descriptor = 95;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  InterruptingVectorFile file;
  FileDescriptor* descriptor = new FileDescriptor(&file, 0, Descriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(Descriptor, descriptor);
  FileDescriptor::OpenFileDescriptionLease description = descriptor->acquireOpenFileDescription();

  VectorSignalContext context(Descriptor, &file);
  Thread* worker = new Thread(process, vectorSignalWorker, &context, nullptr, false, true, true);
  worker->setName("hosted vector signal progress");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  bool passed = started && joined && context.returned == 1 && context.noProgressResult == -1 &&
                context.noProgressError == Error::Interrupted && context.partialResult == 3 &&
                context.partialError == PreservedErrno && file.calls() == 2 &&
                descriptor->getOffset() == 3;
  passed = closeDescriptor(subsystem, Descriptor) && passed;
  description.reset();
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vector-signal-progress: "
        "writev did not distinguish interruption before and after progress");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vector-signal-progress");
  return true;
}

struct LargeVectorPipeContext {
  LargeVectorPipeContext(size_t descriptor, bool largeFirst)
      : descriptor(descriptor), largeFirst(largeFirst), result(-2), error(0), returned(0) {}

  size_t descriptor;
  bool largeFirst;
  int result;
  int error;
  Atomic<size_t> returned;
};

int largeVectorPipeWriter(void* parameter) {
  LargeVectorPipeContext* context = reinterpret_cast<LargeVectorPipeContext*>(parameter);
  char large[PIPE_BUF_MAX];
  char small = context->largeFirst ? 'b' : 'a';
  ByteSet(large, context->largeFirst ? 'a' : 'b', sizeof(large));
  struct iovec vectors[2] = {
      {context->largeFirst ? static_cast<void*>(large) : static_cast<void*>(&small),
       context->largeFirst ? sizeof(large) : 1},
      {context->largeFirst ? static_cast<void*>(&small) : static_cast<void*>(large),
       context->largeFirst ? 1 : sizeof(large)}};

  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result = posix_writev(static_cast<int>(context->descriptor), vectors, 2);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool runLargeVectorPipeCase(Process* process, FileDescriptor* reader, FileDescriptor* writer,
                            size_t descriptor, bool largeFirst) {
  char fill[PIPE_BUF_MAX];
  ByteSet(fill, 'f', sizeof(fill));
  if (writer->write(sizeof(fill), reinterpret_cast<uintptr_t>(fill), true) != sizeof(fill)) {
    return false;
  }

  char byte = 0;
  if (reader->read(1, reinterpret_cast<uintptr_t>(&byte), true) != 1 || byte != 'f') {
    return false;
  }

  LargeVectorPipeContext context(descriptor, largeFirst);
  Thread* worker = new Thread(process, largeVectorPipeWriter, &context, nullptr, false, true, true);
  worker->setName(largeFirst ? "hosted large-first vector pipe" : "hosted small-first vector pipe");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  char drain[PIPE_BUF_MAX];
  const bool drained = joined && reader->read(sizeof(drain), reinterpret_cast<uintptr_t>(drain),
                                              true) == sizeof(drain);
  return started && joined && drained && context.returned == 1 && context.result == 1 &&
         context.error == PreservedErrno && drain[PIPE_BUF_MAX - 1] == 'a';
}

bool vectorPipeBoundaryIndependentPartial(Process* kernelProcess) {
  constexpr size_t ReadDescriptor = 96;
  constexpr size_t WriteDescriptor = 97;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY | O_NONBLOCK);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  bool passed = runLargeVectorPipeCase(process, reader, writer, WriteDescriptor, true) &&
                runLargeVectorPipeCase(process, reader, writer, WriteDescriptor, false);
  passed = closeDescriptor(subsystem, WriteDescriptor) &&
           closeDescriptor(subsystem, ReadDescriptor) && passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vector-pipe-boundary-independent-partial: "
        "large nonblocking writev behavior depended on an iovec boundary");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vector-pipe-boundary-independent-partial");
  return true;
}

Atomic<size_t> g_VectorSigpipeHandlerCalls(0);
File* g_VectorSigpipeTarget = nullptr;

void vectorSigpipeHandler(size_t) {
  File::WriteGuard guard = g_VectorSigpipeTarget->lockWrites();
  g_VectorSigpipeHandlerCalls += 1;
}

struct VectorSigpipeContext {
  explicit VectorSigpipeContext(size_t descriptor)
      : descriptor(descriptor), result(-2), error(0), returned(0) {}

  size_t descriptor;
  int result;
  int error;
  Atomic<size_t> returned;
};

int vectorSigpipeWriter(void* parameter) {
  VectorSigpipeContext* context = reinterpret_cast<VectorSigpipeContext*>(parameter);
  char first = 's';
  char second = 'p';
  struct iovec vectors[2] = {{&first, 1}, {&second, 1}};
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(PreservedErrno);
  context->result = posix_writev(static_cast<int>(context->descriptor), vectors, 2);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool vectorSigpipeAfterWriteGuard(Process* kernelProcess) {
  constexpr size_t WriteDescriptor = 98;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  subsystem->addFileDescriptor(WriteDescriptor,
                               new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY));

  PosixSubsystem::SignalHandler* handler = new PosixSubsystem::SignalHandler;
  handler->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(&vectorSigpipeHandler), SIGPIPE);
  subsystem->setSignalHandler(SIGPIPE, handler);

  g_VectorSigpipeHandlerCalls = 0;
  g_VectorSigpipeTarget = pipe;
  VectorSigpipeContext context(WriteDescriptor);
  Thread* worker = new Thread(process, vectorSigpipeWriter, &context, nullptr, false, true, true);
  worker->setName("hosted vector SIGPIPE guard release");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  g_VectorSigpipeTarget = nullptr;

  bool passed = started && joined && context.returned == 1 && context.result == -1 &&
                context.error == Error::BrokenPipe && g_VectorSigpipeHandlerCalls == 1;
  passed = closeDescriptor(subsystem, WriteDescriptor) && passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vector-sigpipe-after-write-guard: "
        "SIGPIPE ran before vector write serialization was released");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vector-sigpipe-after-write-guard");
  return true;
}
}  // namespace

bool runHostedVectorIoRegressions(Process* process) {
  return vectorWriteBounceAndLifetime(process) && vectorReadBounceAndScatter(process) &&
         diskVectorReadBatching(process) && vectorUsercopyFaultProgress(process) &&
         vectorPipePayloadPrevalidation(process) && vectorSignalProgress(process) &&
         vectorPipeBoundaryIndependentPartial(process) && vectorSigpipeAfterWriteGuard(process);
}

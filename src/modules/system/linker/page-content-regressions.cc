/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include "DynamicLinker.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"

namespace {
bool fail(const char* test, const char* detail) {
  ERROR("HOSTED-PAGE-CONTENT-TEST: FAIL " << test << ": " << detail);
  return false;
}

size_t g_FailedAllocationCalls = 0;

physical_uintptr_t failDemandPageAllocation() {
  ++g_FailedAllocationCalls;
  return 0;
}

class DemandPageElf final : public Elf {
 public:
  DemandPageElf(size_t fileSize, size_t memorySize) {
    m_nProgramHeaders = 1;
    m_pProgramHeaders = new ElfProgramHeader_t[1];
    ByteSet(m_pProgramHeaders, 0, sizeof(ElfProgramHeader_t));
    m_pProgramHeaders[0].type = PT_LOAD;
    m_pProgramHeaders[0].filesz = fileSize;
    m_pProgramHeaders[0].memsz = memorySize;
  }

  void addRelocations(uintptr_t relativeOffset, uintptr_t relativeAddend, uintptr_t narrowOffset,
                      uintptr_t narrowSymbol, uintptr_t narrowAddend) {
    m_pRelaTable = new ElfRela_t[2];
    ByteSet(m_pRelaTable, 0, sizeof(ElfRela_t) * 2);
    m_pRelaTable[0].offset = relativeOffset;
    m_pRelaTable[0].info = 8;  // R_X86_64_RELATIVE
    m_pRelaTable[0].addend = relativeAddend;
    m_pRelaTable[1].offset = narrowOffset;
    m_pRelaTable[1].info = (static_cast<Elf_Xword>(1) << 32) | 10;  // R_X86_64_32
    m_pRelaTable[1].addend = narrowAddend;
    m_nRelaTableSize = sizeof(ElfRela_t) * 2;

    m_pDynamicSymbolTable = new ElfSymbol_t[2];
    ByteSet(m_pDynamicSymbolTable, 0, sizeof(ElfSymbol_t) * 2);
    m_pDynamicSymbolTable[1].info = ST_INFO(STB_LOCAL, STT_SECTION);
    m_pDynamicSymbolTable[1].shndx = 1;
    m_nDynamicSymbolTableSize = sizeof(ElfSymbol_t) * 2;

    m_pSectionHeaders = new ElfSectionHeader_t[2];
    ByteSet(m_pSectionHeaders, 0, sizeof(ElfSectionHeader_t) * 2);
    m_pSectionHeaders[1].addr = narrowSymbol;
    m_nSectionHeaders = 2;
  }
};

Atomic<size_t> g_DemandPageAllocationCalls(0);
Atomic<size_t> g_DemandPageReadyCalls(0);
Atomic<size_t> g_DemandPageFreeCalls(0);
Atomic<size_t> g_DemandPageFreeMask(0);
Atomic<bool> g_DemandPageReadyWaitFailed(false);
physical_uintptr_t g_DemandPageAllocations[2] = {};
Semaphore* g_DemandPageFirstReady = nullptr;
Semaphore* g_DemandPageResumeFirst = nullptr;

physical_uintptr_t allocateControlledDemandPage() {
  const size_t call = g_DemandPageAllocationCalls += 1;
  return call <= 2 ? g_DemandPageAllocations[call - 1] : 0;
}

void pauseFirstReadyDemandPage(uintptr_t) {
  const size_t call = g_DemandPageReadyCalls += 1;
  if (call == 1 && g_DemandPageFirstReady && g_DemandPageResumeFirst) {
    g_DemandPageFirstReady->release();
    if (!g_DemandPageResumeFirst->acquireForCompletion()) {
      g_DemandPageReadyWaitFailed = true;
    }
  }
}

void observeFreedDemandPage(physical_uintptr_t page) {
  g_DemandPageFreeCalls += 1;
  for (size_t i = 0; i < 2; ++i) {
    if (page == g_DemandPageAllocations[i]) {
      g_DemandPageFreeMask |= static_cast<size_t>(1) << i;
    }
  }
}

struct DemandPageLoadContext {
  DemandPageLoadContext(Elf* elf, uintptr_t buffer, size_t size, uintptr_t address)
      : elf(elf), buffer(buffer), size(size), address(address), completed(false), result(false) {}

  Elf* elf;
  uintptr_t buffer;
  size_t size;
  uintptr_t address;
  bool completed;
  bool result;
};

int demandPageLoadWorker(void* parameter) {
  DemandPageLoadContext* context = reinterpret_cast<DemandPageLoadContext*>(parameter);
  context->result = DynamicLinker::loadDemandPageForTest(
      context->elf, context->buffer, context->size, context->address, nullptr, context->address);
  context->completed = true;
  return context->result ? 0 : 1;
}

class SentinelFile final : public File {
 public:
  SentinelFile(size_t pageSize, size_t dataSize)
      : File(), m_PageSize(pageSize), m_DataSize(dataSize), m_Reads(0), m_ReadShapeValid(true) {
    setSize(dataSize);
  }

  size_t reads() const {
    return m_Reads;
  }

  bool readShapeValid() const {
    return m_ReadShapeValid;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    ++m_Reads;
    if (location != 0 || size != m_DataSize || !buffer) {
      m_ReadShapeValid = false;
      return 0;
    }

    uint8_t* bytes = reinterpret_cast<uint8_t*>(buffer);
    ByteSet(bytes, 0xA5, m_PageSize);
    for (size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<uint8_t>(i + 1);
    }
    return size;
  }

 private:
  size_t m_PageSize;
  size_t m_DataSize;
  size_t m_Reads;
  bool m_ReadShapeValid;
};

class BlockingSentinelFile final : public File {
 public:
  explicit BlockingSentinelFile(size_t dataSize)
      : File(),
        m_DataSize(dataSize),
        m_ReadEntered(0),
        m_ResumeRead(0),
        m_Reads(0),
        m_ReadShapeValid(true) {
    setSize(dataSize);
  }

  bool waitUntilRead() {
    return m_ReadEntered.acquireForCompletion(1, 2);
  }

  void resumeRead() {
    m_ResumeRead.release();
  }

  size_t reads() const {
    return m_Reads;
  }

  bool readShapeValid() const {
    return m_ReadShapeValid;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    ++m_Reads;
    if (location != 0 || size != m_DataSize || !buffer) {
      m_ReadShapeValid = false;
      return 0;
    }

    m_ReadEntered.release();
    if (!m_ResumeRead.acquireForCompletion()) {
      m_ReadShapeValid = false;
      return 0;
    }

    uint8_t* bytes = reinterpret_cast<uint8_t*>(buffer);
    for (size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<uint8_t>(i + 1);
    }
    return size;
  }

 private:
  size_t m_DataSize;
  Semaphore m_ReadEntered;
  Semaphore m_ResumeRead;
  size_t m_Reads;
  bool m_ReadShapeValid;
};

struct PublishAfterInitialiseContext {
  PublishAfterInitialiseContext(MemoryMappedFile* mapping, uintptr_t address)
      : mapping(mapping), address(address), completed(false), result(false) {}

  MemoryMappedFile* mapping;
  uintptr_t address;
  bool completed;
  bool result;
};

int publishAfterInitialiseWorker(void* parameter) {
  PublishAfterInitialiseContext* context =
      reinterpret_cast<PublishAfterInitialiseContext*>(parameter);
  context->result = context->mapping->trap(Processor::information().getVirtualAddressSpace(),
                                           context->address, false);
  context->completed = true;
  return context->result ? 0 : 1;
}

bool memoryMappedFileEofZeroFill() {
  constexpr size_t DataSize = 37;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (pageSize <= DataSize) {
    return fail("mmap-eof-zero-fill", "target page is too small for the fixture");
  }

  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread ? thread->getParent() : nullptr;
  if (!process) {
    return fail("mmap-eof-zero-fill", "no current process");
  }

  uintptr_t address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, pageSize, address)) {
    return fail("mmap-eof-zero-fill", "could not reserve a target page");
  }

  bool trapped = false;
  bool dataIntact = false;
  bool tailZero = false;
  bool readValid = false;
  {
    SentinelFile file(pageSize, DataSize);
    MemoryMappedFile mapping(address, DataSize, 0, &file, true, MemoryMappedObject::Read);
    trapped = mapping.trap(Processor::information().getVirtualAddressSpace(), address, false);
    if (trapped) {
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(address);
      dataIntact = true;
      for (size_t i = 0; i < DataSize; ++i) {
        if (bytes[i] != static_cast<uint8_t>(i + 1)) {
          dataIntact = false;
          break;
        }
      }

      tailZero = true;
      for (size_t i = DataSize; i < pageSize; ++i) {
        if (bytes[i] != 0) {
          tailZero = false;
          break;
        }
      }
    }
    readValid = file.reads() == 1 && file.readShapeValid();
  }

  process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
  if (!trapped || !readValid || !dataIntact || !tailZero) {
    return fail("mmap-eof-zero-fill", "the copied EOF page retained non-file data");
  }

  NOTICE("HOSTED-PAGE-CONTENT-TEST: PASS mmap-eof-zero-fill");
  return true;
}

bool memoryMappedFilePublishesAfterInitialise() {
  constexpr size_t DataSize = 37;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (pageSize <= DataSize) {
    return fail("mmap-publish-after-init", "target page is too small for the fixture");
  }

  Thread* current = Processor::information().getCurrentThread();
  Process* process = current ? current->getParent() : nullptr;
  if (!process) {
    return fail("mmap-publish-after-init", "no current process");
  }

  uintptr_t address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, pageSize, address)) {
    return fail("mmap-publish-after-init", "could not reserve a target page");
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  bool started = false;
  bool readEntered = false;
  bool absentWhileInitialising = false;
  bool joined = false;
  bool completed = false;
  bool trapped = false;
  bool mapped = false;
  bool dataIntact = false;
  bool tailZero = false;
  bool readValid = false;
  {
    BlockingSentinelFile file(DataSize);
    MemoryMappedFile mapping(address, DataSize, 0, &file, true, MemoryMappedObject::Read);
    PublishAfterInitialiseContext context(&mapping, address);
    Thread* worker =
        new Thread(process, publishAfterInitialiseWorker, &context, nullptr, false, true, true);
    worker->setName("hosted mmap publish-after-init");
    started = worker->start();
    readEntered = started && file.waitUntilRead();
    absentWhileInitialising = readEntered && !va.isMapped(reinterpret_cast<void*>(address));
    file.resumeRead();
    joined = started && worker->joinForCompletion();
    if (!started) {
      delete worker;
    }

    completed = context.completed;
    trapped = context.result;
    mapped = va.isMapped(reinterpret_cast<void*>(address));
    dataIntact = mapped;
    tailZero = mapped;
    if (mapped) {
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(address);
      for (size_t i = 0; i < DataSize; ++i) {
        if (bytes[i] != static_cast<uint8_t>(i + 1)) {
          dataIntact = false;
          break;
        }
      }
      for (size_t i = DataSize; i < pageSize; ++i) {
        if (bytes[i]) {
          tailZero = false;
          break;
        }
      }
    }
    readValid = file.reads() == 1 && file.readShapeValid();
  }

  process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
  if (!started || !readEntered || !absentWhileInitialising || !joined || !completed || !trapped ||
      !mapped || !dataIntact || !tailZero || !readValid) {
    return fail("mmap-publish-after-init", "the user mapping was visible before page population");
  }

  NOTICE("HOSTED-PAGE-CONTENT-TEST: PASS mmap-publish-after-init");
  return true;
}

bool dynamicDemandPagePublishesOnce() {
  constexpr size_t FileSize = 64;
  constexpr uintptr_t RelocationOffset = 16;
  constexpr uintptr_t RelocationAddend = 0x1234;
  constexpr uintptr_t NarrowRelocationSymbol = 0x10203040;
  constexpr uintptr_t NarrowRelocationAddend = 0x55;

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (pageSize <= FileSize) {
    return fail("dynamic-demand-page-publish", "target page is too small for the fixture");
  }
  const uintptr_t NarrowRelocationOffset = pageSize - sizeof(uint32_t);

  Thread* current = Processor::information().getCurrentThread();
  Process* process = current ? current->getParent() : nullptr;
  if (!process) {
    return fail("dynamic-demand-page-publish", "no current process");
  }

  uintptr_t address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, pageSize, address)) {
    return fail("dynamic-demand-page-publish", "could not reserve a target page");
  }

  PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
  g_DemandPageAllocations[0] = memory.allocatePage();
  g_DemandPageAllocations[1] = memory.allocatePage();
  if (!g_DemandPageAllocations[0] || !g_DemandPageAllocations[1]) {
    if (g_DemandPageAllocations[0]) {
      memory.freePage(g_DemandPageAllocations[0]);
    }
    if (g_DemandPageAllocations[1]) {
      memory.freePage(g_DemandPageAllocations[1]);
    }
    process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
    return fail("dynamic-demand-page-publish", "could not allocate controlled pages");
  }

  uint8_t fileData[FileSize];
  for (size_t i = 0; i < FileSize; ++i) {
    fileData[i] = static_cast<uint8_t>((i * 5) + 1);
  }

  DemandPageElf elf(FileSize, pageSize);
  elf.addRelocations(RelocationOffset, RelocationAddend, NarrowRelocationOffset,
                     NarrowRelocationSymbol, NarrowRelocationAddend);

  Semaphore firstReady(0);
  Semaphore resumeFirst(0);
  g_DemandPageFirstReady = &firstReady;
  g_DemandPageResumeFirst = &resumeFirst;
  g_DemandPageAllocationCalls = 0;
  g_DemandPageReadyCalls = 0;
  g_DemandPageFreeCalls = 0;
  g_DemandPageFreeMask = 0;
  g_DemandPageReadyWaitFailed = false;
  DynamicLinker::setDemandPageAllocationHookForTest(allocateControlledDemandPage);
  DynamicLinker::setDemandPageReadyHookForTest(pauseFirstReadyDemandPage);
  DynamicLinker::setDemandPageFreeHookForTest(observeFreedDemandPage);

  DemandPageLoadContext firstContext(&elf, reinterpret_cast<uintptr_t>(fileData), sizeof(fileData),
                                     address);
  Thread* first =
      new Thread(process, demandPageLoadWorker, &firstContext, nullptr, false, true, true);
  first->setName("hosted linker first page publisher");
  const bool firstStarted = first->start();
  const bool firstReachedReady = firstStarted && firstReady.acquireForCompletion(1, 2);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  const bool absentWhileFirstReady =
      firstReachedReady && !va.isMapped(reinterpret_cast<void*>(address));

  DemandPageLoadContext secondContext(&elf, reinterpret_cast<uintptr_t>(fileData), sizeof(fileData),
                                      address);
  Thread* second = nullptr;
  bool secondStarted = false;
  bool secondJoined = false;
  physical_uintptr_t pageAfterSecond = 0;
  if (firstReachedReady) {
    second = new Thread(process, demandPageLoadWorker, &secondContext, nullptr, false, true, true);
    second->setName("hosted linker second page publisher");
    secondStarted = second->start();
    secondJoined = secondStarted && second->joinForCompletion();
    if (!secondStarted) {
      delete second;
    }
    if (va.isMapped(reinterpret_cast<void*>(address))) {
      size_t flags = 0;
      va.getMapping(reinterpret_cast<void*>(address), pageAfterSecond, flags);
    }
  }

  resumeFirst.release();
  const bool firstJoined = firstStarted && first->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }

  DynamicLinker::setDemandPageReadyHookForTest(nullptr);
  DynamicLinker::setDemandPageAllocationHookForTest(nullptr);
  DynamicLinker::setDemandPageFreeHookForTest(nullptr);
  g_DemandPageFirstReady = nullptr;
  g_DemandPageResumeFirst = nullptr;

  const bool mapped = va.isMapped(reinterpret_cast<void*>(address));
  physical_uintptr_t finalPage = 0;
  size_t finalFlags = 0;
  if (mapped) {
    va.getMapping(reinterpret_cast<void*>(address), finalPage, finalFlags);
  }

  bool dataIntact = mapped;
  bool tailZero = mapped;
  bool relocationCorrect = mapped;
  bool narrowRelocationCorrect = mapped;
  if (mapped) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i < FileSize; ++i) {
      if (i >= RelocationOffset && i < RelocationOffset + sizeof(uint64_t)) {
        continue;
      }
      if (i >= NarrowRelocationOffset && i < NarrowRelocationOffset + sizeof(uint32_t)) {
        continue;
      }
      if (bytes[i] != fileData[i]) {
        dataIntact = false;
        break;
      }
    }
    for (size_t i = FileSize; i < pageSize; ++i) {
      if (i >= NarrowRelocationOffset && i < NarrowRelocationOffset + sizeof(uint32_t)) {
        continue;
      }
      if (bytes[i]) {
        tailZero = false;
        break;
      }
    }
    relocationCorrect = *reinterpret_cast<const uint64_t*>(address + RelocationOffset) ==
                        address + RelocationAddend;
    narrowRelocationCorrect =
        *reinterpret_cast<const uint32_t*>(address + NarrowRelocationOffset) ==
        NarrowRelocationSymbol + NarrowRelocationAddend;
  }

  if (mapped) {
    va.unmap(reinterpret_cast<void*>(address));
    memory.freePage(finalPage);
  }
  for (size_t i = 0; i < 2; ++i) {
    const bool wasPublished = mapped && finalPage == g_DemandPageAllocations[i];
    const bool wasFreed = (g_DemandPageFreeMask & (static_cast<size_t>(1) << i)) != 0;
    if (!wasPublished && !wasFreed) {
      memory.freePage(g_DemandPageAllocations[i]);
    }
  }
  process->freeUserRange(Process::UserRegion::Normal, address, pageSize);

  const bool passed =
      firstStarted && firstReachedReady && absentWhileFirstReady && secondStarted && secondJoined &&
      secondContext.completed && secondContext.result && firstJoined && firstContext.completed &&
      firstContext.result && g_DemandPageAllocationCalls == 2 && g_DemandPageReadyCalls == 2 &&
      mapped && pageAfterSecond == g_DemandPageAllocations[1] && finalPage == pageAfterSecond &&
      g_DemandPageFreeCalls == 1 && g_DemandPageFreeMask == 1 && !g_DemandPageReadyWaitFailed &&
      dataIntact && tailZero && relocationCorrect && narrowRelocationCorrect;
  if (!passed) {
    return fail(
        "dynamic-demand-page-publish",
        "a staging page was published early, overwritten by a loser, or relocated via its alias");
  }

  NOTICE("HOSTED-PAGE-CONTENT-TEST: PASS dynamic-demand-page-publish");
  return true;
}

bool memoryMapFaultReplay() {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  MemoryMapManager& manager = MemoryMapManager::instance();
  MemoryMappedObject* mapping =
      manager.mapAnon(address, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping) {
    return fail("mmap-fault-replay", "could not create an anonymous mapping");
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  const bool initialRead = manager.trapForHostedTest(address, false, false);
  const bool initialMapped = va.isMapped(reinterpret_cast<void*>(address));

  physical_uintptr_t readPage = 0;
  size_t readFlags = 0;
  if (initialRead && initialMapped) {
    va.getMapping(reinterpret_cast<void*>(address), readPage, readFlags);
  }

  const bool missingReplay = manager.trapForHostedTest(address, false, false);
  const bool replayedReadMapped = va.isMapped(reinterpret_cast<void*>(address));
  physical_uintptr_t replayedReadPage = 0;
  size_t replayedReadFlags = 0;
  if (missingReplay && replayedReadMapped) {
    va.getMapping(reinterpret_cast<void*>(address), replayedReadPage, replayedReadFlags);
  }

  const bool copyOnWrite = manager.trapForHostedTest(address, true, true);
  const bool writeMapped = va.isMapped(reinterpret_cast<void*>(address));
  physical_uintptr_t writePage = 0;
  size_t writeFlags = 0;
  if (copyOnWrite && writeMapped) {
    va.getMapping(reinterpret_cast<void*>(address), writePage, writeFlags);
  }

  const bool writeReplay = manager.trapForHostedTest(address, true, true);
  const bool replayedWriteMapped = va.isMapped(reinterpret_cast<void*>(address));
  physical_uintptr_t replayedWritePage = 0;
  size_t replayedWriteFlags = 0;
  if (writeReplay && replayedWriteMapped) {
    va.getMapping(reinterpret_cast<void*>(address), replayedWritePage, replayedWriteFlags);
  }

  manager.removeAndRelease(address, pageSize);

  const bool passed = initialRead && initialMapped && missingReplay && replayedReadMapped &&
                      copyOnWrite && writeMapped && writeReplay && replayedWriteMapped &&
                      readPage == replayedReadPage && readFlags == replayedReadFlags &&
                      writePage != readPage && (writeFlags & VirtualAddressSpace::Write) &&
                      writePage == replayedWritePage && writeFlags == replayedWriteFlags;
  if (!passed) {
    return fail("mmap-fault-replay", "a replay repeated or skipped demand-page work");
  }

  NOTICE("HOSTED-PAGE-CONTENT-TEST: PASS mmap-fault-replay");
  return true;
}
}  // namespace

bool runHostedPageContentRegressions() {
  constexpr size_t FileSize = 48;
  constexpr size_t MemorySize = 96;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (pageSize <= MemorySize) {
    return fail("dynamic-demand-page-zero-fill", "target page is too small for the fixture");
  }

  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread ? thread->getParent() : nullptr;
  if (!process) {
    return fail("dynamic-demand-page-zero-fill", "no current process");
  }

  uintptr_t address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, pageSize, address)) {
    return fail("dynamic-demand-page-zero-fill", "could not reserve a target page");
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();

  uint8_t fileData[FileSize];
  for (size_t i = 0; i < FileSize; ++i) {
    fileData[i] = static_cast<uint8_t>((i * 7) + 3);
  }
  DemandPageElf elf(FileSize, MemorySize);

  g_FailedAllocationCalls = 0;
  DynamicLinker::setDemandPageAllocationHookForTest(failDemandPageAllocation);
  const bool allocationFailureRejected = !DynamicLinker::loadDemandPageForTest(
      &elf, reinterpret_cast<uintptr_t>(fileData), sizeof(fileData), address, nullptr, address);
  DynamicLinker::setDemandPageAllocationHookForTest(nullptr);
  const bool allocationFailureClean = !va.isMapped(reinterpret_cast<void*>(address));
  if (!allocationFailureRejected || g_FailedAllocationCalls != 1 || !allocationFailureClean) {
    if (!allocationFailureClean) {
      va.unmap(reinterpret_cast<void*>(address));
    }
    process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
    return fail("dynamic-demand-page-zero-fill", "an allocation failure left demand-page state");
  }

  const physical_uintptr_t dirtyPage = memory.allocatePage();
  if (!va.map(dirtyPage, reinterpret_cast<void*>(address), VirtualAddressSpace::Write)) {
    memory.freePage(dirtyPage);
    process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
    return fail("dynamic-demand-page-zero-fill", "could not map the sentinel page");
  }

  ByteSet(reinterpret_cast<void*>(address), 0xA5, pageSize);
  va.unmap(reinterpret_cast<void*>(address));
  memory.freePage(dirtyPage);

  const bool loaded = DynamicLinker::loadDemandPageForTest(
      &elf, reinterpret_cast<uintptr_t>(fileData), sizeof(fileData), address, nullptr, address);

  physical_uintptr_t loadedPage = 0;
  size_t flags = 0;
  const bool mapped = va.isMapped(reinterpret_cast<void*>(address));
  if (mapped) {
    va.getMapping(reinterpret_cast<void*>(address), loadedPage, flags);
  }

  bool fileDataIntact = loaded && mapped;
  bool remainderZero = loaded && mapped;
  if (loaded && mapped) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i < FileSize; ++i) {
      if (bytes[i] != fileData[i]) {
        fileDataIntact = false;
        break;
      }
    }
    for (size_t i = FileSize; i < pageSize; ++i) {
      if (bytes[i] != 0) {
        remainderZero = false;
        break;
      }
    }
  }

  if (mapped) {
    va.unmap(reinterpret_cast<void*>(address));
    memory.freePage(loadedPage);
  }
  process->freeUserRange(Process::UserRegion::Normal, address, pageSize);

  const bool reusedSentinel = mapped && loadedPage == dirtyPage;
  const bool demandPassed = loaded && reusedSentinel && fileDataIntact && remainderZero;
  if (!demandPassed) {
    return fail("dynamic-demand-page-zero-fill", "the demand-loaded page retained prior data");
  }

  NOTICE("HOSTED-PAGE-CONTENT-TEST: PASS dynamic-demand-page-zero-fill");
  return dynamicDemandPagePublishesOnce() && memoryMappedFileEofZeroFill() &&
         memoryMappedFilePublishesAfterInitialise() && memoryMapFaultReplay();
}

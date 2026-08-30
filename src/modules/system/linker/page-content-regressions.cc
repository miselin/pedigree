/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/process/Process.h"
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
};

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
  if (!process->getSpaceAllocator().allocate(pageSize, address)) {
    return fail("mmap-eof-zero-fill", "could not reserve a target page");
  }

  bool trapped = false;
  bool dataIntact = false;
  bool tailZero = false;
  bool readValid = false;
  {
    SentinelFile file(pageSize, DataSize);
    MemoryMappedFile mapping(address, DataSize, 0, &file, true, MemoryMappedObject::Read);
    trapped = mapping.trap(address, false);
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

  process->getSpaceAllocator().free(address, pageSize);
  if (!trapped || !readValid || !dataIntact || !tailZero) {
    return fail("mmap-eof-zero-fill", "the copied EOF page retained non-file data");
  }

  NOTICE("HOSTED-PAGE-CONTENT-TEST: PASS mmap-eof-zero-fill");
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
  if (!process->getSpaceAllocator().allocate(pageSize, address)) {
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
    process->getSpaceAllocator().free(address, pageSize);
    return fail("dynamic-demand-page-zero-fill", "an allocation failure left demand-page state");
  }

  const physical_uintptr_t dirtyPage = memory.allocatePage();
  if (!va.map(dirtyPage, reinterpret_cast<void*>(address), VirtualAddressSpace::Write)) {
    memory.freePage(dirtyPage);
    process->getSpaceAllocator().free(address, pageSize);
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
  process->getSpaceAllocator().free(address, pageSize);

  const bool reusedSentinel = mapped && loadedPage == dirtyPage;
  const bool demandPassed = loaded && reusedSentinel && fileDataIntact && remainderZero;
  if (!demandPassed) {
    return fail("dynamic-demand-page-zero-fill", "the demand-loaded page retained prior data");
  }

  NOTICE("HOSTED-PAGE-CONTENT-TEST: PASS dynamic-demand-page-zero-fill");
  return memoryMappedFileEofZeroFill();
}

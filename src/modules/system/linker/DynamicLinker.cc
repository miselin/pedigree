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

#include "DynamicLinker.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/linker/SymbolTable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/KernelCoreSyscallManager.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/Module.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"

namespace {
bool prepareNativeImageAllocation() {
  auto& space = Processor::information().getVirtualAddressSpace();
  auto* account = space.memoryLockAccount();
  if (account && account->futureMode() != MemoryLockMode::None) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  // This optional linker owns demand-paged raw ELF extents. The maintained
  // POSIX loader uses managed mappings with a complete locking inventory.
  space.rawUserMemory().setCompleteInventory(false);
  return true;
}

class DemandPageStagingMapping {
 public:
  explicit DemandPageStagingMapping(physical_uintptr_t page)
      : m_Region("Dynamic Linker Demand Page"), m_Mapped(false) {
    PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
    if (!memory.allocateRegion(
            m_Region, 1, PhysicalMemoryManager::virtualOnly | PhysicalMemoryManager::anonymous,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
      return;
    }

    VirtualAddressSpace& kernelSpace = VirtualAddressSpace::getKernelAddressSpace();
    m_Mapped = kernelSpace.map(page, m_Region.virtualAddress(),
                               VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write);
    if (!m_Mapped) {
      m_Region.free();
    }
  }

  ~DemandPageStagingMapping() {
    if (m_Mapped) {
      VirtualAddressSpace::getKernelAddressSpace().unmap(m_Region.virtualAddress());
    }
    m_Region.free();
  }

  bool valid() const {
    return m_Mapped;
  }

  uintptr_t address() const {
    return reinterpret_cast<uintptr_t>(m_Region.virtualAddress());
  }

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(DemandPageStagingMapping);

  MemoryRegion m_Region;
  bool m_Mapped;
};

Mutex g_DemandPagePublishLock;
}  // namespace

DLTrapHandler DLTrapHandler::m_Instance;

#if defined(PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS) && \
    (!defined(PEDIGREE_HOSTED_DARWIN) || !PEDIGREE_HOSTED_DARWIN)
extern bool runHostedPageContentRegressions();
#endif

#if defined(PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS)
namespace {
physical_uintptr_t (*g_DemandPageAllocationHook)() = nullptr;
void (*g_DemandPageReadyHook)(uintptr_t) = nullptr;
void (*g_DemandPageFreeHook)(physical_uintptr_t) = nullptr;
}  // namespace
#endif

namespace {
void releaseDemandPage(physical_uintptr_t page) {
#if defined(PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS)
  if (g_DemandPageFreeHook) {
    g_DemandPageFreeHook(page);
  }
#endif
  PhysicalMemoryManager::instance().freePage(page);
}
}  // namespace

uintptr_t DynamicLinker::resolvePlt(SyscallState& state) {
  Process* pProcess = Processor::information().getCurrentThread()->getParent();

  return pProcess->getLinker()->resolvePltSymbol(state.getSyscallParameter(0),
                                                 state.getSyscallParameter(1));
}

DynamicLinker::DynamicLinker()
    : m_pProgramElf(0),
      m_ProgramStart(0),
      m_ProgramSize(0),
      m_ProgramBuffer(0),
      m_LoadedObjects(),
      m_Objects() {}

DynamicLinker::DynamicLinker(const DynamicLinker& other)
    : m_pProgramElf(other.m_pProgramElf ? new Elf(*other.m_pProgramElf) : nullptr),
      m_ProgramStart(other.m_ProgramStart),
      m_ProgramSize(other.m_ProgramSize),
      m_ProgramBuffer(other.m_ProgramBuffer),
      m_LoadedObjects(other.m_LoadedObjects),
      m_Objects() {
  // Tree iteration uses a tree-owned cursor. Walk our shallow node copy so
  // copying a linker cannot disturb an active traversal of the source linker.
  Tree<uintptr_t, SharedObject*> objects(other.m_Objects);
  for (Tree<uintptr_t, SharedObject*>::Iterator it = objects.begin(); it != objects.end(); it++) {
    uintptr_t key = it.key();
    SharedObject* pSo = it.value();
    m_Objects.insert(
        key, new SharedObject(new Elf(*pSo->elf), pSo->file, pSo->buffer, pSo->address, pSo->size));
  }
}

DynamicLinker::~DynamicLinker() {
  //    VirtualAddressSpace &va =
  //    Processor::information().getVirtualAddressSpace();

  for (Tree<uintptr_t, SharedObject*>::Iterator it = m_Objects.begin(); it != m_Objects.end();
       it++) {
    SharedObject* pSo = it.value();
    delete pSo->elf;
    delete pSo;
  }

  delete m_pProgramElf;
}

bool DynamicLinker::loadProgram(File* pFile, bool bDryRun, bool bInterpreter,
                                String* sInterpreter) {
  if (!pFile)
    return false;

  uintptr_t buffer = 0;
  MemoryMappedObject* pMmFile = MemoryMapManager::instance().mapFile(
      pFile, buffer, pFile->getSize(), MemoryMappedObject::Read);
  if (!pMmFile) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  String fileName;
  pFile->getName(fileName);
#if VERBOSE_KERNEL
  NOTICE("DynamicLinker::loadProgram(" << fileName << ")");
#endif

  Elf* programElf = new Elf();
  if (!programElf) {
    MemoryMapManager::instance().unmap(pMmFile);
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  if (!bDryRun) {
    delete m_pProgramElf;
    m_pProgramElf = programElf;
    if (!m_pProgramElf->create(reinterpret_cast<uint8_t*>(buffer), pFile->getSize())) {
      ERROR("DynamicLinker: Main program ELF failed to create: `" << fileName << "' at " << buffer);
      MemoryMapManager::instance().unmap(pMmFile);

      delete m_pProgramElf;
      m_pProgramElf = 0;
      return false;
    }

    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    if (!prepareNativeImageAllocation() ||
        !m_pProgramElf->allocate(reinterpret_cast<uint8_t*>(buffer), pFile->getSize(),
                                 m_ProgramStart, 0, false, &m_ProgramSize)) {
      ERROR("DynamicLinker: Main program ELF failed to load: `" << fileName << "'");
      MemoryMapManager::instance().unmap(pMmFile);

      delete m_pProgramElf;
      m_pProgramElf = 0;
      return false;
    }

    m_ProgramBuffer = buffer;
  } else {
    if (!programElf->createNeededOnly(reinterpret_cast<uint8_t*>(buffer), pFile->getSize())) {
      ERROR("DynamicLinker: Main program ELF failed to create: `" << fileName << "' at " << buffer);
      MemoryMapManager::instance().unmap(pMmFile);

      if (!bDryRun) {
        delete m_pProgramElf;
        m_pProgramElf = 0;
      } else
        delete programElf;
      return false;
    }
  }

  if (bInterpreter) {
    if (!sInterpreter)
      return false;
    *sInterpreter = programElf->getInterpreter();
    bool hasInterpreter = sInterpreter->length() > 0;
    if (bDryRun) {
      // Clean up the ELF
      delete programElf;
      m_pProgramElf = 0;

      // Unmap this file - any future loadProgram will map it again.
      MemoryMapManager::instance().unmap(pMmFile);
    }
    return hasInterpreter;
  }

  List<char*>& dependencies = programElf->neededLibraries();

  // Load all dependencies
  for (List<char*>::Iterator it = dependencies.begin(); it != dependencies.end(); it++) {
    // Extreme validation
    if (!*it)
      continue;
    void* loadedObject = nullptr;
    if (m_LoadedObjects.lookup(String(*it), loadedObject)) {
      WARNING("Object `" << *it << "' has already been loaded");
      continue;
    }

    String filename;
    filename += "/usr/lib/";
    filename += *it;
    Directory::ChildLease dependencyLease;
    File* pDependencyFile = VFS::instance().findRetained(filename, dependencyLease);
    if (!pDependencyFile) {
      ERROR("DynamicLinker: Dependency `" << filename << "' not found!");
      if (!bDryRun) {
        delete m_pProgramElf;
        m_pProgramElf = 0;
      } else
        delete programElf;
      return false;
    }
    while (pDependencyFile && pDependencyFile->isSymlink()) {
      Directory::ChildLease targetLease;
      pDependencyFile = Symlink::fromFile(pDependencyFile)->followLinkRetained(targetLease);
      dependencyLease.swap(targetLease);
    }
    if (!pDependencyFile || !loadObject(pDependencyFile, bDryRun)) {
      ERROR("DynamicLinker: Dependency `" << filename << "' failed to load!");
      if (!bDryRun) {
        delete m_pProgramElf;
        m_pProgramElf = 0;
      } else
        delete programElf;
      return false;
    }

    // Success! Add the filename of the library (NOT WITH LIBRARIES
    // DIRECTORY) to the known loaded objects list.
    if (!bDryRun)
      m_LoadedObjects.insert(String(*it), reinterpret_cast<void*>(1));
  }

  if (!bDryRun)
    initPlt(m_pProgramElf, 0);
  else
    delete programElf;

  return true;
}

bool DynamicLinker::loadObject(File* pFile, bool bDryRun) {
  uintptr_t buffer = 0;
  size_t size;
  uintptr_t loadBase = 0;
  MemoryMappedObject* pMmFile = MemoryMapManager::instance().mapFile(
      pFile, buffer, pFile->getSize(), MemoryMappedObject::Read);
  if (!pMmFile) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  Elf* pElf = new Elf();
  if (!pElf) {
    MemoryMapManager::instance().unmap(pMmFile);
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  SharedObject* pSo = 0;

  String fileName;
  pFile->getName(fileName);
  NOTICE("DynamicLinker::loadObject(" << fileName << ")");

  if (!bDryRun) {
    if (!pElf->create(reinterpret_cast<uint8_t*>(buffer), pFile->getSize())) {
      ERROR("DynamicLinker: ELF creation failed for file `" << pFile->getName() << "'");
      delete pElf;
      MemoryMapManager::instance().unmap(pMmFile);
      return false;
    }

    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    if (!prepareNativeImageAllocation() ||
        !pElf->allocate(reinterpret_cast<uint8_t*>(buffer), pFile->getSize(), loadBase,
                        m_pProgramElf->getSymbolTable(), false, &size)) {
      ERROR("DynamicLinker: ELF allocate failed for file `" << pFile->getName() << "'");
      delete pElf;
      MemoryMapManager::instance().unmap(pMmFile);
      return false;
    }

    pSo = new SharedObject(pElf, pMmFile, buffer, loadBase, size);

    m_Objects.insert(loadBase, pSo);
  } else {
    if (!pElf->createNeededOnly(reinterpret_cast<uint8_t*>(buffer), pFile->getSize())) {
      ERROR("DynamicLinker: ELF creation failed for file `" << pFile->getName() << "'");
      delete pElf;
      return false;
    }
  }

  List<char*>& dependencies = pElf->neededLibraries();

  // Load all dependencies
  for (List<char*>::Iterator it = dependencies.begin(); it != dependencies.end(); it++) {
    // Extreme validation
    if (!*it)
      continue;
    void* loadedObject = nullptr;
    if (m_LoadedObjects.lookup(String(*it), loadedObject)) {
      WARNING("Object `" << *it << "' has already been loaded");
      continue;
    }

    String filename;
    filename += "/usr/lib/";
    filename += *it;
    Directory::ChildLease dependencyLease;
    File* _pFile = VFS::instance().findRetained(filename, dependencyLease);
    if (!_pFile) {
      ERROR("DynamicLinker: Dependency `" << filename << "' not found!");
      if (!bDryRun) {
        if (loadBase) {
          m_Objects.remove(loadBase);
        }
        delete pSo;
      }
      delete pElf;
      return false;
    }
    while (_pFile && _pFile->isSymlink()) {
      Directory::ChildLease targetLease;
      _pFile = Symlink::fromFile(_pFile)->followLinkRetained(targetLease);
      dependencyLease.swap(targetLease);
    }
    if (!_pFile || !loadObject(_pFile, bDryRun)) {
      ERROR("DynamicLinker: Dependency `" << filename << "' failed to load!");
      if (!bDryRun) {
        if (loadBase) {
          m_Objects.remove(loadBase);
        }
        delete pSo;
      }
      delete pElf;
      return false;
    }

    // Success! Add the filename of the library (NOT WITH LIBRARIES
    // DIRECTORY) to the known loaded objects list.
    if (!bDryRun)
      m_LoadedObjects.insert(String(*it), reinterpret_cast<void*>(1));
  }

  if (!bDryRun)
    initPlt(pElf, loadBase);
  else
    delete pElf;

  return true;
}

#if defined(PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS)
void DynamicLinker::setDemandPageAllocationHookForTest(physical_uintptr_t (*hook)()) {
  g_DemandPageAllocationHook = hook;
}

void DynamicLinker::setDemandPageReadyHookForTest(void (*hook)(uintptr_t)) {
  g_DemandPageReadyHook = hook;
}

void DynamicLinker::setDemandPageFreeHookForTest(void (*hook)(physical_uintptr_t)) {
  g_DemandPageFreeHook = hook;
}
#endif

bool DynamicLinker::loadDemandPage(Elf* pElf, uintptr_t buffer, size_t size, uintptr_t offset,
                                   SymbolTable* pSymbols, uintptr_t address) {
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uintptr_t v = address & ~(pageSize - 1);

  // Grab a physical page.
  const physical_uintptr_t p =
#if defined(PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS)
      g_DemandPageAllocationHook ? g_DemandPageAllocationHook() :
#endif
                                 PhysicalMemoryManager::instance().allocatePage();
  if (!p) {
    WARNING("IMAGE: allocatePage() failed in ElfImage::trap()");
    return false;
  }

  bool loaded = false;
  {
    DemandPageStagingMapping staging(p);
    if (!staging.valid()) {
      WARNING("IMAGE: could not create a kernel staging mapping for vaddr: " << v);
      releaseDemandPage(p);
      return false;
    }

    ByteSet(reinterpret_cast<void*>(staging.address()), 0, pageSize);

    // Keep all logical relocation addresses in the process address space, but
    // direct writes to the private staging alias until the page is complete.
    loaded = pElf->load(reinterpret_cast<uint8_t*>(buffer), size, offset, pSymbols, v, v + pageSize,
                        true, staging.address());
    if (loaded) {
      // Relocations can modify executable bytes after segment population, so
      // publish only after cache maintenance covers the completed page.
      Processor::flushDCacheAndInvalidateICache(staging.address(), staging.address() + pageSize);
    }
  }

  if (!loaded) {
    WARNING("LINKER: load() failed in DynamicLinker::trap()");
    releaseDemandPage(p);
    return false;
  }

#if defined(PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS)
  if (g_DemandPageReadyHook) {
    g_DemandPageReadyHook(v);
  }
#endif

  // Hosted map() uses MAP_FIXED and therefore cannot itself arbitrate two
  // publishers. Keep the recheck and map in one short cross-host critical
  // section so a completed page has exactly one winner.
  LockGuard<Mutex> publishGuard(g_DemandPagePublishLock);
  if (va.isMapped(reinterpret_cast<void*>(v))) {
    releaseDemandPage(p);
    return true;
  }

  if (!va.map(p, reinterpret_cast<void*>(v),
              VirtualAddressSpace::Write | VirtualAddressSpace::Execute)) {
    releaseDemandPage(p);
    WARNING("IMAGE: map() failed in ElfImage::trap(): vaddr: " << v);
    return false;
  }

  return true;
}

bool DynamicLinker::trap(uintptr_t address) {
  Elf* pElf = 0;
  uintptr_t offset = 0;
  uintptr_t buffer = 0;
  size_t size = 0;

  if (address >= m_ProgramStart && address < m_ProgramStart + m_ProgramSize) {
    pElf = m_pProgramElf;
    offset = 0;
    buffer = m_ProgramBuffer;
    size = m_ProgramSize;
  } else {
    for (Tree<uintptr_t, SharedObject*>::Iterator it = m_Objects.begin(); it != m_Objects.end();
         it++) {
      SharedObject* pSo = it.value();

      // Totally pedantic
      EMIT_IF(ADDITIONAL_CHECKS) {
        if (!pSo) {
          ERROR("A null shared object was in the object list.");
          continue;
        }
      }

      if (address >= pSo->address && address < pSo->address + pSo->size) {
        pElf = pSo->elf;
        offset = pSo->address;
        buffer = pSo->buffer;
        size = pSo->size;
        break;
      }
    }
  }

  if (!pElf)
    return false;

  return loadDemandPage(pElf, buffer, size, offset, m_pProgramElf->getSymbolTable(), address);
}

uintptr_t DynamicLinker::resolve(String name) {
  return m_pProgramElf->getSymbolTable()->lookup(name, m_pProgramElf);
}

DLTrapHandler::DLTrapHandler() {
  const bool registered = PageFaultHandler::instance().registerHandler(this);
  assert(registered);
}

DLTrapHandler::~DLTrapHandler() {
  const bool unregistered = PageFaultHandler::instance().unregisterHandler(this);
  assert(unregistered);
}

bool DLTrapHandler::trap(InterruptState& state, uintptr_t address, bool bIsWrite,
                         bool bWasPresent) {
  if (bWasPresent) {
    return false;
  }

  DynamicLinker* pL = Processor::information().getCurrentThread()->getParent()->getLinker();
  if (!pL)
    return false;
  return pL->trap(address);
}

static bool init() {
#if defined(PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS) && \
    (!defined(PEDIGREE_HOSTED_DARWIN) || !PEDIGREE_HOSTED_DARWIN)
  if (!runHostedPageContentRegressions()) {
    return false;
  }
#endif
  KernelCoreSyscallManager::instance().registerSyscall(KernelCoreSyscallManager::link,
                                                       &DynamicLinker::resolvePlt);
  return true;
}

static void destroy() {}

MODULE_INFO_NON_UNLOADABLE("linker", &init, &destroy, "vfs");

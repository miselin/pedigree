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

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/linker/SymbolTable.h"
#if THREADS
#include "pedigree/kernel/process/Scheduler.h"
#endif
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryCount.h"
#include "pedigree/kernel/utilities/MemoryTracing.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

KernelElf KernelElf::m_Instance;

// Define to dump each module's dependencies in the serial log.
#define DUMP_DEPENDENCIES 1

// Define to 1 to load modules using threads.
#define THREADED_MODULE_LOADING 0

/**
 * Extend the given pointer by adding its canonical prefix again.
 * This is because in the conversion to a 32-bit object, we manage to lose
 * the prefix (as all addresses get truncated to 32 bits)
 */

#define EXTENSION_ADDEND 0xFFFFFFFF00000000ULL

template <class T>
static T* extend(T* p) {
  EMIT_IF(X86_COMMON && !BITS_32) {
    uintptr_t u = reinterpret_cast<uintptr_t>(p);
    if (u < EXTENSION_ADDEND)
      u += EXTENSION_ADDEND;
    return reinterpret_cast<T*>(u);
  }

  return p;
}

template <class T>
static uintptr_t extend(T p) {
  EMIT_IF(X86_COMMON && !BITS_32) {
    // Must assign to a possibly-larger type before arithmetic.
    uintptr_t u = p;
    if (u < EXTENSION_ADDEND)
      u += EXTENSION_ADDEND;
    return u;
  }

  return p;
}

template <class T>
static T* retract(T* p) {
  EMIT_IF(X86_COMMON && !BITS_32) {
    uintptr_t u = reinterpret_cast<uintptr_t>(p);
    if (u >= EXTENSION_ADDEND)
      u -= EXTENSION_ADDEND;
    return reinterpret_cast<T*>(u);
  }

  return p;
}

template <class T>
static uintptr_t retract(T p) {
  EMIT_IF(X86_COMMON && !BITS_32) {
    // Must assign to a possibly-larger type before arithmetic.
    uintptr_t u = p;
    if (u >= EXTENSION_ADDEND)
      u -= EXTENSION_ADDEND;
    return u;
  }

  return p;
}

bool KernelElf::initialise(const BootstrapStruct_t& pBootstrap) {
  // Do we even have section headers to peek at?
  if (pBootstrap.getSectionHeaderCount() == 0) {
    WARNING("No ELF object available to extract symbol table from.");

    // Hosted dynamic modules can resolve exported kernel symbols from the
    // process linker. Darwin kernels are Mach-O, so there is intentionally
    // no ELF image from which to import a second symbol table.
    return (STATIC_DRIVERS == 1) || (HOSTED == 1);
  }

  EMIT_IF(X86_COMMON) {
    PhysicalMemoryManager& physicalMemoryManager = PhysicalMemoryManager::instance();
    size_t pageSz = PhysicalMemoryManager::getPageSize();

    m_AdditionalSectionHeaders = new MemoryRegion("Kernel ELF Section Headers");

    // Map in section headers.
    size_t sectionHeadersLength =
        pBootstrap.getSectionHeaderCount() * pBootstrap.getSectionHeaderEntrySize();
    if ((sectionHeadersLength % pageSz) > 0) {
      sectionHeadersLength += pageSz;
    }
    if (physicalMemoryManager.allocateRegion(
            *m_AdditionalSectionHeaders, sectionHeadersLength / pageSz,
            PhysicalMemoryManager::continuous,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write,
            pBootstrap.getSectionHeaders()) == false) {
      ERROR(
          "KernelElf::initialise failed to allocate for "
          "m_AdditionalSectionHeaders");
      return false;
    }

    // Determine the layout of the contents of non-code sections.
    physical_uintptr_t start = ~0;
    physical_uintptr_t end = 0;
    for (size_t i = 1; i < pBootstrap.getSectionHeaderCount(); i++) {
      // Force 32-bit section header type as we are a 32-bit ELF object
      // even on 64-bit targets.
      uintptr_t shdr_addr =
          pBootstrap.getSectionHeaders() + i * pBootstrap.getSectionHeaderEntrySize();
      Elf32SectionHeader_t* pSh =
          m_AdditionalSectionHeaders->convertPhysicalPointer<Elf32SectionHeader_t>(shdr_addr);

      if ((pSh->flags & SHF_ALLOC) != SHF_ALLOC) {
        if (pSh->addr <= start) {
          start = pSh->addr;
        }

        if ((pSh->addr + pSh->size) >= end) {
          end = pSh->addr + pSh->size;
        }
      }
    }

    // Is there an overlap between headers and section data?
    if ((start & ~(pageSz - 1)) == (pBootstrap.getSectionHeaders() & ~(pageSz - 1))) {
      // Yes, there is. Point the section headers MemoryRegion to the
      // Contents.
      delete m_AdditionalSectionHeaders;
      m_AdditionalSectionHeaders = &m_AdditionalSectionContents;
    }

    // Map in all non-alloc sections.
    uintptr_t alignedStart = start & ~(pageSz - 1);
    uintptr_t allocSize = end - alignedStart;
    if ((allocSize % pageSz) > 0) {
      allocSize += pageSz;
    }
    size_t additionalContentsPages = allocSize / pageSz;
    if (physicalMemoryManager.allocateRegion(
            m_AdditionalSectionContents, additionalContentsPages, PhysicalMemoryManager::continuous,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write, start) == false) {
      ERROR(
          "KernelElf::initialise failed to allocate for "
          "m_AdditionalSectionContents");
      return false;
    }
  }

  // Get the string table
  uintptr_t stringTableHeader =
      (pBootstrap.getSectionHeaders() +
       pBootstrap.getSectionHeaderStringTableIndex() * pBootstrap.getSectionHeaderEntrySize());
  KernelElfSectionHeader_t* stringTableShdr =
      reinterpret_cast<KernelElfSectionHeader_t*>(stringTableHeader);

  const char* tmpStringTable;

  EMIT_IF(X86_COMMON) {
    tmpStringTable =
        m_AdditionalSectionContents.convertPhysicalPointer<const char>(stringTableShdr->addr);
  }
  else {
    tmpStringTable = reinterpret_cast<const char*>(stringTableShdr->addr);
  }

  // Search for the symbol/string table and adjust sections
  for (size_t i = 1; i < pBootstrap.getSectionHeaderCount(); i++) {
    uintptr_t shdr_addr =
        pBootstrap.getSectionHeaders() + i * pBootstrap.getSectionHeaderEntrySize();

    // The expanded x86 header must live until the section has been inspected.
    ElfSectionHeader_t sh;
    ElfSectionHeader_t* pSh = 0;

    EMIT_IF(X86_COMMON) {
      KernelElfSectionHeader_t* pTruncatedSh =
          m_AdditionalSectionHeaders->convertPhysicalPointer<KernelElfSectionHeader_t>(shdr_addr);

      // Copy into larger format for analysis
      sh.name = pTruncatedSh->name;
      sh.type = pTruncatedSh->type;
      sh.flags = pTruncatedSh->flags;
      sh.addr = pTruncatedSh->addr;
      sh.offset = pTruncatedSh->offset;
      sh.size = pTruncatedSh->size;
      sh.link = pTruncatedSh->link;
      sh.info = pTruncatedSh->info;
      sh.addralign = pTruncatedSh->addralign;
      sh.entsize = pTruncatedSh->entsize;

      pSh = &sh;

      // Adjust the section
      if ((pSh->flags & SHF_ALLOC) != SHF_ALLOC) {
        NOTICE("Converting shdr " << Hex << pSh->addr << " -> " << pSh->addr + pSh->size);
        pSh->addr = reinterpret_cast<uintptr_t>(
            m_AdditionalSectionContents.convertPhysicalPointer<void>(pSh->addr));
        NOTICE(" to " << Hex << pSh->addr);
        pSh->offset = pSh->addr;
      }
    }
    else {
      pSh = reinterpret_cast<ElfSectionHeader_t*>(shdr_addr);
    }

    // Save the symbol/string table
    const char* pStr = tmpStringTable + pSh->name;

    if (pSh->type == SHT_SYMTAB) {
      m_pSymbolTable = reinterpret_cast<KernelElfSymbol_t*>(pSh->addr);
      m_nSymbolTableSize = pSh->size;
    } else if (!StringCompare(pStr, ".strtab")) {
      m_pStringTable = reinterpret_cast<char*>(pSh->addr);
    } else if (!StringCompare(pStr, ".shstrtab")) {
      m_pShstrtab = reinterpret_cast<char*>(pSh->addr);
    } else if (!StringCompare(pStr, ".debug_frame")) {
      m_pDebugTable = reinterpret_cast<uint32_t*>(pSh->addr);
      m_nDebugTableSize = pSh->size;
    }
  }

  // Initialise remaining member variables
  m_pSectionHeaders = reinterpret_cast<KernelElfSectionHeader_t*>(pBootstrap.getSectionHeaders());
  m_nSectionHeaders = pBootstrap.getSectionHeaderCount();

  if (DEBUGGER && m_pSymbolTable && m_pStringTable) {
    KernelElfSymbol_t* pSymbol = m_pSymbolTable;

    const char* pStrtab = reinterpret_cast<const char*>(m_pStringTable);

    // quick pass to preallocate for the symbol table
    size_t numLocal = 0;
    size_t numWeak = 0;
    size_t numGlobal = 0;
    for (size_t i = 0; i < m_nSymbolTableSize / sizeof(*pSymbol); i++) {
      switch (ST_BIND(m_pSymbolTable[i].info)) {
        case STB_LOCAL:
          ++numLocal;
          break;
        case STB_GLOBAL:
          ++numGlobal;
          break;
        case STB_WEAK:
          ++numWeak;
          break;
        default:
          ++numGlobal;
      }
    }

    NOTICE("KERNELELF: preallocating symbol table with "
           << numGlobal << " global " << numWeak << " weak and " << numLocal << " local symbols.");
    m_SymbolTable.preallocate(numGlobal, numWeak, this, numLocal);

    for (size_t i = 1; i < m_nSymbolTableSize / sizeof(*pSymbol); i++) {
      const char* pStr = 0;

      if (ST_TYPE(pSymbol->info) == STT_SECTION) {
        // Section type - the name will be the name of the section
        // header it refers to.
        KernelElfSectionHeader_t* pSh = &m_pSectionHeaders[pSymbol->shndx];
        // If it's not allocated, it's a link-once-only section that we
        // can ignore.
        if (!(pSh->flags & SHF_ALLOC)) {
          pSymbol++;
          continue;
        }
        // Grab the shstrtab
        pStr = reinterpret_cast<const char*>(m_pShstrtab) + pSh->name;
      } else {
        pStr = pStrtab + pSymbol->name;
      }

      // Insert the symbol into the symbol table.
      SymbolTable::Binding binding;
      switch (ST_BIND(pSymbol->info)) {
        case STB_LOCAL:
          binding = SymbolTable::Local;
          break;
        case STB_GLOBAL:
          binding = SymbolTable::Global;
          break;
        case STB_WEAK:
          binding = SymbolTable::Weak;
          break;
        default:
          binding = SymbolTable::Global;
      }

      EMIT_IF(!TRACK_HIDDEN_SYMBOLS) {
        // Don't insert hidden symbols to the main symbol table.
        if (pSymbol->other == STV_HIDDEN) {
          ++pSymbol;
          continue;
        }
      }

      if (pStr && (*pStr != '\0')) {
        EMIT_IF(HOSTED) {
          // If name starts with __wrap_, rewrite it in flight as it's
          // a wrapped symbol on hosted systems.
          if (!StringCompareN(pStr, "__wrap_", 7)) {
            pStr += 7;
          }
        }

        m_SymbolTable.insert(String(pStr), binding, this, extend(pSymbol->value));
      }
      pSymbol++;
    }
  }

  return true;
}

KernelElf::KernelElf()
    : m_AdditionalSectionContents("Kernel ELF Section Data"),
      m_AdditionalSectionHeaders(0),
      m_Modules(),
      m_ModuleAllocator(),
      m_pSectionHeaders(0),
      m_pSymbolTable(0),
      m_ModuleAdjustmentLock(false),
      m_ModuleShutdown(false),
      m_ModuleShutdownStatus(ShutdownOpen),
      m_ModuleLoading(false),
      m_UnloadingModule(nullptr),
      m_ModuleExecutions(0),
      m_ModuleExecutionPasses(0),
      m_TerminalQuiesceOwner(nullptr),
      m_TerminalQuiesceHook(nullptr),
      m_TerminalQuiesceStatus(QuiesceOpen),
      m_InitModule(nullptr) {}

KernelElf::~KernelElf() {
  delete m_AdditionalSectionHeaders;

  // All of these non-alloc sections are just pointers into the loaded kernel
  // ELF, which is not heap allocated. In normal Elf objects these are
  // allocated and then copied into. Not so here.
  m_pSymbolTable = nullptr;
  m_pStringTable = nullptr;
  m_pShstrtab = nullptr;
  m_pDebugTable = nullptr;
}

bool KernelElf::beginModuleLoad() {
  lockModules();
  const bool admitted = !m_ModuleShutdown && !m_ModuleLoading && !m_UnloadingModule;
  if (admitted) {
    m_ModuleLoading = true;
  }
  unlockModules();
  return admitted;
}

void KernelElf::finishModuleLoad() {
  lockModules();
  m_ModuleLoading = false;
  unlockModules();
}

Module* KernelElf::loadModule(uint8_t* pModule, size_t len, bool silent) {
  MemoryCount guard(__PRETTY_FUNCTION__);

  if (!beginModuleLoad()) {
    WARNING("KERNELELF: Rejecting concurrent module load or load during shutdown");
    return nullptr;
  }

  // The module memory allocator requires dynamic memory - this isn't
  // initialised until after our constructor is called, so check here if we've
  // loaded any modules yet. If not, we can initialise our memory allocator.
  if (m_Modules.count() == 0) {
    uintptr_t start = VirtualAddressSpace::getKernelAddressSpace().getKernelModulesStart();
    uintptr_t end = VirtualAddressSpace::getKernelAddressSpace().getKernelModulesEnd();
    m_ModuleAllocator.free(start, end - start);
  }

  Module* module = new Module;

  module->elf = new Elf();
  module->buffer = pModule;
  module->buflen = len;

  if (!module->elf->create(pModule, len)) {
    FATAL("Module load failed (1)");
    delete module;
    finishModuleLoad();
    return 0;
  }

  if (!module->elf->loadModule(pModule, len, module->loadBase, module->loadSize, &m_SymbolTable)) {
    FATAL("Module load failed (2)");
    delete module;
    finishModuleLoad();
    return 0;
  }

  //  Load the module debug table (if any)
  if (module->elf->debugFrameTableLength()) {
    size_t sz = m_nDebugTableSize + module->elf->debugFrameTableLength();
    if (sz % sizeof(uint32_t))
      sz += sizeof(uint32_t);
    uint32_t* pDebug = new uint32_t[sz / sizeof(uint32_t)];
    if (UNLIKELY(!pDebug)) {
      ERROR("Could not load module debug frame information.");
    } else {
      MemoryCopy(pDebug, m_pDebugTable, m_nDebugTableSize);
      MemoryCopy(pDebug + m_nDebugTableSize,
                 reinterpret_cast<const void*>(module->elf->debugFrameTable()),
                 module->elf->debugFrameTableLength());
      m_nDebugTableSize += module->elf->debugFrameTableLength();
      m_pDebugTable = pDebug;
      NOTICE("Added debug module debug frame information.");
    }
  }

  // Look up the module's name and entry/exit functions, and dependency list.
  const char** pName = reinterpret_cast<const char**>(module->elf->lookupSymbol("g_pModuleName"));
  if ((!pName) || (!*pName)) {
    ERROR("KERNELELF: Hit an invalid module, ignoring");
    finishModuleLoad();
    return 0;
  }
  module->name.assign(rebase(module, *pName));
  module->elf->setName(module->name);
  auto entryPoint = *reinterpret_cast<bool (**)()>(module->elf->lookupSymbol("g_pModuleEntry"));
  auto exitPoint = *reinterpret_cast<void (**)()>(module->elf->lookupSymbol("g_pModuleExit"));
  // Readjust entry/exit functions for the loaded module if needed
  if (entryPoint) {
    entryPoint = adjust_pointer(entryPoint, module->loadBase);
  }
  if (exitPoint) {
    exitPoint = adjust_pointer(exitPoint, module->loadBase);
  }
  module->entry = entryPoint;
  module->exit = exitPoint;
  bool* unloadable =
      reinterpret_cast<bool*>(module->elf->lookupSymbol("g_bModuleUnloadable"));
  module->unloadable = unloadable ? *unloadable : true;
  bool* runtimeUnloadable =
      reinterpret_cast<bool*>(module->elf->lookupSymbol("g_bModuleRuntimeUnloadable"));
  // Older modules predate explicit-unload policy metadata.
  module->runtimeUnloadable = runtimeUnloadable ? *runtimeUnloadable : true;
  module->depends = reinterpret_cast<const char**>(module->elf->lookupSymbol("g_pDepends"));
  module->depends_opt =
      reinterpret_cast<const char**>(module->elf->lookupSymbol("g_pOptionalDepends"));
  DEBUG_LOG("KERNELELF: Preloaded module " << module->name << " at " << Hex << module->loadBase
                                           << " to " << (module->loadBase + module->loadSize));
  DEBUG_LOG("KERNELELF: Module " << module->name << " consumes " << Dec << (module->loadSize / 1024)
                                 << Hex << "K of memory");

  EMIT_IF(DUMP_DEPENDENCIES) {
    size_t i = 0;
    while (module->depends_opt && rebase(module, module->depends_opt)[i]) {
      DEBUG_LOG("KERNELELF: Module " << module->name << " optdepends on "
                                     << rebase(module, rebase(module, module->depends_opt)[i]));
      ++i;
    }

    i = 0;
    while (module->depends && rebase(module, module->depends)[i]) {
      DEBUG_LOG("KERNELELF: Module " << module->name << " depends on "
                                     << rebase(module, rebase(module, module->depends)[i]));
      ++i;
    }
  }

  EMIT_IF(MEMORY_TRACING) {
    traceMetadata(NormalStaticString(module->name), reinterpret_cast<void*>(module->loadBase),
                  reinterpret_cast<void*>(module->loadBase + module->loadSize));
  }

  const bool initModule = !StringCompare(module->name.cstr(), "init");
  lockModules();
  if (initModule) {
    m_InitModule = module;
  } else {
    module->status = Module::Preloaded;
    m_Modules.pushBack(module);
    ++module->progressCredits;
    ++g_BootProgressCurrent;
  }
  unlockModules();

  if (!initModule) {
    if (g_BootProgressUpdate && !silent)
      g_BootProgressUpdate("moduleload");
  }
  finishModuleLoad();

  return module;
}

void KernelElf::executeModules(bool silent, bool progress) {
  lockModules();
  if (m_ModuleShutdown) {
    unlockModules();
    WARNING("KERNELELF: Rejecting module execution after shutdown began");
    return;
  }
  ++m_ModuleExecutionPasses;
  const size_t moduleCount = m_Modules.count();
  unlockModules();
  NOTICE("KERNELELF: executing " << moduleCount << " modules...");

  while (true) {
    Module* module = nullptr;
    bool executing = false;
    bool updateProgress = false;
    lockModules();
    if (m_ModuleShutdown) {
      unlockModules();
      break;
    }
    if (m_ModuleLoading) {
      unlockModules();
#if THREADS
      Scheduler::instance().yield();
      continue;
#else
      FATAL("KERNELELF: Concurrent module load without scheduler support");
      break;
#endif
    }
    executing = m_ModuleExecutions != 0;
    for (auto candidate : m_Modules) {
      if (candidate->isPending() && moduleDependenciesSatisfiedLocked(candidate)) {
        // Eligibility and the Preloaded -> Executing transition are one
        // claim. An unload owner can therefore observe either state, but can
        // never unmap a module between the dependency check and its launch.
        candidate->status = Module::Executing;
        ++m_ModuleExecutions;
        if (progress) {
          ++candidate->progressCredits;
          ++g_BootProgressCurrent;
          updateProgress = true;
        }
        module = candidate;
        break;
      }
    }
    unlockModules();

    if (!module) {
      if (executing) {
#if THREADS
        Scheduler::instance().yield();
        continue;
#endif
      }
      break;
    }

    if (updateProgress && g_BootProgressUpdate && !silent) {
      g_BootProgressUpdate("moduleexec");
    }
    executeModule(module);
  }

  lockModules();
  if (!m_ModuleExecutionPasses) {
    unlockModules();
    FATAL("KERNELELF: Module execution-pass accounting underflow");
    return;
  }
  --m_ModuleExecutionPasses;
  unlockModules();
}

Module* KernelElf::loadModule(struct ModuleInfo* info, bool silent) {
  /// \todo rewrite to the new module dependency logic
  if (!beginModuleLoad()) {
    WARNING("KERNELELF: Rejecting concurrent static module load or load during shutdown");
    return nullptr;
  }

  Module* module = new Module;

  module->buffer = 0;
  module->buflen = 0;

  module->name.assign(info->name);
  module->entry = info->entry;
  module->exit = info->exit;
  module->unloadable = info->unloadable;
  module->runtimeUnloadable = info->runtimeUnloadable;
  module->depends = info->dependencies;
  module->depends_opt = info->opt_dependencies;
  DEBUG_LOG("KERNELELF: Preloaded module " << module->name);

  EMIT_IF(DUMP_DEPENDENCIES) {
    size_t i = 0;
    while (module->depends_opt && rebase(module, module->depends_opt)[i]) {
      DEBUG_LOG("KERNELELF: Module " << module->name << " optdepends on "
                                     << rebase(module, rebase(module, module->depends_opt)[i]));
      ++i;
    }

    i = 0;
    while (module->depends && rebase(module, module->depends)[i]) {
      DEBUG_LOG("KERNELELF: Module " << module->name << " depends on "
                                     << rebase(module, rebase(module, module->depends)[i]));
      ++i;
    }
  }

  EMIT_IF(MEMORY_TRACING) {
    traceMetadata(NormalStaticString(module->name), reinterpret_cast<void*>(module->loadBase),
                  reinterpret_cast<void*>(module->loadBase + module->loadSize));
  }

  const bool initModule = !StringCompare(module->name.cstr(), "init");
  lockModules();
  if (initModule) {
    m_InitModule = module;
  } else {
    module->status = Module::Preloaded;
    m_Modules.pushBack(module);
    ++module->progressCredits;
    ++g_BootProgressCurrent;
  }
  unlockModules();

  if (!initModule) {
    if (g_BootProgressUpdate && !silent)
      g_BootProgressUpdate("moduleload");
  }
  finishModuleLoad();

  return module;
}

bool KernelElf::moduleRegisteredLocked(Module* module) const {
  for (auto registered : m_Modules) {
    if (registered == module) {
      return true;
    }
  }
  return false;
}

Module* KernelElf::findModuleByName(const Vector<Module*>& modules, const String& name) {
  for (auto module : modules) {
    if (module->name == name) {
      return module;
    }
  }
  return nullptr;
}

bool KernelElf::moduleDependsOn(Module* consumer, Module* provider) {
  const char** dependencyLists[] = {consumer->depends, consumer->depends_opt};
  for (const char** dependencies : dependencyLists) {
    if (!dependencies) {
      continue;
    }

    const char** rebasedDependencies = rebase(consumer, dependencies);
    for (size_t i = 0; rebasedDependencies[i]; ++i) {
      const char* dependency = rebase(consumer, rebasedDependencies[i]);
      if (!StringCompare(dependency, provider->name.cstr())) {
        return true;
      }
    }
  }
  return false;
}

bool KernelElf::hasLiveDependent(const Vector<Module*>& modules, Module* provider) {
  for (auto consumer : modules) {
    if (consumer == provider || consumer->unloadComplete || consumer->status == Module::Unloaded) {
      continue;
    }
    if (moduleDependsOn(consumer, provider)) {
      return true;
    }
  }
  return false;
}

Module* KernelElf::findUnloadCandidate(const Vector<Module*>& modules, bool& waiting) {
  waiting = false;
  for (auto module : modules) {
    if (module->isExecuting() || module->isUnloading()) {
      waiting = true;
      return nullptr;
    }
  }

  for (auto module : modules) {
    if (module->unloadComplete || module->status == Module::Unloaded || !module->unloadable) {
      continue;
    }
    if (!hasLiveDependent(modules, module)) {
      return module;
    }
  }
  return nullptr;
}

KernelElf::ModuleUnloadClaim KernelElf::claimModuleUnloadLocked(
    Module* module, bool allowShutdown, bool requireMembership, bool enforceDependencies,
    bool& wasFailed, bool& runLifecycle) {
  wasFailed = false;
  runLifecycle = false;

  if (!module || (requireMembership && !moduleRegisteredLocked(module))) {
    return UnloadUnknown;
  }
  if (module->unloadComplete || module->status == Module::Unloaded) {
    return UnloadComplete;
  }
  if (m_ModuleShutdown && !allowShutdown) {
    return UnloadShutdown;
  }
  if (!module->unloadable) {
    return UnloadPinned;
  }
  if (!allowShutdown && !module->runtimeUnloadable) {
    return UnloadRuntimePinned;
  }
  if (module == m_TerminalQuiesceOwner && m_TerminalQuiesceHook) {
    return UnloadBusy;
  }
  if (m_ModuleLoading || m_UnloadingModule) {
    return UnloadBusy;
  }
  if (requireMembership) {
    for (auto registered : m_Modules) {
      if (registered->isExecuting()) {
        return UnloadBusy;
      }
    }
  }
  if (enforceDependencies && hasLiveDependent(m_Modules, module)) {
    return UnloadDependedOn;
  }

  wasFailed = module->isFailed();
  runLifecycle = module->isActive() || wasFailed;
  module->status = Module::Unloading;
  m_UnloadingModule = module;
  return UnloadClaimed;
}

void KernelElf::finishClaimedUnload(Module* module, bool wasFailed) {
  lockModules();
  if (m_UnloadingModule == module) {
    module->unloadComplete = true;
    module->status = wasFailed ? Module::Failed : Module::Unloaded;
    m_UnloadingModule = nullptr;
  }
  unlockModules();
}

bool KernelElf::completeUnloadAttempt(Module* module, ModuleUnloadClaim claim, bool wasFailed,
                                      bool runLifecycle, bool silent, bool progress) {
  switch (claim) {
    case UnloadComplete:
      return true;
    case UnloadBusy:
      WARNING("KERNELELF: Module unload is busy; retry later");
      return false;
    case UnloadPinned:
      WARNING("KERNELELF: Module " << module->name << " is pinned and cannot be unloaded");
      return false;
    case UnloadRuntimePinned:
      WARNING("KERNELELF: Module " << module->name
                                   << " cannot be unloaded while the system is running");
      return false;
    case UnloadDependedOn:
      WARNING("KERNELELF: Module " << module->name << " still has a live dependent");
      return false;
    case UnloadShutdown:
      WARNING("KERNELELF: Module unload rejected after shutdown began");
      return false;
    case UnloadUnknown:
      ERROR("KERNELELF: Module unload target is not registered");
      return false;
    case UnloadClaimed:
      break;
  }

  NOTICE("KERNELELF: Unloading module " << module->name);

  bool progressUpdated = false;
  if (progress) {
    lockModules();
    if (module->progressCredits) {
      --module->progressCredits;
      if (g_BootProgressCurrent) {
        --g_BootProgressCurrent;
      }
      progressUpdated = true;
    }
    unlockModules();
    if (progressUpdated && g_BootProgressUpdate && !silent)
      g_BootProgressUpdate("moduleunload");
  }

  if (runLifecycle && module->exit)
    module->exit();

  // Check for a destructors list and execute.
  // Note: static drivers have their ctors/dtors all shared.
  EMIT_IF(!STATIC_DRIVERS) {
    uintptr_t startDtors = 0;
    uintptr_t endDtors = 0;
    if (runLifecycle && module->elf) {
      startDtors = module->elf->lookupSymbol("start_dtors");
      endDtors = module->elf->lookupSymbol("end_dtors");
    }

    if (startDtors && endDtors) {
      uintptr_t* iterator = reinterpret_cast<uintptr_t*>(startDtors);
      while (iterator < reinterpret_cast<uintptr_t*>(endDtors)) {
        if (static_cast<intptr_t>(*iterator) == -1) {
          ++iterator;
          continue;
        } else if ((*iterator) == 0) {
          // End of table.
          break;
        }

        uintptr_t dtor = *iterator;
        void (*fp)(void) = reinterpret_cast<void (*)(void)>(dtor);
        fp();
        iterator++;
      }
    }

    if (module->elf) {
      m_SymbolTable.eraseByElf(module->elf);
    }
  }

  progressUpdated = false;
  if (progress) {
    lockModules();
    if (module->progressCredits) {
      --module->progressCredits;
      if (g_BootProgressCurrent) {
        --g_BootProgressCurrent;
      }
      progressUpdated = true;
    }
    unlockModules();
    if (progressUpdated && g_BootProgressUpdate && !silent)
      g_BootProgressUpdate("moduleunloaded");
  }

  NOTICE("KERNELELF: Module " << module->name << " unloaded.");

  EMIT_IF(!STATIC_DRIVERS) {
    size_t pageSz = PhysicalMemoryManager::getPageSize();
    size_t numPages = (module->loadSize / pageSz) + (module->loadSize % pageSz ? 1 : 0);

    // Unmap!
    VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
    for (size_t i = 0; i < numPages; i++) {
      void* unmapAddr = reinterpret_cast<void*>(module->loadBase + (i * pageSz));
      if (va.isMapped(unmapAddr)) {
        // Unmap the virtual address
        physical_uintptr_t phys = 0;
        size_t flags = 0;
        va.getMapping(unmapAddr, phys, flags);
        va.unmap(unmapAddr);

        // Free the physical page
        PhysicalMemoryManager::instance().freePage(phys);
      }
    }

    m_ModuleAllocator.free(module->loadBase, module->loadSize);
  }

  delete module->elf;
  module->elf = nullptr;

  finishClaimedUnload(module, wasFailed);
  return true;
}

bool KernelElf::unloadModule(const char* name, bool silent, bool progress) {
  String findName(name);
  Module* module = nullptr;
  ModuleUnloadClaim claim = UnloadUnknown;
  bool wasFailed = false;
  bool runLifecycle = false;

  lockModules();
  module = findModuleByName(m_Modules, findName);
  if (module) {
    claim = claimModuleUnloadLocked(module, false, true, true, wasFailed, runLifecycle);
  }
  unlockModules();

  if (!module) {
    ERROR("KERNELELF: Module " << name << " not found");
    return false;
  }
  return completeUnloadAttempt(module, claim, wasFailed, runLifecycle, silent, progress);
}

bool KernelElf::unloadModule(Module* module, bool silent, bool progress) {
  bool wasFailed = false;
  bool runLifecycle = false;
  lockModules();
  const ModuleUnloadClaim claim =
      claimModuleUnloadLocked(module, false, true, true, wasFailed, runLifecycle);
  unlockModules();
  return completeUnloadAttempt(module, claim, wasFailed, runLifecycle, silent, progress);
}

bool KernelElf::registerTerminalQuiesce(ModuleEntry ownerEntry, TerminalQuiesceHook hook) {
  if (!ownerEntry || !hook) {
    return false;
  }

  lockModules();
  if (m_ModuleShutdown || m_TerminalQuiesceStatus != QuiesceOpen) {
    unlockModules();
    return false;
  }
  if (m_TerminalQuiesceHook) {
    const bool alreadyRegistered = m_TerminalQuiesceOwner &&
                                   m_TerminalQuiesceOwner->entry == ownerEntry &&
                                   m_TerminalQuiesceHook == hook;
    unlockModules();
    return alreadyRegistered;
  }

  Module* owner = nullptr;
  for (auto module : m_Modules) {
    if (module->entry != ownerEntry || !(module->isExecuting() || module->isActive())) {
      continue;
    }
    if (owner) {
      unlockModules();
      return false;
    }
    owner = module;
  }
  if (!owner) {
    unlockModules();
    return false;
  }

  m_TerminalQuiesceOwner = owner;
  m_TerminalQuiesceHook = hook;
  unlockModules();
  return true;
}

bool KernelElf::unregisterTerminalQuiesce(ModuleEntry ownerEntry, TerminalQuiesceHook hook) {
  lockModules();
  if (!m_TerminalQuiesceHook) {
    unlockModules();
    return true;
  }
  if (!m_TerminalQuiesceOwner || m_TerminalQuiesceOwner->entry != ownerEntry ||
      m_TerminalQuiesceHook != hook || m_TerminalQuiesceStatus != QuiesceOpen) {
    unlockModules();
    return false;
  }

  m_TerminalQuiesceOwner = nullptr;
  m_TerminalQuiesceHook = nullptr;
  unlockModules();
  return true;
}

void KernelElf::unloadModules() {
  while (true) {
    bool waiting = false;
    bool failed = false;

    lockModules();
    if (m_ModuleShutdownStatus == ShutdownOpen) {
      m_ModuleShutdown = true;
      m_ModuleShutdownStatus = ShutdownRunning;
      unlockModules();
      break;
    }
    if (m_ModuleShutdownStatus == ShutdownComplete) {
      unlockModules();
      return;
    }
    failed = m_ModuleShutdownStatus == ShutdownFailed;
    waiting = m_ModuleShutdownStatus == ShutdownRunning;
    unlockModules();

    if (failed) {
      FATAL("KERNELELF: Module shutdown previously failed");
      return;
    }
    if (waiting) {
#if THREADS
      Scheduler::instance().yield();
      continue;
#else
      FATAL("KERNELELF: Concurrent module shutdown without scheduler support");
      return;
#endif
    }
  }

  if (g_BootProgressUpdate) {
    g_BootProgressUpdate("unload");
  }

  while (true) {
    TerminalQuiesceHook hook = nullptr;
    bool waiting = false;
    bool failed = false;

    lockModules();
    if (m_TerminalQuiesceStatus == QuiesceComplete) {
      unlockModules();
      break;
    }
    if (m_TerminalQuiesceStatus == QuiesceFailed) {
      failed = true;
    } else if (m_TerminalQuiesceStatus == QuiesceInvoking || m_ModuleLoading || m_UnloadingModule ||
               m_ModuleExecutions || m_ModuleExecutionPasses) {
      waiting = true;
    } else {
      m_TerminalQuiesceStatus = QuiesceInvoking;
      hook = m_TerminalQuiesceHook;
      m_TerminalQuiesceOwner = nullptr;
      m_TerminalQuiesceHook = nullptr;
      if (!hook) {
        m_TerminalQuiesceStatus = QuiesceComplete;
      }
    }
    unlockModules();

    if (failed) {
      lockModules();
      m_ModuleShutdownStatus = ShutdownFailed;
      unlockModules();
      FATAL("KERNELELF: Terminal module quiesce failed; refusing to unload modules");
      return;
    }
    if (hook) {
      const bool quiesced = hook();
      lockModules();
      m_TerminalQuiesceStatus = quiesced ? QuiesceComplete : QuiesceFailed;
      unlockModules();
      if (!quiesced) {
        lockModules();
        m_ModuleShutdownStatus = ShutdownFailed;
        unlockModules();
        FATAL("KERNELELF: Terminal module quiesce failed; refusing to unload modules");
        return;
      }
      break;
    }
    if (!waiting) {
      break;
    }
#if THREADS
    Scheduler::instance().yield();
#else
    lockModules();
    m_ModuleShutdownStatus = ShutdownFailed;
    unlockModules();
    FATAL("KERNELELF: Terminal quiesce encountered an in-flight module operation");
    return;
#endif
  }

  while (true) {
    Module* candidate = nullptr;
    ModuleUnloadClaim claim = UnloadBusy;
    bool wasFailed = false;
    bool runLifecycle = false;
    bool waiting = false;

    lockModules();
    if (m_ModuleLoading || m_UnloadingModule || m_ModuleExecutions || m_ModuleExecutionPasses) {
      waiting = true;
    } else {
      if (m_InitModule) {
        // The init image is held aside until userspace launch, but a
        // controlled shutdown can begin before that point. Transfer it only
        // after any admitted load has published its result.
        m_InitModule->status = Module::Preloaded;
        m_Modules.pushBack(m_InitModule);
        m_InitModule = nullptr;
      }
      candidate = findUnloadCandidate(m_Modules, waiting);
      if (candidate) {
        claim = claimModuleUnloadLocked(candidate, true, true, false, wasFailed, runLifecycle);
      }
    }
    unlockModules();

    if (candidate && claim == UnloadClaimed) {
      completeUnloadAttempt(candidate, claim, wasFailed, runLifecycle, false, false);
      continue;
    }
    if (waiting || (candidate && claim == UnloadBusy)) {
#if THREADS
      Scheduler::instance().yield();
      continue;
#else
      lockModules();
      m_ModuleShutdownStatus = ShutdownFailed;
      unlockModules();
      FATAL("KERNELELF: Module shutdown encountered an in-flight module operation");
      return;
#endif
    }
    break;
  }

  for (auto module : m_Modules) {
    if (!module->unloadComplete && module->status != Module::Unloaded) {
      if (!module->unloadable) {
        WARNING("KERNELELF: Leaving permanently pinned module " << module->name
                                                                << " mapped at shutdown");
      } else {
        WARNING("KERNELELF: Leaving module " << module->name
                                             << " mapped because its shutdown dependencies remain");
      }
    }
  }

  // Module records are tombstones for repeat/concurrent callers until this
  // terminal shutdown point. The kernel is terminating, so dropping the
  // pointer list is safer than freeing records another CPU may still name.
  lockModules();
  m_Modules.clear();
  m_ModuleShutdownStatus = ShutdownComplete;
  unlockModules();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t KernelElf::planModuleUnloadOrderForTest(Module** modules, size_t count, Module** order,
                                               size_t capacity) {
  Vector<Module*> pending;
  for (size_t i = 0; i < count; ++i) {
    pending.pushBack(modules[i]);
  }

  size_t planned = 0;
  while (true) {
    bool waiting = false;
    Module* candidate = findUnloadCandidate(pending, waiting);
    if (!candidate || waiting) {
      break;
    }
    if (planned < capacity) {
      order[planned] = candidate;
    }
    ++planned;
    candidate->unloadComplete = true;
    candidate->status = Module::Unloaded;
  }
  return planned;
}

KernelElf::TestModuleUnloadClaim KernelElf::claimModuleUnloadForTest(Module* module,
                                                                     bool allowShutdown,
                                                                     bool* lifecycle) {
  bool wasFailed = false;
  bool runLifecycle = false;
  KernelElf& kernelElf = instance();
  kernelElf.lockModules();
  const ModuleUnloadClaim claim = kernelElf.claimModuleUnloadLocked(module, allowShutdown, false,
                                                                    false, wasFailed, runLifecycle);
  kernelElf.unlockModules();
  if (lifecycle) {
    *lifecycle = runLifecycle;
  }
  return static_cast<TestModuleUnloadClaim>(claim);
}

KernelElf::TestModuleUnloadClaim KernelElf::claimNamedModuleUnloadForTest(
    Module** modules, size_t count, const char* name) {
  Vector<Module*> fixtures;
  for (size_t i = 0; i < count; ++i) {
    fixtures.pushBack(modules[i]);
  }

  KernelElf& kernelElf = instance();
  bool wasFailed = false;
  bool runLifecycle = false;
  kernelElf.lockModules();
  Module* module = findModuleByName(fixtures, String(name));
  ModuleUnloadClaim claim = UnloadUnknown;
  if (module) {
    claim = kernelElf.claimModuleUnloadLocked(module, true, false, false, wasFailed, runLifecycle);
  }
  kernelElf.unlockModules();
  return static_cast<TestModuleUnloadClaim>(claim);
}

void KernelElf::completeModuleUnloadForTest(Module* module, bool wasFailed, bool runLifecycle) {
  instance().completeUnloadAttempt(module, UnloadClaimed, wasFailed, runLifecycle, true, false);
}
#endif

bool KernelElf::moduleIsLoaded(char* name) {
  // this should hash the name and make comparisons super fast
  String compName(name);

  bool loaded = false;
  lockModules();
  for (auto module : m_Modules) {
    if (module->isLoaded() && module->name == compName) {
      loaded = true;
      break;
    }
  }
  unlockModules();
  return loaded;
}

char* KernelElf::getDependingModule(char* name) {
  char* result = nullptr;
  lockModules();
  for (auto module : m_Modules) {
    if (!module->isLoaded()) {
      // can't depend on unloaded modules - might be unmapped
      continue;
    } else if (module->depends == 0) {
      continue;
    }

    size_t i = 0;
    while (rebase(module, module->depends)[i]) {
      const char* rebased = rebase(module, rebase(module, module->depends)[i]);
      if (!StringCompare(rebased, name)) {
        result = const_cast<char*>(static_cast<const char*>(module->name));
        break;
      }

      ++i;
    }
    if (result) {
      break;
    }
  }
  unlockModules();
  return result;
}

bool KernelElf::moduleDependenciesSatisfiedLocked(Module* module) const {
  int i = 0;

  // First pass: optional dependencies.
  if (module->depends_opt) {
    while (rebase(module, module->depends_opt)[i]) {
      const char* depname = rebase(module, rebase(module, module->depends_opt)[i]);

      bool exists = false;
      bool attempted = false;
      for (auto mod : m_Modules) {
        if (!StringCompare(mod->name.cstr(), depname)) {
          exists = true;
          attempted = mod->wasAttempted();
          break;
        }
      }

      if (exists) {
        if (!attempted) {
          // optional dependency hasn't yet been tried
          return false;
        }
      }

      ++i;
    }
  }

  // Second pass: mandatory dependencies.
  i = 0;
  if (!module->depends) {
    return true;
  }

  while (rebase(module, module->depends)[i]) {
    const char* depname = rebase(module, rebase(module, module->depends)[i]);

    bool exists = false;
    for (auto mod : m_Modules) {
      if (!StringCompare(mod->name.cstr(), depname)) {
        exists = true;
        if (!mod->isActive()) {
          // module dependency is not yet active
          return false;
        }
        break;
      }
    }
    if (!exists) {
      return false;
    }

    ++i;
  }
  return true;
}

static int executeModuleThread(void* mod) {
  Module* module = reinterpret_cast<Module*>(mod);

  NOTICE("running module: " << module->name);

  if (module->buffer) {
    if (!module->elf->finaliseModule(module->buffer, module->buflen)) {
      FATAL("KERNELELF: Module relocation failed for module " << module->name);
      KernelElf::instance().updateModuleStatus(module, false, false);
      return false;
    }

    // Check for a constructors list and execute.
    uintptr_t startCtors = module->elf->lookupSymbol("start_ctors");
    uintptr_t endCtors = module->elf->lookupSymbol("end_ctors");

    if (startCtors && endCtors) {
      uintptr_t* iterator = reinterpret_cast<uintptr_t*>(startCtors);
      while (iterator < reinterpret_cast<uintptr_t*>(endCtors)) {
        if (static_cast<intptr_t>(*iterator) == -1) {
          ++iterator;
          continue;
        } else if ((*iterator) == 0) {
          // End of table.
          break;
        }

        uintptr_t ctor = *iterator;
        void (*fp)(void) = reinterpret_cast<void (*)(void)>(ctor);
        fp();
        iterator++;
      }
    } else {
      WARNING("KERNELELF: Module " << module->name << " had no ctors!");
    }

    uintptr_t optionalDeps = module->elf->lookupSymbol("__add_optional_deps");
    if (optionalDeps) {
      NOTICE("KERNELELF: Running module " << module->name << " optional dependencies function.");
      void (*fp)(void) = reinterpret_cast<void (*)(void)>(optionalDeps);
      fp();
    }
  }

  NOTICE("KERNELELF: Executing module " << module->name);

  bool bSuccess = false;
  String moduleName(module->name);
  if (module->entry) {
    bSuccess = module->entry();
  }

  KernelElf::instance().updateModuleStatus(module, bSuccess);

  return 0;
}

bool KernelElf::executeModule(Module* module) {
  EMIT_IF(THREADS && THREADED_MODULE_LOADING) {
    Process* me = Processor::information().getCurrentThread()->getParent();
    Thread* pThread = new Thread(me, executeModuleThread, module);
    pThread->setName("KernelElf module execution thread");
    pThread->detach();
  }
  else {
    executeModuleThread(module);
  }

  return true;
}

void KernelElf::updateModuleStatus(Module* module, bool status, bool runFailureLifecycle) {
  String moduleName(module->name);
  if (status) {
    NOTICE("KERNELELF: Module " << moduleName << " finished executing");
    lockModules();
    module->status = Module::Active;
    if (!m_ModuleExecutions) {
      unlockModules();
      FATAL("KERNELELF: Module execution accounting underflow");
      return;
    }
    --m_ModuleExecutions;
    unlockModules();
  } else {
    NOTICE("KERNELELF: Module " << moduleName << " failed, unloading.");
    bool wasFailed = false;
    bool runLifecycle = false;
    ModuleUnloadClaim claim = UnloadBusy;
    lockModules();
    module->status = Module::Failed;
    claim = claimModuleUnloadLocked(module, true, false, false, wasFailed, runLifecycle);
    unlockModules();
    runLifecycle = runLifecycle && runFailureLifecycle;
    completeUnloadAttempt(module, claim, wasFailed, runLifecycle, true, false);
    lockModules();
    if (!m_ModuleExecutions) {
      unlockModules();
      FATAL("KERNELELF: Module execution accounting underflow");
      return;
    }
    --m_ModuleExecutions;
    unlockModules();
  }
}

void KernelElf::waitForModulesToLoad() {
  while (true) {
    lockModules();
    const bool executing = m_ModuleLoading || m_UnloadingModule || m_ModuleExecutions ||
                           m_ModuleExecutionPasses;
    unlockModules();
    if (!executing) {
      break;
    }
#if THREADS
    Scheduler::instance().yield();
#else
    FATAL("Module execution remained active without scheduler support.");
    return;
#endif
  }

  lockModules();
  const size_t moduleCount = m_Modules.count();
  unlockModules();

  NOTICE("SUCCESSFUL MODULES:");
  for (size_t i = 0; i < moduleCount; ++i) {
    Module* module = nullptr;
    bool active = false;
    lockModules();
    if (i < m_Modules.count()) {
      module = m_Modules[i];
      active = module->isActive();
    }
    unlockModules();
    if (module && active) {
      NOTICE(" - " << module->name);
    }
  }

  NOTICE("UNSUCCESSFUL MODULES:");
  for (size_t i = 0; i < moduleCount; ++i) {
    Module* module = nullptr;
    bool failed = false;
    lockModules();
    if (i < m_Modules.count()) {
      module = m_Modules[i];
      failed = module->isFailed();
    }
    unlockModules();
    if (module && failed) {
      NOTICE(" - " << module->name);
    }
  }
}

void KernelElf::invokeInitModule() {
  lockModules();
  Module* mod = m_InitModule;
  if (mod == nullptr) {
    unlockModules();
    WARNING("KernelElf: no init module was ever preloaded, cannot invoke init");
    return;
  }

  if (m_ModuleShutdown) {
    unlockModules();
    WARNING("KernelElf: refusing to invoke init after shutdown began");
    return;
  }

  if (!moduleDependenciesSatisfiedLocked(mod)) {
    unlockModules();
    FATAL("init module could not be invoked - its dependencies were not satisfied");
    return;
  }

  // Init is held aside only during dependency loading. Once it can execute it
  // must participate in ordinary unload ownership and dependency ordering.
  m_InitModule = nullptr;
  m_Modules.pushBack(mod);
  mod->status = Module::Executing;
  ++m_ModuleExecutions;
  unlockModules();

  executeModuleThread(reinterpret_cast<void*>(mod));
}

uintptr_t KernelElf::globalLookupSymbol(const char* pName) {
  return m_SymbolTable.lookup(String(pName), this);
}

const char* KernelElf::globalLookupSymbol(uintptr_t addr, uintptr_t* startAddr) {
  /// \todo This shouldn't match local or weak symbols.

  // Try a lookup in the kernel.
  const char* ret;
  if ((ret = lookupSymbol(retract(addr), startAddr, m_pSymbolTable))) {
    return ret;
  }

  // OK, that didn't work. Try every module.
  lockModules();
  for (auto it : m_Modules) {
    if (!(it->isActive() || it->isExecuting()) || !it->elf) {
      continue;
    }

    if ((ret = it->elf->lookupSymbol(addr, startAddr))) {
      unlockModules();
      return ret;
    }
  }
  unlockModules();
  WARNING_NOLOCK("KERNELELF: GlobalLookupSymbol(" << Hex << addr << ") failed.");
  return 0;
}

bool KernelElf::hasPendingModules() const {
  bool hasPending = false;
  for (auto it : m_Modules) {
    if (it->isPending()) {
      NOTICE("Pending module: " << it->name);
      hasPending = true;
    }
  }
  return hasPending;
}

void KernelElf::lockModules() {
  EMIT_IF(THREADS) {
    m_ModuleAdjustmentLock.acquire();
  }
}

void KernelElf::unlockModules() {
  EMIT_IF(THREADS) {
    m_ModuleAdjustmentLock.release();
  }
}

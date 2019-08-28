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

#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/SymbolTable.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryTracing.h"
#include "pedigree/kernel/utilities/MemoryCount.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

KernelElf KernelElf::m_Instance;

// Define to dump each module's dependencies in the serial log.
#define DUMP_DEPENDENCIES 0

// Define to 1 to load modules using threads.
#define THREADED_MODULE_LOADING 0

/**
 * Extend the given pointer by adding its canonical prefix again.
 * This is because in the conversion to a 32-bit object, we manage to lose
 * the prefix (as all addresses get truncated to 32 bits)
 */

#define EXTENSION_ADDEND 0xFFFFFFFF00000000ULL

template <class T>
static T *extend(T *p)
{
    EMIT_IF(X86_COMMON && !BITS_32)
    {
        uintptr_t u = reinterpret_cast<uintptr_t>(p);
        if (u < EXTENSION_ADDEND)
            u += EXTENSION_ADDEND;
        return reinterpret_cast<T *>(u);
    }

    return p;
}

template <class T>
static uintptr_t extend(T p)
{
    EMIT_IF(X86_COMMON && !BITS_32)
    {
        // Must assign to a possibly-larger type before arithmetic.
        uintptr_t u = p;
        if (u < EXTENSION_ADDEND)
            u += EXTENSION_ADDEND;
        return u;
    }

    return p;
}

template <class T>
static T *retract(T *p)
{
    EMIT_IF(X86_COMMON && !BITS_32)
    {
        uintptr_t u = reinterpret_cast<uintptr_t>(p);
        if (u >= EXTENSION_ADDEND)
            u -= EXTENSION_ADDEND;
        return reinterpret_cast<T *>(u);
    }

    return p;
}

template <class T>
static uintptr_t retract(T p)
{
    EMIT_IF(X86_COMMON && !BITS_32)
    {
        // Must assign to a possibly-larger type before arithmetic.
        uintptr_t u = p;
        if (u >= EXTENSION_ADDEND)
            u -= EXTENSION_ADDEND;
        return u;
    }

    return p;
}

bool KernelElf::initialise(const BootstrapStruct_t &pBootstrap)
{
    // Do we even have section headers to peek at?
    if (pBootstrap.getSectionHeaderCount() == 0)
    {
        WARNING("No ELF object available to extract symbol table from.");

        // If we are running with static drivers we are OK to call this initialized.
        return STATIC_DRIVERS == 1;
    }

    EMIT_IF(X86_COMMON)
    {
        PhysicalMemoryManager &physicalMemoryManager =
            PhysicalMemoryManager::instance();
        size_t pageSz = PhysicalMemoryManager::getPageSize();

        m_AdditionalSectionHeaders = new MemoryRegion("Kernel ELF Section Headers");

        // Map in section headers.
        size_t sectionHeadersLength = pBootstrap.getSectionHeaderCount() *
                                      pBootstrap.getSectionHeaderEntrySize();
        if ((sectionHeadersLength % pageSz) > 0)
        {
            sectionHeadersLength += pageSz;
        }
        if (physicalMemoryManager.allocateRegion(
                *m_AdditionalSectionHeaders, sectionHeadersLength / pageSz,
                PhysicalMemoryManager::continuous,
                VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write,
                pBootstrap.getSectionHeaders()) == false)
        {
            ERROR("KernelElf::initialise failed to allocate for "
                  "m_AdditionalSectionHeaders");
            return false;
        }

        // Determine the layout of the contents of non-code sections.
        physical_uintptr_t start = ~0;
        physical_uintptr_t end = 0;
        for (size_t i = 1; i < pBootstrap.getSectionHeaderCount(); i++)
        {
            // Force 32-bit section header type as we are a 32-bit ELF object
            // even on 64-bit targets.
            uintptr_t shdr_addr = pBootstrap.getSectionHeaders() +
                                  i * pBootstrap.getSectionHeaderEntrySize();
            Elf32SectionHeader_t *pSh =
                m_AdditionalSectionHeaders
                    ->convertPhysicalPointer<Elf32SectionHeader_t>(shdr_addr);

            if ((pSh->flags & SHF_ALLOC) != SHF_ALLOC)
            {
                if (pSh->addr <= start)
                {
                    start = pSh->addr;
                }

                if ((pSh->addr + pSh->size) >= end)
                {
                    end = pSh->addr + pSh->size;
                }
            }
        }

        // Is there an overlap between headers and section data?
        if ((start & ~(pageSz - 1)) ==
            (pBootstrap.getSectionHeaders() & ~(pageSz - 1)))
        {
            // Yes, there is. Point the section headers MemoryRegion to the
            // Contents.
            delete m_AdditionalSectionHeaders;
            m_AdditionalSectionHeaders = &m_AdditionalSectionContents;
        }

        // Map in all non-alloc sections.
        uintptr_t alignedStart = start & ~(pageSz - 1);
        uintptr_t allocSize = end - alignedStart;
        if ((allocSize % pageSz) > 0)
        {
            allocSize += pageSz;
        }
        size_t additionalContentsPages = allocSize / pageSz;
        if (physicalMemoryManager.allocateRegion(
                m_AdditionalSectionContents, additionalContentsPages,
                PhysicalMemoryManager::continuous,
                VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write,
                start) == false)
        {
            ERROR("KernelElf::initialise failed to allocate for "
                  "m_AdditionalSectionContents");
            return false;
        }
    }

    // Get the string table
    uintptr_t stringTableHeader =
        (pBootstrap.getSectionHeaders() +
         pBootstrap.getSectionHeaderStringTableIndex() *
             pBootstrap.getSectionHeaderEntrySize());
    KernelElfSectionHeader_t *stringTableShdr =
        reinterpret_cast<KernelElfSectionHeader_t *>(stringTableHeader);

    const char *tmpStringTable;

    EMIT_IF(X86_COMMON)
    {
        tmpStringTable =
            m_AdditionalSectionContents.convertPhysicalPointer<const char>(
                stringTableShdr->addr);
    }
    else
    {
        tmpStringTable = reinterpret_cast<const char *>(stringTableShdr->addr);
    }

    // Search for the symbol/string table and adjust sections
    for (size_t i = 1; i < pBootstrap.getSectionHeaderCount(); i++)
    {
        uintptr_t shdr_addr = pBootstrap.getSectionHeaders() +
                              i * pBootstrap.getSectionHeaderEntrySize();

        ElfSectionHeader_t *pSh = 0;

        EMIT_IF(X86_COMMON)
        {
            KernelElfSectionHeader_t *pTruncatedSh =
                m_AdditionalSectionHeaders
                    ->convertPhysicalPointer<KernelElfSectionHeader_t>(shdr_addr);

            // Copy into larger format for analysis
            ElfSectionHeader_t sh;
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
            if ((pSh->flags & SHF_ALLOC) != SHF_ALLOC)
            {
                NOTICE(
                    "Converting shdr " << Hex << pSh->addr << " -> "
                                       << pSh->addr + pSh->size);
                pSh->addr = reinterpret_cast<uintptr_t>(
                    m_AdditionalSectionContents.convertPhysicalPointer<void>(
                        pSh->addr));
                NOTICE(" to " << Hex << pSh->addr);
                pSh->offset = pSh->addr;
            }
        }
        else
        {
            pSh = reinterpret_cast<ElfSectionHeader_t *>(shdr_addr);
        }

        // Save the symbol/string table
        const char *pStr = tmpStringTable + pSh->name;

        if (pSh->type == SHT_SYMTAB)
        {
            m_pSymbolTable = reinterpret_cast<KernelElfSymbol_t *>(pSh->addr);
            m_nSymbolTableSize = pSh->size;
        }
        else if (!StringCompare(pStr, ".strtab"))
        {
            m_pStringTable = reinterpret_cast<char *>(pSh->addr);
        }
        else if (!StringCompare(pStr, ".shstrtab"))
        {
            m_pShstrtab = reinterpret_cast<char *>(pSh->addr);
        }
        else if (!StringCompare(pStr, ".debug_frame"))
        {
            m_pDebugTable = reinterpret_cast<uint32_t *>(pSh->addr);
            m_nDebugTableSize = pSh->size;
        }
    }

    // Initialise remaining member variables
    m_pSectionHeaders = reinterpret_cast<KernelElfSectionHeader_t *>(
        pBootstrap.getSectionHeaders());
    m_nSectionHeaders = pBootstrap.getSectionHeaderCount();

    if (DEBUGGER && m_pSymbolTable && m_pStringTable)
    {
        KernelElfSymbol_t *pSymbol = m_pSymbolTable;

        const char *pStrtab = reinterpret_cast<const char *>(m_pStringTable);

        // quick pass to preallocate for the symbol table
        size_t numLocal = 0;
        size_t numWeak = 0;
        size_t numGlobal = 0;
        for (size_t i = 0; i < m_nSymbolTableSize / sizeof(*pSymbol); i++)
        {
            switch (ST_BIND(m_pSymbolTable[i].info))
            {
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

        NOTICE(
            "KERNELELF: preallocating symbol table with "
            << numGlobal << " global " << numWeak << " weak and " << numLocal
            << " local symbols.");
        m_SymbolTable.preallocate(numGlobal, numWeak, this, numLocal);

        for (size_t i = 1; i < m_nSymbolTableSize / sizeof(*pSymbol); i++)
        {
            const char *pStr = 0;

            if (ST_TYPE(pSymbol->info) == STT_SECTION)
            {
                // Section type - the name will be the name of the section
                // header it refers to.
                KernelElfSectionHeader_t *pSh =
                    &m_pSectionHeaders[pSymbol->shndx];
                // If it's not allocated, it's a link-once-only section that we
                // can ignore.
                if (!(pSh->flags & SHF_ALLOC))
                {
                    pSymbol++;
                    continue;
                }
                // Grab the shstrtab
                pStr = reinterpret_cast<const char *>(m_pShstrtab) + pSh->name;
            }
            else
            {
                pStr = pStrtab + pSymbol->name;
            }

            // Insert the symbol into the symbol table.
            SymbolTable::Binding binding;
            switch (ST_BIND(pSymbol->info))
            {
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

            EMIT_IF(!TRACK_HIDDEN_SYMBOLS)
            {
                // Don't insert hidden symbols to the main symbol table.
                if (pSymbol->other == STV_HIDDEN)
                {
                    ++pSymbol;
                    continue;
                }
            }

            if (pStr && (*pStr != '\0'))
            {
                EMIT_IF(HOSTED)
                {
                    // If name starts with __wrap_, rewrite it in flight as it's
                    // a wrapped symbol on hosted systems.
                    if (!StringCompareN(pStr, "__wrap_", 7))
                    {
                        pStr += 7;
                    }
                }

                m_SymbolTable.insert(
                    String(pStr), binding, this, extend(pSymbol->value));
            }
            pSymbol++;
        }
    }

    return true;
}

KernelElf::KernelElf()
    :
      m_AdditionalSectionContents("Kernel ELF Section Data"),
      m_AdditionalSectionHeaders(0),
      m_Modules(), m_ModuleAllocator(), m_pSectionHeaders(0), m_pSymbolTable(0),
      m_ModuleProgress(0), m_ModuleAdjustmentLock(false),
      m_InitModule(nullptr)
{
}

KernelElf::~KernelElf()
{
    delete m_AdditionalSectionHeaders;

    // All of these non-alloc sections are just pointers into the loaded kernel
    // ELF, which is not heap allocated. In normal Elf objects these are
    // allocated and then copied into. Not so here.
    m_pSymbolTable = nullptr;
    m_pStringTable = nullptr;
    m_pShstrtab = nullptr;
    m_pDebugTable = nullptr;
}

Module *KernelElf::loadModule(uint8_t *pModule, size_t len, bool silent)
{
    MemoryCount guard(__PRETTY_FUNCTION__);

    // The module memory allocator requires dynamic memory - this isn't
    // initialised until after our constructor is called, so check here if we've
    // loaded any modules yet. If not, we can initialise our memory allocator.
    if (m_Modules.count() == 0)
    {
        uintptr_t start = VirtualAddressSpace::getKernelAddressSpace()
                              .getKernelModulesStart();
        uintptr_t end =
            VirtualAddressSpace::getKernelAddressSpace().getKernelModulesEnd();
        m_ModuleAllocator.free(start, end - start);
    }

    Module *module = new Module;

    module->elf = new Elf();
    module->buffer = pModule;
    module->buflen = len;

    if (!module->elf->create(pModule, len))
    {
        FATAL("Module load failed (1)");
        delete module;
        return 0;
    }

    if (!module->elf->loadModule(
            pModule, len, module->loadBase, module->loadSize, &m_SymbolTable))
    {
        FATAL("Module load failed (2)");
        delete module;
        return 0;
    }

    //  Load the module debug table (if any)
    if (module->elf->debugFrameTableLength())
    {
        size_t sz = m_nDebugTableSize + module->elf->debugFrameTableLength();
        if (sz % sizeof(uint32_t))
            sz += sizeof(uint32_t);
        uint32_t *pDebug = new uint32_t[sz / sizeof(uint32_t)];
        if (UNLIKELY(!pDebug))
        {
            ERROR("Could not load module debug frame information.");
        }
        else
        {
            MemoryCopy(pDebug, m_pDebugTable, m_nDebugTableSize);
            MemoryCopy(
                pDebug + m_nDebugTableSize,
                reinterpret_cast<const void *>(module->elf->debugFrameTable()),
                module->elf->debugFrameTableLength());
            m_nDebugTableSize += module->elf->debugFrameTableLength();
            m_pDebugTable = pDebug;
            NOTICE("Added debug module debug frame information.");
        }
    }

    // Look up the module's name and entry/exit functions, and dependency list.
    const char **pName = reinterpret_cast<const char **>(
        module->elf->lookupSymbol("g_pModuleName"));
    if (!pName)
    {
        ERROR("KERNELELF: Hit an invalid module, ignoring");
        return 0;
    }
    module->name = rebase(module, *pName);
    module->elf->setName(module->name);
    auto entryPoint = *reinterpret_cast<bool (**)()>(
        module->elf->lookupSymbol("g_pModuleEntry"));
    auto exitPoint = *reinterpret_cast<void (**)()>(
        module->elf->lookupSymbol("g_pModuleExit"));
    // Readjust entry/exit functions for the loaded module if needed
    if (entryPoint)
    {
        entryPoint = adjust_pointer(entryPoint, module->loadBase);
    }
    if (exitPoint)
    {
        exitPoint = adjust_pointer(exitPoint, module->loadBase);
    }
    module->entry = entryPoint;
    module->exit = exitPoint;
    module->depends =
        reinterpret_cast<const char **>(module->elf->lookupSymbol("g_pDepends"));
    module->depends_opt = reinterpret_cast<const char **>(
        module->elf->lookupSymbol("g_pOptionalDepends"));
    DEBUG_LOG(
        "KERNELELF: Preloaded module "
        << module->name << " at " << Hex << module->loadBase << " to "
        << (module->loadBase + module->loadSize));
    DEBUG_LOG(
        "KERNELELF: Module " << module->name << " consumes " << Dec
                             << (module->loadSize / 1024) << Hex
                             << "K of memory");

    EMIT_IF(DUMP_DEPENDENCIES)
    {
        size_t i = 0;
        while (module->depends_opt && rebase(module, module->depends_opt[i]))
        {
            DEBUG_LOG(
                "KERNELELF: Module " << module->name << " optdepends on "
                                     << rebase(module, module->depends_opt[i]));
            ++i;
        }

        i = 0;
        while (module->depends && rebase(module, module->depends[i]))
        {
            DEBUG_LOG(
                "KERNELELF: Module " << module->name << " depends on "
                                     << rebase(module, module->depends[i]));
            ++i;
        }
    }

    EMIT_IF(MEMORY_TRACING)
    {
        traceMetadata(
            NormalStaticString(module->name),
            reinterpret_cast<void *>(module->loadBase),
            reinterpret_cast<void *>(module->loadBase + module->loadSize));
    }

    if (!StringCompare(module->name, "init"))
    {
        m_InitModule = module;
    }
    else
    {
        g_BootProgressCurrent++;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleload");

        module->status = Module::Preloaded;

        m_Modules.pushBack(module);
    }

    return module;
}

void KernelElf::executeModules(bool silent, bool progress)
{
    NOTICE("KERNELELF: executing " << m_Modules.count() << " modules...");

    // keep trying until all modules were invoked
    bool executedModule = true;
    while (executedModule)
    {
        executedModule = false;

        for (auto module : m_Modules)
        {
            if (module->wasAttempted())
            {
                continue;
            }

            bool dependenciesSatisfied = moduleDependenciesSatisfied(module);

            // Can we load this module yet?
            if (dependenciesSatisfied)
            {
                executeModule(module);

                g_BootProgressCurrent++;
                if (g_BootProgressUpdate && !silent)
                    g_BootProgressUpdate("moduleexec");

                executedModule = true;
            }
        }
    }
}

Module *KernelElf::loadModule(struct ModuleInfo *info, bool silent)
{
    /// \todo rewrite to the new module dependency logic
    Module *module = new Module;

    module->buffer = 0;
    module->buflen = 0;

    module->name = info->name;
    module->entry = info->entry;
    module->exit = info->exit;
    module->depends = info->dependencies;
    module->depends_opt = info->opt_dependencies;
    DEBUG_LOG("KERNELELF: Preloaded module " << module->name);

    EMIT_IF(DUMP_DEPENDENCIES)
    {
        size_t i = 0;
        while (module->depends_opt && rebase(module, module->depends_opt[i]))
        {
            DEBUG_LOG(
                "KERNELELF: Module " << module->name << " optdepends on "
                                     << rebase(module, module->depends_opt[i]));
            ++i;
        }

        i = 0;
        while (module->depends && rebase(module, module->depends[i]))
        {
            DEBUG_LOG(
                "KERNELELF: Module " << module->name << " depends on "
                                     << rebase(module, module->depends[i]));
            ++i;
        }
    }

    EMIT_IF(MEMORY_TRACING)
    {
        traceMetadata(
            NormalStaticString(module->name),
            reinterpret_cast<void *>(module->loadBase),
            reinterpret_cast<void *>(module->loadBase + module->loadSize));
    }

    if (!StringCompare(module->name, "init"))
    {
        m_InitModule = module;
    }
    else
    {
        g_BootProgressCurrent++;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleload");

        module->status = Module::Preloaded;

        m_Modules.pushBack(module);
    }

    return module;
}

void KernelElf::unloadModule(const char *name, bool silent, bool progress)
{
    String findName(name);
    for (auto it : m_Modules)
    {
        if (it->name == findName)
        {
            unloadModule(it, silent, progress);
            return;
        }
    }
    ERROR("KERNELELF: Module " << name << " not found");
}

void KernelElf::unloadModule(Module *module, bool silent, bool progress)
{
    NOTICE("KERNELELF: Unloading module " << module->name);

    if (progress)
    {
        g_BootProgressCurrent--;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleunload");
    }

    if (module->exit)
        module->exit();

    // Check for a destructors list and execute.
    // Note: static drivers have their ctors/dtors all shared.
    EMIT_IF(!STATIC_DRIVERS)
    {
        uintptr_t startDtors = module->elf->lookupSymbol("start_dtors");
        uintptr_t endDtors = module->elf->lookupSymbol("end_dtors");

        if (startDtors && endDtors)
        {
            uintptr_t *iterator = reinterpret_cast<uintptr_t *>(startDtors);
            while (iterator < reinterpret_cast<uintptr_t *>(endDtors))
            {
                if (static_cast<intptr_t>(*iterator) == -1)
                {
                    ++iterator;
                    continue;
                }
                else if ((*iterator) == 0)
                {
                    // End of table.
                    break;
                }

                uintptr_t dtor = *iterator;
                void (*fp)(void) = reinterpret_cast<void (*)(void)>(dtor);
                fp();
                iterator++;
            }
        }

        m_SymbolTable.eraseByElf(module->elf);
    }

    if (progress)
    {
        g_BootProgressCurrent--;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleunloaded");
    }

    NOTICE("KERNELELF: Module " << module->name << " unloaded.");

    EMIT_IF(!STATIC_DRIVERS)
    {
        size_t pageSz = PhysicalMemoryManager::getPageSize();
        size_t numPages =
            (module->loadSize / pageSz) + (module->loadSize % pageSz ? 1 : 0);

        // Unmap!
        VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
        for (size_t i = 0; i < numPages; i++)
        {
            void *unmapAddr =
                reinterpret_cast<void *>(module->loadBase + (i * pageSz));
            if (va.isMapped(unmapAddr))
            {
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

    // Failed also means unloaded - it just reports a particular status.
    // A module unloaded intentionally (i.e. by the user) that was successfully
    // active and running goes into Unloaded mode on unload.
    if (!module->isFailed())
    {
        module->status = Module::Unloaded;
    }
}

void KernelElf::unloadModules()
{
    if (g_BootProgressUpdate)
        g_BootProgressUpdate("unload");

    for (auto it : m_Modules)
    {
        if (!it->isUnloaded())
        {
            unloadModule(it);
            delete it;
        }
    }

    m_Modules.clear();
}

bool KernelElf::moduleIsLoaded(char *name)
{
    // this should hash the name and make comparisons super fast
    String compName(name);

    for (auto module : m_Modules)
    {
        if (module->isLoaded())
        {
            if (module->name == compName)
            {
                return true;
            }
        }
    }

    return false;
}

char *KernelElf::getDependingModule(char *name)
{
    for (auto module : m_Modules)
    {
        if (!module->isLoaded())
        {
            // can't depend on unloaded modules - might be unmapped
            continue;
        }
        else if (module->depends == 0)
        {
            continue;
        }

        size_t i = 0;
        while (module->depends[i])
        {
            const char *rebased = rebase(module, module->depends[i]);
            if (!StringCompare(rebased, name))
            {
                return const_cast<char *>(static_cast<const char *>(module->name));
            }

            ++i;
        }
    }

    return 0;
}

bool KernelElf::moduleDependenciesSatisfied(Module *module)
{
    int i = 0;

    // First pass: optional dependencies.
    if (module->depends_opt)
    {
        while (module->depends_opt[i])
        {
            String depname(rebase(module, module->depends_opt[i]));

            bool exists = false;
            bool attempted = false;
            for (auto mod : m_Modules)
            {
                if (mod->name == depname)
                {
                    exists = true;
                    attempted = mod->wasAttempted();
                    break;
                }
            }

            if (exists)
            {
                if (!attempted)
                {
                    // optional dependency hasn't yet been tried
                    return false;
                }
            }
            else
            {
                EMIT_IF(DUMP_DEPENDENCIES)
                {
                    WARNING("KernelElf: optional dependency '" << depname << "' (wanted by '" << module->name << "') doesn't even exist, skipping.");
                }
            }

            ++i;
        }
    }

    // Second pass: mandatory dependencies.
    i = 0;
    if (!module->depends)
    {
        return true;
    }

    while (module->depends[i])
    {
        String depname(rebase(module, module->depends[i]));

        for (auto mod : m_Modules)
        {
            if (mod->name == depname)
            {
                if (!mod->isActive())
                {
                    // module dependency is not yet active
                    return false;
                }
            }
        }

        ++i;
    }
    return true;
}

static int executeModuleThread(void *mod)
{
    Module *module = reinterpret_cast<Module *>(mod);
    module->status = Module::Executing;

    if (module->buffer)
    {
        if (!module->elf->finaliseModule(module->buffer, module->buflen))
        {
            FATAL(
                "KERNELELF: Module relocation failed for module "
                << module->name);
            return false;
        }

        // Check for a constructors list and execute.
        uintptr_t startCtors = module->elf->lookupSymbol("start_ctors");
        uintptr_t endCtors = module->elf->lookupSymbol("end_ctors");

        if (startCtors && endCtors)
        {
            uintptr_t *iterator = reinterpret_cast<uintptr_t *>(startCtors);
            while (iterator < reinterpret_cast<uintptr_t *>(endCtors))
            {
                if (static_cast<intptr_t>(*iterator) == -1)
                {
                    ++iterator;
                    continue;
                }
                else if ((*iterator) == 0)
                {
                    // End of table.
                    break;
                }

                uintptr_t ctor = *iterator;
                void (*fp)(void) = reinterpret_cast<void (*)(void)>(ctor);
                fp();
                iterator++;
            }
        }
        else
        {
            WARNING("KERNELELF: Module " << module->name << " had no ctors!");
        }

        uintptr_t optionalDeps = module->elf->lookupSymbol("__add_optional_deps");
        if (optionalDeps)
        {
            NOTICE("KERNELELF: Running module " << module->name << " optional dependencies function.");
            void (*fp)(void) = reinterpret_cast<void (*)(void)>(optionalDeps);
            fp();
        }
    }

    NOTICE("KERNELELF: Executing module " << module->name);

    bool bSuccess = false;
    String moduleName(module->name);
    if (module->entry)
    {
        bSuccess = module->entry();
    }

    KernelElf::instance().updateModuleStatus(module, bSuccess);

    return 0;
}

bool KernelElf::executeModule(Module *module)
{
    EMIT_IF(THREADS && THREADED_MODULE_LOADING)
    {
        Process *me = Processor::information().getCurrentThread()->getParent();
        Thread *pThread = new Thread(me, executeModuleThread, module);
        pThread->detach();
    }
    else
    {
        executeModuleThread(module);
    }

    return true;
}

void KernelElf::updateModuleStatus(Module *module, bool status)
{
    String moduleName(module->name);
    if (status)
    {
        NOTICE("KERNELELF: Module " << moduleName << " finished executing");
        module->status = Module::Active;
    }
    else
    {
        NOTICE("KERNELELF: Module " << moduleName << " failed, unloading.");
        module->status = Module::Failed;
        unloadModule(moduleName, true, false);
    }

    m_ModuleProgress.release();
}

void KernelElf::waitForModulesToLoad()
{
    for (size_t i = 0; i < m_Modules.count(); ++i)
    {
        m_ModuleProgress.acquire();
    }

    NOTICE("SUCCESSFUL MODULES:");
    for (auto it : m_Modules)
    {
        if (it->isActive())
        {
            NOTICE(" - " << it->name);
        }
    }

    NOTICE("UNSUCCESSFUL MODULES:");
    for (auto it : m_Modules)
    {
        if (it->isFailed())
        {
            NOTICE(" - " << it->name);
        }
    }
}

void KernelElf::invokeInitModule()
{
    if (m_InitModule == nullptr)
    {
        WARNING("KernelElf: no init module was ever preloaded, cannot invoke init");
        return;
    }

    Module *mod = m_InitModule;
    m_InitModule = nullptr;

    if (!moduleDependenciesSatisfied(mod))
    {
        FATAL("init module could not be invoked - its dependencies were not satisfied");
    }

    executeModuleThread(reinterpret_cast<void *>(mod));
}

uintptr_t KernelElf::globalLookupSymbol(const char *pName)
{
    return m_SymbolTable.lookup(String(pName), this);
}

const char *KernelElf::globalLookupSymbol(uintptr_t addr, uintptr_t *startAddr)
{
    /// \todo This shouldn't match local or weak symbols.

    // Try a lookup in the kernel.
    const char *ret;
    if ((ret = lookupSymbol(retract(addr), startAddr, m_pSymbolTable)))
    {
        return ret;
    }

    // OK, that didn't work. Try every module.
    lockModules();
    for (auto it : m_Modules)
    {
        if (!(it->isActive() || it->isExecuting()))
        {
            continue;
        }

        unlockModules();

        if ((ret = it->elf->lookupSymbol(addr, startAddr)))
        {
            return ret;
        }

        lockModules();
    }
    unlockModules();
    WARNING_NOLOCK(
        "KERNELELF: GlobalLookupSymbol(" << Hex << addr << ") failed.");
    return 0;
}

bool KernelElf::hasPendingModules() const
{
    bool hasPending = false;
    for (auto it : m_Modules)
    {
        if (it->isPending())
        {
            NOTICE("Pending module: " << *it->name);
        }
    }
    return hasPending;
}

void KernelElf::lockModules()
{
    EMIT_IF(THREADS)
    {
        m_ModuleAdjustmentLock.acquire();
    }
}

void KernelElf::unlockModules()
{
    EMIT_IF(THREADS)
    {
        m_ModuleAdjustmentLock.release();
    }
}

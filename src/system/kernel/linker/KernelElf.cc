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

#include <BootstrapInfo.h>
#include <linker/KernelElf.h>
#include <utilities/utility.h>
#include <processor/Processor.h>
#include <processor/VirtualAddressSpace.h>
#include <processor/PhysicalMemoryManager.h>
#include <Log.h>

KernelElf KernelElf::m_Instance;

/**
 * Extend the given pointer by adding its canonical prefix again.
 * This is because in the conversion to a 32-bit object, we manage to lose
 * the prefix (as all addresses get truncated to 32 bits)
 */

template<class T>
static T *extend(T *p)
{
#ifdef BITS_32
    return p;
#else
    uintptr_t u = reinterpret_cast<uintptr_t>(p);
    if(u < 0xFFFFFFFF00000000ULL)
        u += 0xFFFFFFFF00000000ULL;
    return reinterpret_cast<T*>(u);
#endif
}

template<class T>
static uintptr_t extend(T p)
{
#ifdef BITS_32
    return p;
#else
    // Must assign to a possibly-larger type before arithmetic.
    uintptr_t u = p;
    if(u < 0xFFFFFFFF00000000ULL)
        u += 0xFFFFFFFF00000000ULL;
    return u;
#endif
}

bool KernelElf::initialise(const BootstrapStruct_t &pBootstrap)
{
    PhysicalMemoryManager &physicalMemoryManager = PhysicalMemoryManager::instance();
    size_t pageSz = PhysicalMemoryManager::getPageSize();

#if defined(X86_COMMON)
    m_AdditionalSectionHeaders = new MemoryRegion("Kernel ELF Section Headers");

    // Map in section headers.
    size_t sectionHeadersLength = pBootstrap.getSectionHeaderCount() * pBootstrap.getSectionHeaderEntrySize();
    if (physicalMemoryManager.allocateRegion(*m_AdditionalSectionHeaders,
                                             (sectionHeadersLength + pageSz - 1) / pageSz,
                                             PhysicalMemoryManager::continuous,
                                             VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write,
                                             pBootstrap.getSectionHeaders()) == false)
    {
        ERROR("KernelElf::initialise failed to allocate for m_AdditionalSectionHeaders");
        return false;
    }

    // Determine the layout of the contents of non-code sections.
    physical_uintptr_t start = ~0;
    physical_uintptr_t end   = 0;
    for (size_t i = 1; i < pBootstrap.getSectionHeaderCount(); i++)
    {
        // Force 32-bit section header type as we are a 32-bit ELF object
        // even on 64-bit targets.
        uintptr_t shdr_addr = pBootstrap.getSectionHeaders() + i * pBootstrap.getSectionHeaderEntrySize();
        Elf32SectionHeader_t *pSh = m_AdditionalSectionHeaders->convertPhysicalPointer<Elf32SectionHeader_t>(shdr_addr);

        if ((pSh->flags & SHF_ALLOC) != SHF_ALLOC)
        {
            if (pSh->addr <= start)
            {
                start = pSh->addr;
            }

            if (pSh->addr >= end)
            {
                end = pSh->addr + pSh->size;
            }
        }
    }

    // Is there an overlap between headers and section data?
    if ((start & ~(pageSz - 1)) == (pBootstrap.getSectionHeaders() & ~(pageSz - 1)))
    {
        // Yes, there is. Point the section headers MemoryRegion to the Contents.
        delete m_AdditionalSectionHeaders;
        m_AdditionalSectionHeaders = &m_AdditionalSectionContents;
    }

    // Map in all non-alloc sections.
    if (physicalMemoryManager.allocateRegion(m_AdditionalSectionContents,
                                             (end - start + pageSz - 1) / pageSz,
                                             PhysicalMemoryManager::continuous,
                                             VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write,
                                             start) == false)
    {
        ERROR("KernelElf::initialise failed to allocate for m_AdditionalSectionContents");
        return false;
    }
#endif

    // Get the string table
    const char *tmpStringTable = reinterpret_cast<const char*>(reinterpret_cast<Elf32SectionHeader_t*>(pBootstrap.getSectionHeaders() + pBootstrap.getSectionHeaderStringTableIndex() * pBootstrap.getSectionHeaderEntrySize())->addr);

    // Search for the symbol/string table and adjust sections
    for (size_t i = 1; i < pBootstrap.getSectionHeaderCount(); i++)
    {
        uintptr_t shdr_addr = pBootstrap.getSectionHeaders() + i * pBootstrap.getSectionHeaderEntrySize();
        Elf32SectionHeader_t *pSh = m_AdditionalSectionHeaders->convertPhysicalPointer<Elf32SectionHeader_t>(shdr_addr);

#if defined(X86_COMMON)
        // Adjust the section
        if ((pSh->flags & SHF_ALLOC) != SHF_ALLOC)
        {
            pSh->addr = reinterpret_cast<uintptr_t>(m_AdditionalSectionContents.convertPhysicalPointer<void>(pSh->addr));
            pSh->offset = pSh->addr;
        }
#endif

        // Save the symbol/string table
        const char *pStr = tmpStringTable + pSh->name;

        if (pSh->type == SHT_SYMTAB)
        {
            m_pSymbolTable = extend(reinterpret_cast<Elf32Symbol_t*> (pSh->addr));
            m_nSymbolTableSize = pSh->size;
        }
        else if (!strcmp(pStr, ".strtab"))
        {
            m_pStringTable = extend(reinterpret_cast<char*> (pSh->addr));
        }
        else if (!strcmp(pStr, ".shstrtab"))
        {
            m_pShstrtab = extend(reinterpret_cast<char*> (pSh->addr));
        }
        else if (!strcmp(pStr, ".debug_frame"))
        {
            m_pDebugTable = extend(reinterpret_cast<uint8_t*> (pSh->addr));
            m_nDebugTableSize = pSh->size;
        }
    }

    // Initialise remaining member variables
#if defined(X86_COMMON)
    m_pSectionHeaders = m_AdditionalSectionHeaders->convertPhysicalPointer<Elf32SectionHeader_t>(pBootstrap.getSectionHeaders());
#else
    m_pSectionHeaders = reinterpret_cast<Elf32SectionHeader_t*>(pBootstrap.getSectionHeaders());
#endif
    m_nSectionHeaders = pBootstrap.getSectionHeaderCount();

#ifdef DEBUGGER
    if (m_pSymbolTable && m_pStringTable)
    {
        Elf32Symbol_t *pSymbol = reinterpret_cast<Elf32Symbol_t *>(m_pSymbolTable);

        const char *pStrtab = reinterpret_cast<const char *>(m_pStringTable);

        for (size_t i = 1; i < m_nSymbolTableSize / sizeof(Elf32Symbol_t); i++)
        {
            const char *pStr = 0;

            if (ST_TYPE(pSymbol->info) == 3)
            {
                // Section type - the name will be the name of the section header it refers to.
                Elf32SectionHeader_t *pSh = &m_pSectionHeaders[pSymbol->shndx];
                // If it's not allocated, it's a link-once-only section that we can ignore.
                if (!(pSh->flags & SHF_ALLOC))
                {
                    pSymbol++;
                    continue;
                }
                // Grab the shstrtab
                pStr = reinterpret_cast<const char*> (m_pShstrtab) + pSh->name;
            }
            else
                pStr = pStrtab + pSymbol->name;

            // Insert the symbol into the symbol table.
            SymbolTable::Binding binding;
            switch (ST_BIND(pSymbol->info))
            {
                case 0: // STB_LOCAL
                    binding = SymbolTable::Local;
                    break;
                case 1: // STB_GLOBAL
                    binding = SymbolTable::Global;
                    break;
                case 2: // STB_WEAK
                    binding = SymbolTable::Weak;
                    break;
                default:
                    binding = SymbolTable::Global;
            }

            if (pStr && (*pStr != '\0'))
            {
                m_SymbolTable.insert(String(pStr), binding, this, extend(pSymbol->value));
            }
            pSymbol++;
        }
    }
#endif

    return true;
}

KernelElf::KernelElf() :
#if defined(X86_COMMON)
    m_AdditionalSectionContents("Kernel ELF Section Data"),
    m_AdditionalSectionHeaders(0),
#endif
    m_Modules(), m_LoadedModules(), m_FailedModules(), m_PendingModules(), m_ModuleAllocator(),
    m_pSectionHeaders(0), m_pSymbolTable(0)
{
}

KernelElf::~KernelElf()
{
}

#ifdef PPC_COMMON
#define MOD_START 0xe1000000
#elif defined(X86)
#define MOD_START 0xfa000000
#elif defined(X64)
#define MOD_START 0xFFFFFFFFF0000000
#elif defined(ARMV7)
#define MOD_START 0x60000000
#endif
#define MOD_LEN 0x400000

Module *KernelElf::loadModule(uint8_t *pModule, size_t len, bool silent)
{
    // The module memory allocator requires dynamic memory - this isn't initialised until after our constructor
    // is called, so check here if we've loaded any modules yet. If not, we can initialise our memory allocator.
    if (m_Modules.count() == 0)
    {
        m_ModuleAllocator.free(MOD_START, MOD_LEN);
    }

    Module *module = new Module;

    module->buffer = pModule;
    module->buflen = len;

    if (!module->elf.create(pModule, len))
    {
        FATAL ("Module load failed (1)");
        delete module;
        return 0;
    }

    if (!module->elf.loadModule(pModule, len, module->loadBase, module->loadSize, &m_SymbolTable))
    {
        FATAL ("Module load failed (2)");
        delete module;
        return 0;
    }

    //  Load the module debug table (if any)
    if(module->elf.debugFrameTableLength())
    {
        uint8_t*  pDebug = new uint8_t[m_nDebugTableSize + module->elf.debugFrameTableLength()];
        if(UNLIKELY(!pDebug))
        {
            ERROR ("Could not load module debug frame information.");
        }
        else
        {
            memcpy(pDebug, m_pDebugTable, m_nDebugTableSize);
            memcpy( pDebug + m_nDebugTableSize,
                    reinterpret_cast<const void*>(module->elf.debugFrameTable()),
                    module->elf.debugFrameTableLength());
            m_nDebugTableSize+= module->elf.debugFrameTableLength();
            m_pDebugTable = pDebug;
            NOTICE("Added debug module debug frame information.");
        }
    }

    // Look up the module's name and entry/exit functions, and dependency list.
    const char **pName = reinterpret_cast<const char**> (module->elf.lookupSymbol("g_pModuleName"));
    if(!pName)
    {
        ERROR("KERNELELF: Hit an invalid module, ignoring");
        return 0;
    }
    module->name = *pName;
    module->entry = *reinterpret_cast<bool (**)()> (module->elf.lookupSymbol("g_pModuleEntry"));
    module->exit = *reinterpret_cast<void (**)()> (module->elf.lookupSymbol("g_pModuleExit"));
    module->depends = reinterpret_cast<const char **> (module->elf.lookupSymbol("g_pDepends"));
    module->depends_opt = reinterpret_cast<const char **> (module->elf.lookupSymbol("g_pOptionalDepends"));
    DEBUG("KERNELELF: Preloaded module " << module->name << " at " << module->loadBase);

#ifdef MEMORY_TRACING
    traceMetadata(NormalStaticString(module->name), reinterpret_cast<void*>(module->loadBase), reinterpret_cast<void*>(module->loadBase + module->loadSize));
#endif

    g_BootProgressCurrent ++;
    if (g_BootProgressUpdate && !silent)
        g_BootProgressUpdate("moduleload");

    m_Modules.pushBack(module);

    // Can we load this module yet?
    if (moduleDependenciesSatisfied(module))
    {
        executeModule(module);

        g_BootProgressCurrent ++;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleexec");

        // Now check if we've allowed any currently pending modules to load.
        bool somethingLoaded = true;
        while (somethingLoaded)
        {
            somethingLoaded = false;
            for (Vector<Module*>::Iterator it = m_PendingModules.begin();
                it != m_PendingModules.end();
                it++)
            {
                if (moduleDependenciesSatisfied(*it))
                {
                    if(!executeModule(*it))
                        module = 0;

                    g_BootProgressCurrent ++;
                    if (g_BootProgressUpdate && !silent)
                        g_BootProgressUpdate("moduleexec");

                    m_PendingModules.erase(it);
                    somethingLoaded = true;
                    break;
                }
            }
        }
    }
    else
    {
        m_PendingModules.pushBack(module);
    }

    return module;
}

#ifdef STATIC_DRIVERS
Module *KernelElf::loadModule(struct ModuleInfo *info, bool silent)
{
    Module *module = new Module;

    module->buffer = 0;
    module->buflen = 0;

    module->name = info->name;
    module->entry = info->entry;
    module->exit = info->exit;
    module->depends = info->dependencies;
    DEBUG("KERNELELF: Preloaded module " << module->name);

    g_BootProgressCurrent ++;
    if (g_BootProgressUpdate && !silent)
        g_BootProgressUpdate("moduleload");

    m_Modules.pushBack(module);

    // Can we load this module yet?
    if (moduleDependenciesSatisfied(module))
    {
        executeModule(module);

        g_BootProgressCurrent ++;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleexec");

        // Now check if we've allowed any currently pending modules to load.
        bool somethingLoaded = true;
        while (somethingLoaded)
        {
            somethingLoaded = false;
            for (Vector<Module*>::Iterator it = m_PendingModules.begin();
                it != m_PendingModules.end();
                )
            {
                if (moduleDependenciesSatisfied(*it))
                {
                    executeModule(*it);
                    g_BootProgressCurrent ++;
                    if (g_BootProgressUpdate && !silent)
                        g_BootProgressUpdate("moduleexec");

                    it = m_PendingModules.erase(it);
                    somethingLoaded = true;
                    break;
                }
                else
                    ++it;
            }
        }
    }
    else
    {
        m_PendingModules.pushBack(module);
    }

    return module;
}
#endif

void KernelElf::unloadModule(const char *name, bool silent, bool progress)
{
    for (Vector<Module*>::Iterator it = m_LoadedModules.begin();
        it != m_LoadedModules.end();
        it++)
    {
        if(!strcmp((*it)->name, name))
        {
            unloadModule(it, silent, progress);
            return;
        }
    }
    ERROR("KERNELELF: Module " << name << " not found");
}

void KernelElf::unloadModule(Vector<Module*>::Iterator it, bool silent, bool progress)
{
    Module *module = *it;
    NOTICE("KERNELELF: Unloading module " << module->name);

    if(progress)
    {
        g_BootProgressCurrent --;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleunload");
    }

    if(module->exit)
        module->exit();

    // Check for a destructors list and execute.
    uintptr_t startDtors = module->elf.lookupSymbol("start_dtors");
    uintptr_t endDtors = module->elf.lookupSymbol("end_dtors");

    if (startDtors && endDtors)
    {
        uintptr_t *iterator = reinterpret_cast<uintptr_t*>(startDtors);
        while (iterator < reinterpret_cast<uintptr_t*>(endDtors))
        {
            void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
            fp();
            iterator++;
        }
    }

    m_SymbolTable.eraseByElf(&module->elf);

    if(progress)
    {
        g_BootProgressCurrent --;
        if (g_BootProgressUpdate && !silent)
            g_BootProgressUpdate("moduleunloaded");
    }

    m_LoadedModules.erase(it);
    //m_Modules.erase(it);

    NOTICE("KERNELELF: Module " << module->name << " unloaded.");

    size_t pageSz = PhysicalMemoryManager::getPageSize();
    size_t numPages = (module->loadSize / pageSz) + (module->loadSize % pageSz ? 1 : 0);

    // Unmap!
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    for (size_t i = 0; i < numPages; i++)
    {
        void *unmapAddr = reinterpret_cast<void*>(module->loadBase + (i * pageSz));
        if(va.isMapped(unmapAddr))
        {
            // Unmap the virtual address
            physical_uintptr_t phys = 0;
            size_t flags = 0;
            va.getMapping(unmapAddr, phys, flags);
            va.unmap(reinterpret_cast<void*>(unmapAddr));

            // Free the physical page
            PhysicalMemoryManager::instance().freePage(phys);
        }
    }

    m_ModuleAllocator.free(module->loadBase, module->loadSize);

    delete module;
}

void KernelElf::unloadModules()
{
    if (g_BootProgressUpdate)
        g_BootProgressUpdate("unload");

    for (Vector<Module*>::Iterator it = m_LoadedModules.end()-1;
        it != m_LoadedModules.begin();
        it--)
        unloadModule(it);

    m_LoadedModules.clear();
    m_Modules.clear();
}

bool KernelElf::moduleIsLoaded(char *name)
{
    for (Vector<Module*>::Iterator it = m_LoadedModules.begin();
        it != m_LoadedModules.end();
        it++)
    {
        Module *module = *it;
        if(!strcmp(module->name, name))
            return true;
    }
    return false;
}

char *KernelElf::getDependingModule(char *name)
{
    for (Vector<Module*>::Iterator it = m_LoadedModules.begin();
        it != m_LoadedModules.end();
        it++)
    {
        Module *module = *it;
        if(module->depends == 0) continue;
        int i = 0;
        while(module->depends[i])
        {
            if(!strcmp(module->depends[i], name))
                return const_cast<char*>(module->name);
            i++;
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
        while(module->depends_opt[i])
        {
            bool found = false;
            for (size_t j = 0; j < m_LoadedModules.count(); ++j)
            {
                if (!strcmp(m_LoadedModules[j]->name, module->depends_opt[i]))
                {
                    found = true;
                    break;
                }
            }

            if(!found)
            {
                for (size_t j = 0; j < m_FailedModules.count(); ++j)
                {
                    if (!strcmp(static_cast<const char *>(*m_FailedModules[j]), module->depends_opt[i]))
                    {
                        found = true;
                        break;
                    }
                }

                // Optional dependency has not yet had any attempt to load.
                if(!found)
                    return false;
            }

            ++i;
        }
    }

    // Second pass: mandatory dependencies.
    i = 0;
    if (!module->depends) return true;

    while (module->depends[i])
    {
        bool found = false;
        for (size_t j = 0; j < m_LoadedModules.count(); j++)
        {
            if (!strcmp(m_LoadedModules[j]->name, module->depends[i]))
            {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        i++;
    }
    return true;
}

bool KernelElf::executeModule(Module *module)
{
    m_LoadedModules.pushBack(module);

    if(module->buffer)
    {
        if (!module->elf.finaliseModule(module->buffer, module->buflen))
        {
            FATAL ("KERNELELF: Module relocation failed");
            return false;
        }

        // Check for a constructors list and execute.
        uintptr_t startCtors = module->elf.lookupSymbol("start_ctors");
        uintptr_t endCtors = module->elf.lookupSymbol("end_ctors");

        if (startCtors && endCtors)
        {
            uintptr_t *iterator = reinterpret_cast<uintptr_t*>(startCtors);
            while (iterator < reinterpret_cast<uintptr_t*>(endCtors))
            {
                void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
                fp();
                iterator++;
            }
        }
    }

    NOTICE("KERNELELF: Executing module " << module->name);

    bool bSuccess = true;
    if (module->entry)
    {
        if(!(bSuccess = module->entry()))
        {
            NOTICE("KERNELELF: Module " << module->name << " failed, unloading.");
            m_FailedModules.pushBack(new String(module->name));
            unloadModule(module->name, true, false);
        }
    }

    if(bSuccess)
        NOTICE("KERNELELF: Module " << module->name << " finished executing");

    return bSuccess;
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

    if ((ret = lookupSymbol(addr, startAddr)))
        return ret;

    // OK, try every module.
    for (Vector<Module*>::Iterator it = m_LoadedModules.begin();
        it != m_LoadedModules.end();
        it++)
    {
        if ((ret = (*it)->elf.lookupSymbol(addr, startAddr)))
            return ret;
    }
    WARNING_NOLOCK("KERNELELF: GlobalLookupSymbol(" << Hex << addr << ") failed.");
    return 0;
}

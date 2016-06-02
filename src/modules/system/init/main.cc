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

#include <compiler.h>
#include <Log.h>
#include <Module.h>
#include <vfs/VFS.h>
#include <subsys/posix/PosixSubsystem.h>
#include <subsys/posix/PosixProcess.h>
#include <core/BootIO.h>
#include <linker/DynamicLinker.h>
#include <users/UserManager.h>

extern void pedigree_init_sigret();
extern void pedigree_init_pthreads();

static void error(const char *s)
{
    extern BootIO bootIO;
    static HugeStaticString str;
    str += s;
    str += "\n";
    bootIO.write(str, BootIO::Red, BootIO::Black);
    str.clear();
}

static int init_stage2(void *param)
{
#if defined(HOSTED) && defined(HAS_ADDRESS_SANITIZER)
    extern void system_reset();
    NOTICE("Note: ASAN build, so triggering a restart now.");
    system_reset();
    return;
#endif

    // Load initial program.
    String fname = String("root»/applications/init");
    File *interpProg = 0;
    File* initProg = VFS::instance().find(fname);
    if (!initProg)
    {
        error("Loading init program FAILED (root»/applications/init not found).");
        return 0;
    }

    NOTICE("INIT: File found");
    NOTICE("INIT: name: " << fname);

    // That will have forked - we don't want to fork, so clear out all the chaff
    // in the new address space that's not in the kernel address space so we
    // have a clean slate.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    pProcess->getSpaceAllocator().clear();
    pProcess->getDynamicSpaceAllocator().clear();
    pProcess->getSpaceAllocator().free(
            pProcess->getAddressSpace()->getUserStart(),
            pProcess->getAddressSpace()->getUserReservedStart() - pProcess->getAddressSpace()->getUserStart());
    if(pProcess->getAddressSpace()->getDynamicStart())
    {
        pProcess->getDynamicSpaceAllocator().free(
            pProcess->getAddressSpace()->getDynamicStart(),
            pProcess->getAddressSpace()->getDynamicEnd() - pProcess->getAddressSpace()->getDynamicStart());
    }
    pProcess->getAddressSpace()->revertToKernelAddressSpace();

    DynamicLinker *pLinker = new DynamicLinker();
    pProcess->setLinker(pLinker);

    // Should we actually load this file, or request another program load the file?
    String interpreter("");
    if(pLinker->checkInterpreter(initProg, interpreter))
    {
        // Switch to the interpreter.
        interpProg = VFS::instance().find(interpreter, pProcess->getCwd());
        if(!interpProg)
        {
            error("Interpreter for init program could not be found.");
            return 0;
        }

        // Using the interpreter - don't worry about dynamic linking.
        delete pLinker;
        pLinker = 0;
        pProcess->setLinker(pLinker);
    }
    else
    {
        error("No interpreter found for the init program.");
        return 0;
    }

    // Map in the ELF we plan on loading.
    uintptr_t elfBaseAddress = 0;
    MemoryMappedObject::Permissions perms = MemoryMappedObject::Read | MemoryMappedObject::Write | MemoryMappedObject::Exec;
    MemoryMappedObject *pElfFile = MemoryMapManager::instance().mapFile(initProg, elfBaseAddress, initProg->getSize(), perms);
    if (!pElfFile)
    {
        error("Memory for the ELF image could not be allocated.");
        return 0;
    }

    uintptr_t originalEntryPoint = 0;
    uintptr_t interpEntryPoint = 0;

    Elf::extractEntryPoint(reinterpret_cast<uint8_t *>(elfBaseAddress), initProg->getSize(), originalEntryPoint);

    size_t phdrCount = 0, phdrEntrySize = 0;
    uintptr_t phdrPointer = 0;
    Elf::extractInformation(reinterpret_cast<uint8_t *>(elfBaseAddress), initProg->getSize(), phdrCount, phdrEntrySize, phdrPointer);

    // Initialise the sigret and pthreads shizzle.
    pedigree_init_sigret();
    pedigree_init_pthreads();

    class RunInitEvent : public Event
    {
    public:
        RunInitEvent(uintptr_t addr) : Event(addr, true)
        {}
        size_t serialize(uint8_t *pBuffer)
        {return 0;}
        size_t getNumber() {return ~0UL;}
    };

    // Load the interpreter proper now.
    uintptr_t interpFileAddress = 0;
    MemoryMappedObject *pInterpFile = MemoryMapManager::instance().mapFile(interpProg, interpFileAddress, interpProg->getSize(), perms);
    if(!pInterpFile)
    {
        error("Memory for the dynamic linker could not be allocated.");
        return 0;
    }

    Elf *pElf = new Elf();
    pElf->create(reinterpret_cast<uint8_t *>(interpFileAddress), interpProg->getSize());
    uintptr_t interpLoadAddress = 0;
    pElf->allocate(reinterpret_cast<uint8_t *>(interpFileAddress), interpProg->getSize(), interpLoadAddress);
    pElf->load(reinterpret_cast<uint8_t *>(interpFileAddress), interpProg->getSize(), interpLoadAddress, 0, 0, ~0, false);

    interpEntryPoint = pElf->getEntryPoint();

    VirtualAddressSpace::Stack *stack = Processor::information().getVirtualAddressSpace().allocateStack();
    uintptr_t *loaderStack = reinterpret_cast<uintptr_t *>(stack->getTop());

#define PUSH(value) *--loaderStack = value
#define PUSH2(value1, value2) PUSH(value2); PUSH(value1)
#define PUSH_COPY(length, value) loaderStack = adjust_pointer(loaderStack, -length); MemoryCopy(loaderStack, value, length)
#define PUSH_ZEROES(length) loaderStack = adjust_pointer(loaderStack, -length); ByteSet(loaderStack, 0, length)

    PUSH_COPY(7, "x86_64");
    void *platform = loaderStack;

    PUSH_ZEROES(16);  // 16 random bytes (but not really).
    void *random = loaderStack;

    // Align to 16 bytes.
    PUSH_ZEROES(16 - (reinterpret_cast<uintptr_t>(loaderStack) & 15));

    // Aux vector.
    /// \todo get the AT_* values from musl defines
    PUSH2(0, 0);  // AT_NULL
    PUSH2(15, reinterpret_cast<uintptr_t>(platform));  // AT_PLATFORM
    PUSH2(25, reinterpret_cast<uintptr_t>(random));  // AT_RANDOM
    PUSH2(23, 0);  // AT_SECURE
    PUSH2(14, 0);  // AT_EGID
    PUSH2(13, 0);  // AT_GID
    PUSH2(12, 0);  // AT_EUID
    PUSH2(11, 0);  // AT_UID
    PUSH2(9, originalEntryPoint);  // AT_ENTRY
    PUSH2(8, 0);  // AT_FLAGS
    PUSH2(7, interpLoadAddress);  // AT_BASE
    PUSH2(6, PhysicalMemoryManager::getPageSize());  // AT_PAGESZ
    PUSH2(5, phdrCount);  // AT_PHNUM
    PUSH2(4, phdrEntrySize);  // AT_PHENT - size of phdr entry
    PUSH2(3, phdrPointer);  // AT_PHDR - base of phdrs

    // env/argv/argc
    //PUSH(0);  // env[0]
    PUSH(0);  // argv[0]
    PUSH(0);  // argc

    Processor::setInterrupts(true);
    pProcess->recordTime(true);

    NOTICE("init: interpreter entry point is " << Hex << interpEntryPoint << ", elf entry is " << originalEntryPoint << "...");
    NOTICE(" -> adjusted: " << Hex << (interpLoadAddress + interpEntryPoint) << ", " << (originalEntryPoint + elfBaseAddress) << "...");

    // Alrighty - lets create a new thread for this program - -8 as PPC assumes
    // the previous stack frame is available...
    NOTICE("spinning up main thread for init now");
    Process::setInit(pProcess);
    Thread *pThread = new Thread(
            pProcess,
            reinterpret_cast<Thread::ThreadStartFunc>(interpEntryPoint + interpLoadAddress),
            0,
            loaderStack);
    pThread->detach();

    return 0;
}

static bool init()
{
#ifdef THREADS
    // Create a new process for the init process.
    Process *pProcess = new PosixProcess(Processor::information().getCurrentThread()->getParent());
    pProcess->setUser(UserManager::instance().getUser(0));
    pProcess->setGroup(UserManager::instance().getUser(0)->getDefaultGroup());
    pProcess->setEffectiveUser(pProcess->getUser());
    pProcess->setEffectiveGroup(pProcess->getGroup());

    pProcess->description().clear();
    pProcess->description().append("init");

    pProcess->setCwd(VFS::instance().find(String("root»/")));
    pProcess->setCtty(0);

    PosixSubsystem *pSubsystem = new PosixSubsystem;
    pProcess->setSubsystem(pSubsystem);

    Thread *pThread = new Thread(pProcess, init_stage2, 0);
    pThread->detach();
#endif

    return true;
}

static void destroy()
{
}

#if defined(X86_COMMON)
#define __MOD_DEPS "vfs", "posix", "linker", "users"
#define __MOD_DEPS_OPT "gfx-deps", "mountroot"
#else
#define __MOD_DEPS "vfs", "posix", "linker", "users"
#define __MOD_DEPS_OPT "mountroot"
#endif
MODULE_INFO("init", &init, &destroy, __MOD_DEPS);
#ifdef __MOD_DEPS_OPT
MODULE_OPTIONAL_DEPENDS(__MOD_DEPS_OPT);
#endif

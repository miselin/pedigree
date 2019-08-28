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

#include "modules/Module.h"
#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/core/BootIO.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"

class File;

static Mutex g_Started(false);

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
#if HOSTED && HAS_ADDRESS_SANITIZER
    extern void system_reset();
    NOTICE("Note: ASAN build, so triggering a restart now.");
    system_reset();
    return;
#endif

    bool tryingLinux = false;

    File *file = 0;

    String init_path("root»/applications/init");
    NOTICE("Searching for init program at " << init_path);
    file = VFS::instance().find(init_path);
    if (!file)
    {
        WARNING(
            "Did not find " << init_path
                            << ", trying for a Linux userspace...");
        init_path = "root»/sbin/init";
        tryingLinux = true;

        NOTICE("Searching for Linux init at " << init_path);
        file = VFS::instance().find(init_path);
        if (!file)
        {
            error("failed to find init program (tried root»/applications/init and root»/sbin/init)");
        }
    }

    NOTICE("Found an init program at " << init_path);

    Vector<String> argv, env;
    argv.pushBack(init_path);

    if (tryingLinux)
    {
        // Jump to runlevel 5
        argv.pushBack(String("5"));
    }

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    if (!pProcess->getSubsystem()->invoke(file, init_path, argv, env))
    {
        error("failed to load init program");
    }

    Process::setInit(pProcess);

    g_Started.release();

    return 0;
}

static bool init()
{
#if THREADS
    g_Started.acquire();

    // Create a new process for the init process.
    PosixProcess *pProcess = new PosixProcess(
        Processor::information().getCurrentThread()->getParent());

    pProcess->setUserId(0);
    pProcess->setGroupId(0);
    pProcess->setEffectiveUserId(0);
    pProcess->setEffectiveGroupId(0);
    pProcess->setSavedUserId(0);
    pProcess->setSavedGroupId(0);

    pProcess->description() = "init";
    pProcess->setCwd(VFS::instance().find(String("root»/")));
    pProcess->setCtty(0);

    PosixSubsystem *pSubsystem = new PosixSubsystem;
    pProcess->setSubsystem(pSubsystem);

    // add an empty stdout, stdin
    File *pNull = VFS::instance().find(String("dev»/null"));
    if (!pNull)
    {
        error("dev»/null does not exist");
        return false;
    }

    FileDescriptor *stdinDescriptor = new FileDescriptor(pNull, 0, 0, 0, 0);
    FileDescriptor *stdoutDescriptor = new FileDescriptor(pNull, 0, 1, 0, 0);

    pSubsystem->addFileDescriptor(0, stdinDescriptor);
    pSubsystem->addFileDescriptor(1, stdoutDescriptor);

    Thread *pThread = new Thread(pProcess, init_stage2, 0);
    pThread->detach();

    // wait for the other process to start before we move on with startup
    g_Started.acquire();
#endif

    return true;
}

static void destroy()
{
}

#if X86_COMMON
#define __MOD_DEPS "vfs", "posix", "linker", "users"
#define __MOD_DEPS_OPT "gfx-deps", "mountroot", "confignics"
#else
#define __MOD_DEPS "vfs", "posix", "linker", "users"
#define __MOD_DEPS_OPT "mountroot", "confignics"
#endif
MODULE_INFO("init", &init, &destroy, __MOD_DEPS);
#ifdef __MOD_DEPS_OPT
MODULE_OPTIONAL_DEPENDS(__MOD_DEPS_OPT);
#endif

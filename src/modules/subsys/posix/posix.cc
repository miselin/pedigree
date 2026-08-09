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

#include "DevFs.h"
#include "PosixSyscallManager.h"
#include "ProcFs.h"
#include "UnixFilesystem.h"
#include "modules/Module.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/VFS.h"
#include "net-syscalls.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "signal-syscalls.h"
#include "system-syscalls.h"

static PosixSyscallManager g_PosixSyscallManager;

UnixFilesystem *g_pUnixFilesystem = 0;
static RamFs *g_pRunFilesystem = 0;

DevFs *g_pDevFs = 0;
static ProcFs *g_pProcFs = 0;

static bool init()
{
    if (!g_PosixSyscallManager.initialise())
    {
        return false;
    }

    g_pDevFs = new DevFs();
    g_pDevFs->initialise(0);

    g_pProcFs = new ProcFs();
    g_pProcFs->initialise(0);

    g_pUnixFilesystem = new UnixFilesystem();

    g_pRunFilesystem = new RamFs;
    g_pRunFilesystem->initialise(0);
    VFS::instance().registerFilesystem(
        g_pRunFilesystem, String("posix-runtime"));
    VFS::instance().registerFilesystem(g_pUnixFilesystem, String("unix"));
    VFS::instance().registerFilesystem(g_pDevFs, String("dev"));
    VFS::instance().registerFilesystem(g_pProcFs, String("proc"));

    Filesystem *scratchfs =
        VFS::instance().getFilesystemAt(String("/media/scratch"));

    // Keep the socket namespace separate from ordinary runtime files while
    // exposing it at a conventional path.
    VFS::instance().createDirectory(
        String("/media/posix-runtime/sockets"), 0755);

    // Expose the system filesystems at their conventional FHS locations. Their
    // primary mount records remain visible under /media.
    struct reparse
    {
        String path;
        File *target;
    } reparses[] = {
        {String("/dev"), g_pDevFs->getRoot()},
        {String("/run"), g_pRunFilesystem->getRoot()},
        {String("/run/sockets"), g_pUnixFilesystem->getRoot()},
        {String("/var/run"), g_pRunFilesystem->getRoot()},
        {String("/proc"), g_pProcFs->getRoot()},
        {String("/tmp"), scratchfs ? scratchfs->getRoot() : 0},
    };

    for (auto &p : reparses)
    {
        if (!p.target)
        {
            continue;
        }

        File *point = VFS::instance().find(p.path);
        if (point && point->isDirectory())
        {
            Directory *pDir = Directory::fromFile(point);
            pDir->setReparsePoint(Directory::fromFile(p.target));
        }
    }

    return true;
}

static void destroy()
{
    if (!g_PosixSyscallManager.shutdown())
    {
        FATAL("POSIX syscall handlers could not be retired safely.");
    }

    const char *reparsePaths[] = {
        "/run/sockets", "/var/run", "/run", "/proc", "/dev"};
    for (const char *path : reparsePaths)
    {
        File *point = VFS::instance().find(String(path));
        if (point && point->isDirectory())
        {
            Directory::fromFile(point)->setReparsePoint(nullptr);
        }
    }

    VFS::instance().unregisterFilesystem(g_pProcFs, false);
    VFS::instance().unregisterFilesystem(g_pDevFs, false);
    VFS::instance().unregisterFilesystem(g_pUnixFilesystem, false);
    VFS::instance().unregisterFilesystem(g_pRunFilesystem, false);

    delete g_pRunFilesystem;
    delete g_pUnixFilesystem;
    delete g_pProcFs;
    delete g_pDevFs;
}

MODULE_INFO(
    "posix", &init, &destroy, "console", "network-stack", "mountroot", "lwip");

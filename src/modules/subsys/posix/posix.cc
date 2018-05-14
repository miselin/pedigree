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
#include "signal-syscalls.h"
#include "system-syscalls.h"

static PosixSyscallManager g_PosixSyscallManager;

UnixFilesystem *g_pUnixFilesystem = 0;
static RamFs *g_pRunFilesystem = 0;

DevFs *g_pDevFs = 0;
static ProcFs *g_pProcFs = 0;

static bool init()
{
    g_PosixSyscallManager.initialise();

    g_pDevFs = new DevFs();
    g_pDevFs->initialise(0);

    g_pProcFs = new ProcFs();
    g_pProcFs->initialise(0);

    g_pUnixFilesystem = new UnixFilesystem();

    g_pRunFilesystem = new RamFs;
    g_pRunFilesystem->initialise(0);
    VFS::instance().addAlias(g_pRunFilesystem, String("posix-runtime"));

    VFS::instance().addAlias(
        g_pUnixFilesystem, g_pUnixFilesystem->getVolumeLabel());
    VFS::instance().addAlias(g_pDevFs, g_pDevFs->getVolumeLabel());
    VFS::instance().addAlias(g_pProcFs, g_pProcFs->getVolumeLabel());

    Filesystem *scratchfs = VFS::instance().lookupFilesystem("scratch");

    // Set up default reparse points. normalisePath in file-syscalls.cc is not
    // sufficient in many cases, as it requires matching the _entire_ path to
    // actually work. Reparse points work a lot better and they let us override
    // the directory layout that already exists on disk. If the directory
    // doesn't exist on disk, we won't add a reparse point for it here.
    struct reparse
    {
        const char *path;
        File *target;
    } reparses[] = {
        // {"root»/dev", g_pDevFs->getRoot()},
        {"root»/var/run", g_pRunFilesystem->getRoot()},
        {"root»/proc", g_pProcFs->getRoot()},
        {"root»/tmp", scratchfs ? scratchfs->getRoot() : 0},
    };

    for (auto p : reparses)
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
    VFS::instance().removeAllAliases(g_pProcFs);
    VFS::instance().removeAllAliases(g_pDevFs);
    VFS::instance().removeAllAliases(g_pUnixFilesystem);
    VFS::instance().removeAllAliases(g_pRunFilesystem);
}

#ifdef ARM_COMMON
MODULE_INFO("posix", &init, &destroy, "console", "mountroot");
#else
MODULE_INFO(
    "posix", &init, &destroy, "console", "network-stack", "mountroot", "lwip");
#endif

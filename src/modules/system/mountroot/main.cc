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
#include "modules/system/lodisk/LoDisk.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/core/BootIO.h"
#include "pedigree/kernel/machine/Disk.h"
#include "modules/system/ramfs/RamFs.h"

static bool bRootMounted = false;

static void error(const char *s)
{
    extern BootIO bootIO;
    static HugeStaticString str;
    str += s;
    str += "\n";
    bootIO.write(str, BootIO::Red, BootIO::Black);
    str.clear();
}

static Device *probeDisk(Device *diskDevice)
{
    if (diskDevice->getType() != Device::Disk)
    {
        return diskDevice;
    }

    Disk *pDisk = static_cast<Disk *>(diskDevice);
    String alias;  // Null - gets assigned by the filesystem.
    if (VFS::instance().mount(pDisk, alias))
    {
        // For mount message
        bool didMountAsRoot = false;

        // Search for the root specifier, if we haven't already mounted root
        if (!bRootMounted)
        {
            NormalStaticString s;
            s += alias;
            s += "»/.pedigree-root";

            File *f =
                VFS::instance().find(String(static_cast<const char *>(s)));
            if (f && !bRootMounted)
            {
                NOTICE("Mounted " << alias << " successfully as root.");
                VFS::instance().addAlias(alias, String("root"));
                bRootMounted = didMountAsRoot = true;
            }
        }

        if (!didMountAsRoot)
        {
            NOTICE("Mounted " << alias << ".");
        }
    }

    return diskDevice;
}

static bool init()
{
    // Mount scratch filesystem (ie, pure ram filesystem, for POSIX /tmp etc)
    RamFs *pRamFs = new RamFs;
    pRamFs->initialise(0);
    VFS::instance().addAlias(pRamFs, String("scratch"));

    // Mount runtime filesystem.
    // The runtime filesystem assigns a Process ownership to each file, only
    // that process can modify/remove it. If the Process terminates without
    // removing the file, the file is not removed.
    RamFs *pRuntimeFs = new RamFs;
    pRuntimeFs->initialise(0);
    pRuntimeFs->setProcessOwnership(true);
    VFS::instance().addAlias(pRuntimeFs, String("runtime"));

    // Mount all available filesystems.
    Device::foreach (probeDisk);

    if (VFS::instance().find(String("raw»/")) == 0)
    {
        error("raw» does not exist - cannot continue startup.");
        return false;
    }

    // Are we running a live CD?
    /// \todo Use the configuration manager to determine if we're running a live
    /// CD or
    ///       not, to avoid the potential for conflicts here.
    if (VFS::instance().find(String("root»/livedisk.img")))
    {
        NOTICE("trying to find live disk");
        FileDisk *pRamDisk =
            new FileDisk(String("root»/livedisk.img"), FileDisk::RamOnly);
        if (pRamDisk && pRamDisk->initialise())
        {
            NOTICE("have a live disk");
            Device::addToRoot(pRamDisk);

            // Mount it in the VFS
            VFS::instance().removeAlias(String("root"));
            bRootMounted = false;
            NOTICE("probing ram disk for partitions");
            Device::foreach (probeDisk, pRamDisk);
        }
        else
            delete pRamDisk;
    }

    // Is there a root disk mounted?
    if (VFS::instance().find(String("root»/.pedigree-root")) == 0)
    {
        error("No root disk on this system (no root»/.pedigree-root found).");
        return false;
    }

    // All done, nothing more to do here.
    return true;
}

static void destroy()
{
    NOTICE("Unmounting all filesystems...");

    Tree<Filesystem *, List<String *> *> &mounts = VFS::instance().getMounts();
    List<Filesystem *> deletionQueue;

    for (auto it = mounts.begin(); it != mounts.end(); ++it)
    {
        deletionQueue.pushBack(it.key());
    }

    while (deletionQueue.count())
    {
        Filesystem *pFs = deletionQueue.popFront();
        NOTICE(
            "Unmounting " << pFs->getVolumeLabel() << " [" << Hex << pFs
                          << "]...");
        VFS::instance().removeAllAliases(pFs);
        NOTICE("unmount done");
    }

    NOTICE("Unmounting all filesystems has completed.");
}

MODULE_INFO("mountroot", &init, &destroy, "vfs", "partition");

// We expect the filesystems metamodule to fail, but by the time it does and
// we are allowed to continue, all the filesystems are loaded.
MODULE_OPTIONAL_DEPENDS("filesystems");

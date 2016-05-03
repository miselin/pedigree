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

#include <devfs/FramebufferFile.h>
#include <devfs/NullFile.h>
#include <devfs/RandomFile.h>

#include <utilities/utility.h>

DevFs DevFs::m_Instance;

bool DevFs::initialise(Disk *pDisk)
{
    // Deterministic inode assignment to each devfs node
    size_t baseInode = 0;

    m_pRoot = new DevFsDirectory(String(""), 0, 0, 0, ++baseInode, this, 0, 0);

    // Create /dev/null and /dev/zero nodes
    NullFile *pNull = new NullFile(String("null"), ++baseInode, this, m_pRoot);
    ZeroFile *pZero = new ZeroFile(String("zero"), ++baseInode, this, m_pRoot);
    m_pRoot->addEntry(pNull->getName(), pNull);
    m_pRoot->addEntry(pZero->getName(), pZero);

    // Create /dev/urandom for the RNG.
    RandomFile *pRandom = new RandomFile(String("urandom"), ++baseInode, this, m_pRoot);
    m_pRoot->addEntry(pRandom->getName(), pRandom);

    // Create /dev/fb for the framebuffer device.
    FramebufferFile *pFb = new FramebufferFile(String("fb"), ++baseInode, this, m_pRoot);
    if(pFb->initialise())
        m_pRoot->addEntry(pFb->getName(), pFb);
    else
    {
        WARNING("POSIX: no /dev/fb - framebuffer failed to initialise.");
        --baseInode;
        delete pFb;
    }

    // Create PTYs.
    createPtyNodes(this, m_pRoot, baseInode);

    return true;
}

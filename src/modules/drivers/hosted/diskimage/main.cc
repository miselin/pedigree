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

#include "DiskImage.h"
#include "modules/Module.h"

namespace
{
DiskImage *g_DiskImage = nullptr;

Device *removeDiskImage(Device *device)
{
    if (device != g_DiskImage)
    {
        return device;
    }

    g_DiskImage = nullptr;
    return nullptr;
}
}  // namespace

bool entry()
{
    if (g_DiskImage)
    {
        return true;
    }

    g_DiskImage = new DiskImage();
    if (!g_DiskImage->initialise())
    {
        delete g_DiskImage;
        g_DiskImage = nullptr;

        // Don't mess up the rest of the startup - we may still be able to
        // run without a disk.
        return true;
    }

    Device::addToRoot(g_DiskImage);
    return true;
}

void exit()
{
    if (!g_DiskImage)
    {
        return;
    }

    Device::foreach(removeDiskImage);
    if (g_DiskImage)
    {
        delete g_DiskImage;
        g_DiskImage = nullptr;
    }
}

MODULE_INFO("diskimage", &entry, &exit);

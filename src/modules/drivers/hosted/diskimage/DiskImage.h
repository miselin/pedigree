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

#ifndef DISKIMAGE_H
#define DISKIMAGE_H

#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/Cache.h"

/** Loads a disk image as a usable disk device. */
class DiskImage : public Disk
{
  public:
    DiskImage()
        : Disk(), m_pBase(0), m_nSize(0), m_Cache(), m_CacheLock(),
          m_nAlignPoints(0)
    {
    }

    virtual ~DiskImage();

    bool initialise();

    virtual void getName(String &str)
    {
        str.assign("Hosted disk image");
    }

    virtual void dump(String &str)
    {
        str.assign("Hosted disk image");
    }

    virtual uintptr_t read(uint64_t location);

    virtual size_t getSize() const;

    virtual size_t getBlockSize() const
    {
        return 0x10000;
    }

    virtual void align(uint64_t location);

    virtual bool pin(uint64_t location);

    virtual void unpin(uint64_t location);

  private:
    uint64_t getAlignmentPoint(uint64_t location) const;
    uint64_t getPageLocation(uint64_t location) const;

    void *m_pBase;
    size_t m_nSize;

    Cache m_Cache;
    Mutex m_CacheLock;
    uint64_t m_AlignPoints[8];
    size_t m_nAlignPoints;
};

#endif

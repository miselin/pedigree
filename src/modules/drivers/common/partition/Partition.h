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

#ifndef PARTITION_H
#define PARTITION_H

#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"

/**
 * A partition is a chunk of disk.
 */
class Partition : public Disk
{
  public:
    Partition(const String &type, uint64_t start, uint64_t length);
    virtual ~Partition();

    virtual void getName(String &str)
    {
        NormalStaticString str2;
        str2 += m_Type;
        str2 += " partition";
        str.assign(str2, str2.length());
    }

    virtual void dump(String &str)
    {
        LargeStaticString str2;
        str2 += m_Type;
        str2 += " partition at 0x";
        str2.append(m_Start, 16);
        str2 += "-";
        str2.append(m_Start + m_Length, 16);
        str.assign(str2, str2.length());
    }

    virtual uintptr_t read(uint64_t location)
    {
        // Ensure the read does not begin past the end of our partition
        if (location > m_Length)
            return 0;
        else if ((location + 0x1000) > m_Length)
            return 0;

        Disk *pParent = static_cast<Disk *>(getParent());

        if (!m_bAligned)
        {
            m_bAligned = true;
            // Ensure that we get blocks aligned on our start position (which is
            // quite likely to not be on a 4096-byte boundary).
            pParent->align(m_Start);
        }

        return pParent->read(location + m_Start);
    }

    virtual void write(uint64_t location)
    {
        // Ensure the read does not begin past the end of our partition
        if (location > m_Length)
            return;
        else if ((location + 0x1000) > m_Length)
            return;

        Disk *pParent = static_cast<Disk *>(getParent());

        if (!m_bAligned)
        {
            m_bAligned = true;
            // Ensure that we get blocks aligned on our start position (which is
            // quite likely to not be on a 4096-byte boundary).
            pParent->align(m_Start);
        }

        pParent->write(location + m_Start);
    }

    virtual size_t getSize() const
    {
        return getLength();
    }

    virtual size_t getBlockSize() const
    {
        const Disk *pParent = static_cast<const Disk *>(getParent());
        return pParent->getBlockSize();
    }

    /** Returns the first byte of the parent disk that is in this partition. */
    uint64_t getStart();

    /** Returns the length of this partition. */
    uint64_t getLength() const
    {
        return m_Length;
    }

    /** Returns a string giving the type of the partition. */
    const String &getPartitionType() const
    {
        return m_Type;
    }

  private:
    String m_Type;
    uint64_t m_Start;
    uint64_t m_Length;
    bool m_bAligned;
};

#endif

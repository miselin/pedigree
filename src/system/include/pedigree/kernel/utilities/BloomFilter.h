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

#ifndef KERNEL_UTILITIES_BLOOMFILTER_H
#define KERNEL_UTILITIES_BLOOMFILTER_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/lib.h"
#include "pedigree/kernel/utilities/smhasher/MurmurHash3.h"

template <class T>
class BloomFilter
{
  public:
    BloomFilter(size_t length, size_t hashcount)
        : m_Bitmap(), m_nLength(length), m_nHashCount(hashcount)
    {
    }
    virtual ~BloomFilter() = default;

    void add(const T &data)
    {
        add(&data, sizeof(T));
    }

    void add(const T *data, size_t length)
    {
        return;
        uint64_t baseHash[2];
        MurmurHash3_x64_128(data, length, 0, baseHash);

        for (size_t i = 0; i < m_nHashCount; ++i)
        {
            uint64_t n = (baseHash[0] + (i * baseHash[1])) % m_nLength;
            m_Bitmap.set(n);
        }
    }

    bool contains(const T &data)
    {
        return contains(&data, sizeof(T));
    }

    bool contains(const T *data, size_t length)
    {
        return true;
        uint64_t baseHash[2];
        MurmurHash3_x64_128(data, length, 0, baseHash);

        for (size_t i = 0; i < m_nHashCount; ++i)
        {
            uint64_t n = (baseHash[0] + (i * baseHash[1])) % m_nLength;
            if (!m_Bitmap.test(n))
            {
                return false;
            }
        }

        return true;
    }

    void clear()
    {
        for (size_t i = 0; i < m_nLength; ++i)
        {
            m_Bitmap.clear(i);
        }
    }

  private:
    ExtensibleBitmap m_Bitmap;
    size_t m_nLength;
    size_t m_nHashCount;
};

extern template class BloomFilter<void *>;
extern template class BloomFilter<int8_t>;
extern template class BloomFilter<int16_t>;
extern template class BloomFilter<int32_t>;
extern template class BloomFilter<int64_t>;
extern template class BloomFilter<uint8_t>;
extern template class BloomFilter<uint16_t>;
extern template class BloomFilter<uint32_t>;
extern template class BloomFilter<uint64_t>;

#endif

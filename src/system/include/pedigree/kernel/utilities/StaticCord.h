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

#ifndef KERNEL_UTILITIES_STATICCORD_H
#define KERNEL_UTILITIES_STATICCORD_H

/** @addtogroup kernelutilities
 * @{ */

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/template.h"  // IWYU pragma: keep
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/utilities/String.h"

/**
 * StaticCord is a variant of Cord which avoids allocating memory at the
 * expense of features.
 */
template <unsigned int N>
class EXPORTED_PUBLIC StaticCord
{
    friend class CordIterator;
  public:
    class CordIterator
    {
        friend class StaticCord;
        public:
            CordIterator(const StaticCord &owner) : cord(owner), segment(0), index(0)
            {
            }
            virtual ~CordIterator() = default;

            CordIterator &operator++()
            {
                ++index;
                if (index >= cord.m_Segments[segment].length)
                {
                    index = 0;
                    ++segment;
                }

                if (segment > cord.m_NumSegments)
                {
                    segment = cord.m_NumSegments;
                    index = 0;
                }

                return *this;
            }

            CordIterator &operator--()
            {
                if (index)
                {
                    --index;
                }
                else if (segment)
                {
                    --segment;
                    index = cord.m_Segments[segment].length;
                }

                return *this;
            }

            char operator*() const
            {
                return cord.m_Segments[segment].ptr[index];
            }

            bool operator==(const CordIterator &other) const
            {
                return segment == other.segment && index == other.index;
            }

            bool operator!=(const CordIterator &other) const
            {
                return !(*this == other);
            }

        protected:
            CordIterator(const StaticCord &owner, bool end) : cord(owner), segment(0), index(0)
            {
                if (end)
                {
                    segment = owner.m_NumSegments;
                }
            }

        private:
            const StaticCord &cord;
            size_t segment;
            size_t index;
    };

    class CordSegmentIterator
    {
        friend class StaticCord;
        public:
            CordSegmentIterator(const StaticCord &owner) : cord(owner), segment(0)
            {
            }
            virtual ~CordSegmentIterator() = default;

            CordSegmentIterator &operator++()
            {
                ++segment;

                if (segment > cord.m_NumSegments)
                {
                    segment = cord.m_NumSegments;
                }

                return *this;
            }

            CordSegmentIterator &operator--()
            {
                if (segment)
                {
                    --segment;
                }

                return *this;
            }

            const char *ptr() const
            {
                return cord.m_Segments[segment].ptr;
            }

            size_t length() const
            {
                return cord.m_Segments[segment].length;
            }

            bool operator==(const CordSegmentIterator &other) const
            {
                return segment == other.segment;
            }

            bool operator!=(const CordSegmentIterator &other) const
            {
                return !(*this == other);
            }

        protected:
            CordSegmentIterator(const StaticCord &owner, bool end) : cord(owner), segment(0)
            {
                if (end)
                {
                    segment = owner.m_NumSegments;
                }
            }

        private:
            const StaticCord &cord;
            size_t segment;
    };

    StaticCord() = default;
    StaticCord(const StaticCord &other) : StaticCord()
    {
        assign(other);
    }
    virtual ~StaticCord() = default;

    StaticCord &operator=(const StaticCord &s)
    {
        assign(s);
        return *this;
    }

    void assign(const StaticCord &other)
    {
        m_Length = other.m_Length;
        m_NumSegments = other.m_NumSegments;
        pedigree_std::copy(m_Segments, other.m_Segments, N);
    }

    void clear()
    {
        m_NumSegments = 0;
        m_Length = 0;
    }

    size_t length() const
    {
        return m_Length;
    }

    char operator[](size_t index) const
    {
        size_t offset = 0;
        for (size_t i = 0; i < m_NumSegments; ++i)
        {
            if ((index >= offset) && (index < (offset + m_Segments[i].length)))
            {
                return m_Segments[i].ptr[index - offset];
            }

            offset += m_Segments[i].length;
        }

        return 0;
    }

    String toString() const
    {
        char *buf = new char[m_Length + 1];
        size_t offset = 0;
        for (size_t i = 0; i < m_NumSegments; ++i)
        {
            MemoryCopy(buf + offset, m_Segments[i].ptr, m_Segments[i].length);
            offset += m_Segments[i].length;
        }

        buf[m_Length] = 0;

        String result(buf, m_Length);
        delete [] buf;
        return result;
    }

    void append(const char *s, size_t len=0)
    {
        assert(m_NumSegments < N);

        if (!len)
        {
            len = StringLength(s);
        }

        m_Segments[m_NumSegments++] = CordSegment(s, len);
        m_Length += len;
    }

    CordIterator begin() const
    {
        return CordIterator(*this);
    }

    CordIterator end() const
    {
        return CordIterator(*this, true);
    }

    CordSegmentIterator segbegin() const
    {
        return CordSegmentIterator(*this);
    }

    CordSegmentIterator segend() const
    {
        return CordSegmentIterator(*this, true);
    }

  private:
    struct CordSegment
    {
        CordSegment() = default;
        CordSegment(const char *s, size_t len) : ptr(s), length(len) {}

        const char *ptr = nullptr;
        size_t length = 0;
    };

    CordSegment m_Segments[N];
    size_t m_Length = 0;
    size_t m_NumSegments = 0;
};

// Specializations
extern template class EXPORTED_PUBLIC StaticCord<8>; // IWYU pragma: keep

/** @} */

#endif  // KERNEL_UTILITIES_STATICCORD_H

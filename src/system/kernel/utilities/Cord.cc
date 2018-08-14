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

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/List.h"

Cord::Cord() = default;

Cord::Cord(const Cord &other) : Cord()
{
    assign(other);
}

Cord::~Cord()
{
    clear();
}

Cord &Cord::operator=(const Cord &s)
{
    assign(s);
    return *this;
}

void Cord::reserve(size_t segments)
{
    m_Segments.reserve(segments, true);
}

void Cord::assign(const Cord &other)
{
    clear();

    m_Segments.reserve(other.m_Segments.count(), false);

    for (auto &it : other.m_Segments)
    {
        m_Segments.pushBack(it);
        m_Length += it.length;
    }
}

void Cord::clear()
{
    m_Segments.clear(false);
}

size_t Cord::length() const
{
    return m_Length;
}

String Cord::toString() const
{
    char *buf = new char[m_Length + 1];
    size_t offset = 0;
    for (auto &it : m_Segments)
    {
        MemoryCopy(buf + offset, it.ptr, it.length);
        offset += it.length;
    }

    buf[m_Length] = 0;

    String result(buf, m_Length);
    delete [] buf;
    return result;
}

char Cord::operator[](size_t index) const
{
    size_t i = 0;
    for (auto &it : m_Segments)
    {
        if ((index >= i) && (index < (i + it.length)))
        {
            return it.ptr[index - i];
        }

        i += it.length;
    }

    /// \todo should this be more crashy?
    return 0;
}

void Cord::append(const char *s, size_t len)
{
    if (!len)
    {
        len = StringLength(s);
    }

    m_Segments.pushBack(CordSegment(s, len));
    m_Length += len;
}

void Cord::prepend(const char *s, size_t len)
{
    if (!len)
    {
        len = StringLength(s);
    }

    m_Segments.pushFront(CordSegment(s, len));
    m_Length += len;
}

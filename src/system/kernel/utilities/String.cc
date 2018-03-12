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

#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "pedigree/kernel/Log.h"

String::String()
    : m_Data(0), m_ConstData(nullptr), m_Length(0), m_Size(StaticSize),
      m_HeapData(true), m_Hash(0)
{
    m_Static[0] = '\0';
}

String::String(const char *s) : String()
{
    assign(s);
}

String::String(const char *s, size_t length) : String()
{
    assign(s, length);
}

String::String(const String &x) : String()
{
    assign(x);
}

String::String(String &&x)
{
    // take ownership of the object
    m_Data = pedigree_std::move(x.m_Data);
    m_ConstData = pedigree_std::move(x.m_ConstData);
    m_Length = pedigree_std::move(x.m_Length);
    m_Size = pedigree_std::move(x.m_Size);
    m_HeapData = pedigree_std::move(x.m_HeapData);
    m_Hash = pedigree_std::move(x.m_Hash);
    if (m_Size == StaticSize)
    {
        MemoryCopy(m_Static, x.m_Static, m_Length + 1);
    }

    // free other string but don't destroy the heap pointer if we had one
    // as it is now owned by this new instance
    x.m_Data = 0;
    x.free();
}

String::~String()
{
    free();
}

String &String::operator=(const String &x)
{
    assign(x);
    return *this;
}

String &String::operator=(const char *s)
{
    assign(s);
    return *this;
}

String &String::operator+=(const String &x)
{
    // Switch from const to dynamic string.
    if (!m_HeapData)
    {
        assign(m_ConstData, m_Length);
    }

    size_t newLength = x.length() + m_Length;

    char *dst = m_Static;

    // Do we need to transfer static into dynamic for this?
    if (newLength >= StaticSize)
    {
        reserve(newLength + 1);
        if (m_Length < StaticSize)
            MemoryCopy(m_Data, m_Static, m_Length);
        dst = m_Data;
    }

    const char *src = x.m_Static;
    if (x.length() > StaticSize)
        src = x.m_Data;

    // Copy!
    MemoryCopy(&dst[m_Length], src, x.length() + 1);
    m_Length += x.length();
    computeHash();
    return *this;
}

String &String::operator+=(const char *s)
{
    // Switch from const to dynamic string.
    if (!m_HeapData)
    {
        assign(m_ConstData, m_Length);
    }

    size_t slen = StringLength(s);
    size_t newLength = slen + m_Length;
    if (newLength < StaticSize)
    {
        // By the nature of the two lengths combined being below the static
        // size, we can be assured that we can use the static buffer in
        // both strings.
        MemoryCopy(&m_Static[m_Length], s, slen + 1);
    }
    else
    {
        reserve(slen + m_Length + 1);
        if (m_Length < StaticSize)
            MemoryCopy(m_Data, m_Static, m_Length);
        MemoryCopy(&m_Data[m_Length], s, slen + 1);
    }

    m_Length += slen;
    computeHash();
    return *this;
}

bool String::operator==(const String &s) const
{
    if (m_Length != s.m_Length)
    {
        return false;
    }
    else if (m_Hash != s.m_Hash)
    {
        // precomputed hash didn't match, don't bother
        return false;
    }

    const char *buf = extract();
    const char *other_buf = s.extract();

    // Neither of these can be null because of the above conditions.
    return !StringCompareN(buf, other_buf, m_Length + 1);
}

bool String::operator==(const char *s) const
{
    const char *buf = extract();

    if ((!m_Length) && (s == 0))
    {
        return true;
    }
    else if (s == 0)
    {
        // m_Length > 0 but other buffer is null.
        return false;
    }
    else if ((!m_Length) && *s)
    {
        // Quick check when we're zero-length.
        return false;
    }
    else
    {
        return StringCompareN(buf, s, m_Length + 1) == 0;
    }
}

size_t String::nextCharacter(size_t c)
{
    // UTF-8 version of getting the next character
    const char *buf = extract();
    const uint8_t *u8buf = reinterpret_cast<const uint8_t *>(buf);
    if ((buf[c] & 0xC0) == 0xC0)
    {
        if ((buf[c] & 0xF8) == 0xF0)
        {
            return c + 4;  // 4-byte sequence
        }
        else if ((buf[c] & 0xF0) == 0xE0)
        {
            return c + 3;
        }
        else
        {
            return c + 2;
        }
    }
    return c + 1;
}

size_t String::prevCharacter(size_t c)
{
    // TODO handle multibyte chars.
    return c - 1;
}

void String::assign(const String &x)
{
    m_Length = x.length();
    if (m_Length < StaticSize)
    {
        MemoryCopy(m_Static, x.m_Static, m_Length + 1);
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
        m_Size = StaticSize;
    }
    else
    {
        // Length is bigger than a static buffer, no need to check for empty
        // buffer.
        reserve(m_Length + 1, false);
        MemoryCopy(m_Data, x.m_Data, m_Length + 1);
    }

    m_HeapData = true;
// m_ConstData = nullptr;

    // no need to recompute in this case
    m_Hash = x.m_Hash;

#ifdef ADDITIONAL_CHECKS
    if (*this != x)
    {
        ERROR("mismatch: '" << *this << "' != '" << x << "'");
        ERROR("const data was " << m_ConstData);
    }
    assert(*this == x);
#endif
}

void String::assign(const char *s, size_t len, bool unsafe)
{
    size_t copyLength = 0;
    // len overrides all other optimizations
    if (len)
    {
        // Fix up length if the passed string is much smaller than the 'len'
        // parameter (otherwise we think we have a giant string).
        if (!unsafe)
        {
            size_t trueLength = StringLength(s);
            if (trueLength < len)
            {
                len = trueLength;
            }
        }
        m_Length = len;
        copyLength = len;
    }
    else  if (!s || !*s)
    {
        m_Length = 0;
    }
    else
    {
        m_Length = StringLength(s);
        copyLength = m_Length;
    }

    if (!m_Length)
    {
        ByteSet(m_Static, 0, StaticSize);
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
        m_Size = StaticSize;
    }
    else if (m_Length < StaticSize)
    {
        MemoryCopy(m_Static, s, copyLength);
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
        m_Size = StaticSize;
        m_Static[copyLength] = '\0';
    }
    else
    {
        reserve(m_Length + 1, false);
        MemoryCopy(m_Data, s, copyLength);
        m_Data[copyLength] = '\0';
    }

    m_HeapData = true;
    m_ConstData = nullptr;

#ifdef ADDITIONAL_CHECKS
    if (!len)
    {
        assert(*this == s);
    }
#endif

    computeHash();
}

void String::reserve(size_t size)
{
    reserve(size, true);
}

void String::reserve(size_t size, bool zero)
{
    // Don't reserve if we're a static string.
    if (size <= StaticSize)
    {
        if (m_Size > StaticSize)
        {
            m_Size = StaticSize;
            MemoryCopy(m_Static, m_Data, size);
            if (m_HeapData)
            {
                delete[] m_Data;
            }
            m_Data = 0;
        }

        return;
    }
    else if (size > m_Size)
    {
        char *tmp = m_Data;
        m_Data = new char[size];
        if (tmp)
        {
            MemoryCopy(m_Data, tmp, m_Size > size ? size : m_Size);
            if (m_HeapData)
            {
                delete[] tmp;
            }
        }
        else if (zero)
        {
            ByteSet(m_Data, 0, size);
        }
        m_Size = size;
    }
}
void String::free()
{
    if (m_HeapData)
    {
        delete[] m_Data;
    }
    m_Static[0] = '\0';  /// \note free does not clear old static data
    m_Data = 0;
    m_Length = 0;
    m_Size = 0;
    m_Hash = 0;
}

String String::split(size_t offset)
{
    String result;
    split(offset, result);
    return result;
}

void String::split(size_t offset, String &back)
{
    if (offset >= m_Length)
    {
        back.free();
        return;
    }

    char *buf = extract();

    back.assign(&buf[offset]);
    m_Length = offset;

    // Handle the case where the split causes our string to suddenly be shorter
    // than the static size.
    if ((m_Length < StaticSize) && (buf == m_Data))
    {
        MemoryCopy(m_Static, buf, m_Length);
        buf = m_Static;
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
        m_Size = StaticSize;
    }

    buf[m_Length] = 0;

    computeHash();
}

void String::strip()
{
    lstrip();
    rstrip();
}

void String::lstrip()
{
    char *buf = extract();

    if (!iswhitespace(buf[0]))
        return;

    // finish up the byte tail
    size_t n = 0;
    while (n < m_Length && iswhitespace(buf[n]))
        n++;

    // Move the data to cover up the whitespace and avoid reallocating m_Data
    m_Length -= n;
    MemoryCopy(buf, (buf + n), m_Length);
    buf[m_Length] = 0;

    // Did we suddenly drop below the static size?
    if ((buf == m_Data) && (m_Length < StaticSize))
    {
        MemoryCopy(m_Static, m_Data, m_Length + 1);
        m_Size = StaticSize;
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
    }

    computeHash();
}

void String::rstrip()
{
    char *buf = extract();

    if (!iswhitespace(buf[m_Length - 1]))
        return;

    size_t n = m_Length;
    while (n > 0 && iswhitespace(buf[n - 1]))
        n--;

    // m_Size is still valid - it's the size of the buffer. m_Length is now
    // updated to contain the proper length of the string, but the buffer is
    // not reallocated.
    m_Length = n;
    buf[m_Length] = 0;

    // Did we suddenly drop below the static size?
    if ((buf == m_Data) && (m_Length < StaticSize))
    {
        MemoryCopy(m_Static, m_Data, m_Length + 1);
        m_Size = StaticSize;
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
    }

    computeHash();
}

List<SharedPointer<String>> String::tokenise(char token)
{
    List<tokenise_t> list;
    tokenise(token, list);
    return list;
}

size_t String::Utf32ToUtf8(uint32_t utf32, char *utf8)
{
    // clear out the string before conversion
    ByteSet(utf8, 0, 4);

    size_t nbuf = 0;
    if (utf32 <= 0x7F)
    {
        utf8[0] = utf32 & 0x7F;
        nbuf = 1;
    }
    else if (utf32 <= 0x7FF)
    {
        utf8[0] = 0xC0 | ((utf32 >> 6) & 0x1F);
        utf8[1] = 0x80 | (utf32 & 0x3F);
        nbuf = 2;
    }
    else if (utf32 <= 0xFFFF)
    {
        utf8[0] = 0xE0 | ((utf32 >> 12) & 0x0F);
        utf8[1] = 0x80 | ((utf32 >> 6) & 0x3F);
        utf8[2] = 0x80 | (utf32 & 0x3F);
        nbuf = 3;
    }
    else if (utf32 <= 0x10FFFF)
    {
        utf8[0] = 0xF0 | ((utf32 >> 18) & 0x07);
        utf8[1] = 0x80 | ((utf32 >> 12) & 0x3F);
        utf8[2] = 0x80 | ((utf32 >> 6) & 0x3F);
        utf8[3] = 0x80 | (utf32 & 0x3F);
        nbuf = 4;
    }

    return nbuf;
}

void String::tokenise(char token, List<SharedPointer<String>> &output) const
{
    const char *orig_buffer = extract();
    const char *buffer = orig_buffer;

    output.clear();

    const char *pos = nullptr;
    while (*buffer)
    {
        pos = StringFind(buffer, token);
        if (!pos)
        {
            break;
        }

        if (pos == buffer)
        {
            ++buffer;
            continue;
        }

        tokenise_t pStr = tokenise_t(new String(buffer, pos - buffer));
        if (pStr->length())
            output.pushBack(pStr);

        buffer = pos + 1;
    }

    if (!pos)
    {
        tokenise_t pStr =
            tokenise_t(new String(buffer, m_Length - (buffer - orig_buffer)));
        if (pStr->length())
            output.pushBack(pStr);
    }
}

void String::lchomp()
{
    char *buf = extract();

    StringCopy(buf, &buf[1]);
    --m_Length;

    // Did we suddenly drop below the static size?
    if ((buf == m_Data) && (m_Length < StaticSize))
    {
        MemoryCopy(m_Static, m_Data, m_Length + 1);
        m_Size = StaticSize;
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
    }

    computeHash();
}

void String::chomp()
{
    char *buf = extract();

    m_Length--;
    buf[m_Length] = '\0';

    // Did we suddenly drop below the static size?
    if ((buf == m_Data) && (m_Length < StaticSize))
    {
        MemoryCopy(m_Static, m_Data, m_Length + 1);
        m_Size = StaticSize;
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
    }

    computeHash();
}

void String::Format(const char *fmt, ...)
{
    reserve(256);
    va_list vl;
    va_start(vl, fmt);
    m_Length = VStringFormat(m_Data, fmt, vl);
    va_end(vl);

    if (m_Length < StaticSize)
    {
        MemoryCopy(m_Static, m_Data, m_Length + 1);
        m_Size = StaticSize;
        if (m_HeapData)
        {
            delete[] m_Data;
        }
        m_Data = 0;
    }

    computeHash();
}

bool String::endswith(const char c) const
{
    if (!m_Length)
    {
        return false;
    }

    const char *buf = extract();
    return buf[m_Length - 1] == c;
}

bool String::endswith(const String &s) const
{
    // Not a suffix check.
    if (m_Length == s.length())
        return *this == s;

    const char *otherbuf = s.extract();
    return endswith(otherbuf, s.length());
}

bool String::endswith(const char *s, size_t len) const
{
    if (!len)
    {
        len = StringLength(s);
    }

    // Suffix exceeds our length.
    if (m_Length < len)
        return false;

    const char *mybuf = extract();
    mybuf += m_Length - len;

    return !MemoryCompare(mybuf, s, len);
}

bool String::startswith(const char c) const
{
    if (!m_Length)
    {
        return false;
    }

    const char *buf = extract();
    return buf[0] == c;
}

bool String::startswith(const String &s) const
{
    // Not a prefix check.
    if (m_Length == s.length())
        return *this == s;

    const char *otherbuf = s.extract();
    return startswith(otherbuf, s.length());
}

bool String::startswith(const char *s, size_t len) const
{
    if (!len)
    {
        len = StringLength(s);
    }

    // Prefix exceeds our length.
    if (m_Length < len)
        return false;

    const char *mybuf = extract();

    // Do the check.
    return !MemoryCompare(mybuf, s, len);
}

bool String::iswhitespace(const char c) const
{
    return (c <= ' ' || c == '\x7f');
}

char *String::extract() const
{
    if (!m_HeapData)
    {
        return const_cast<char *>(m_ConstData);
    }

    if (m_Length < StaticSize)
    {
        // const_cast because we don't have a side effect but need to return
        // a pointer to our object regardless
        return const_cast<char *>(m_Static);
    }
    else
    {
        return m_Data;
    }
}

ssize_t String::find(const char c) const
{
    if (!m_Length)
        return -1;

    /// \todo this problem only exists because we use return -1 as a status
    ///       indicator. That could be changed, and we could avoid a
    ///       signed/unsigned conversion here!
    ssize_t signedLength = m_Length;

    char *buf = extract();
    for (ssize_t i = 0; i < signedLength; ++i)
    {
        if (buf[i] == c)
        {
            return i;
        }
    }

    return -1;
}

ssize_t String::rfind(const char c) const
{
    if (!m_Length)
        return -1;

    char *buf = extract();
    for (ssize_t i = m_Length - 1, n = 0; i >= 0; --i, ++n)
    {
        if (buf[i] == c)
        {
            return n;
        }
    }

    return -1;
}

void String::computeHash()
{
    if (m_Length)
    {
        m_Hash = jenkinsHash(extract(), m_Length);
    }
    else
    {
        m_Hash = 0;
    }

}

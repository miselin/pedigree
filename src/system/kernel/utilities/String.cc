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
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"
#include <stdarg.h>

/** Minimum size to remain allocated for a String, to avoid tiny heap allocations. */
#define STRING_MINIMUM_ALLOCATION_SIZE 64UL

String::String()
    : m_Data(nullptr), m_Length(0), m_Size(0), m_Hash(0)
{
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
    move(pedigree_std::move(x));
}

String::String(const Cord &x)
{
    assign(x);
}

String::~String()
{
    clear();
}

void String::move(String &&other)
{
    // take ownership of the object
    m_Data = pedigree_std::move(other.m_Data);
    m_Length = pedigree_std::move(other.m_Length);
    m_Size = pedigree_std::move(other.m_Size);
    m_Hash = pedigree_std::move(other.m_Hash);

    // free other string but don't destroy the heap pointer if we had one
    // as it is now owned by this new instance
    other.m_Data = 0;
    other.clear();
}

String &String::operator=(String &&x)
{
    move(pedigree_std::move(x));
    return *this;
}

String &String::operator=(const String &x)
{
    assign(x);
    return *this;
}

#if !STRING_DISABLE_EXPENSIVE_COPY_CONSTRUCTION
String &String::operator=(const char *s)
{
    assign(s);
    return *this;
}
#endif

String &String::operator=(const Cord &x)
{
    assign(x);
    return *this;
}

String &String::operator+=(const String &x)
{
    assert(assignable());

    size_t newLength = x.length() + m_Length;

    reserve(newLength + 1);

    char *dst = extract();
    const char *src = x.extract();

    // Copy!
    MemoryCopy(&dst[m_Length], src, x.length() + 1);
    m_Length += x.length();

    m_Hash = 0;  // hash is no longer valid
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
    return *this;
}

String &String::operator+=(const char *s)
{
    assert(assignable());

    size_t slen = StringLength(s);
    size_t newLength = slen + m_Length;

    reserve(slen + m_Length + 1);
    MemoryCopy(&m_Data[m_Length], s, slen + 1);
    m_Length += slen;

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
    return *this;
}

bool String::operator==(const String &s) const
{
    /// \note Even if the hashes don't exist yet, we still calculate them.
    /// The hash functions are faster than StringMatch as they operate on
    /// larger sections of the string at one time. The downside is that the
    /// worst case performance for comparison is therefore comparing two
    /// strings that do in fact match.

    if (m_Length != s.m_Length)
    {
        return false;
    }
    else if (LIKELY(m_Hash && s.maybeHash()))
    {
        if (m_Hash != s.hash())
        {
            // precomputed hash didn't match, don't bother
            return false;
        }
    }

    const char *buf = extract();
    const char *other_buf = s.extract();

    // Neither of these can be null because of the above conditions.
    return !StringMatchN(buf, other_buf, m_Length);
}

bool String::operator==(const StringView &s) const
{
    // use StringView::operator==(const String &)
    return s == *this;
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
        return !StringMatchN(buf, s, m_Length);
    }
}

bool String::compare(const char *s, size_t len) const
{
    if (m_Length != len)
    {
        // Mismatch in length
        return false;
    }
    else if (UNLIKELY(s == 0))
    {
        // other buffer is null, don't match
        return false;
    }
    else
    {
        const char *buf = extract();
        return !StringMatchN(buf, s, m_Length);
    }
}

char String::operator[](size_t i) const
{
    assert(i <= m_Length);
    const char *buf = extract();
    return buf[i];
}

uint32_t String::hash() const
{
    if (!m_Hash)
    {
        return computeHash();
    }

    return m_Hash;
}

uint32_t String::hash()
{
    if (!m_Hash)
    {
        computeHash();
    }

    return m_Hash;
}

uint32_t String::maybeHash() const
{
    return m_Hash;
}

size_t String::nextCharacter(size_t c) const
{
    const char *buf = extract();
    return ::nextCharacter(buf, c);
}

size_t String::prevCharacter(size_t c) const
{
    const char *buf = extract();
    return ::prevCharacter(buf, c);
}

void String::assign(const String &x)
{
    assert(assignable());

    if (extract() && x.extract())
    {
        assert(extract() != x.extract());
    }
    clear();

    reserve(x.size(), false);
    MemoryCopy(m_Data, x.extract(), x.size());
    m_Length = x.length();

    // no need to recompute in this case
    m_Hash = x.m_Hash;

#if ADDITIONAL_CHECKS
    if (*this != x)
    {
        ERROR("mismatch: '" << *this << "' != '" << x << "'");
    }
    assert(*this == x);
#endif
}

void String::assign(const Cord &x)
{
    assert(assignable());

    clear();
    reserve(x.length() + 1);

    size_t offset = 0;
    char *buf = extract();
    for (auto &it : x.m_Segments)
    {
        StringCopyN(buf + offset, it.ptr, it.length);
        offset += it.length;
    }
    buf[offset] = 0;

    m_Length = offset;

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
}

void String::assign(const char *s, size_t len, bool unsafe)
{
    assert(assignable());

    clear();

    // Trying to assign self to self?
    assert((m_Data == nullptr) || (m_Data && (m_Data != s)));

    size_t copyLength = 0;
    size_t origLength = len;
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
    else if (!s || !*s)
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
        delete [] m_Data;
        m_Data = 0;
        m_Size = 0;
    }
    else
    {
        reserve(copyLength + 1, false);
        MemoryCopy(m_Data, s, copyLength);
        m_Data[copyLength] = '\0';
    }

#if ADDITIONAL_CHECKS
    if (!len)
    {
        assert(*this == s);
    }
#endif

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
}

void String::reserve(size_t size)
{
    reserve(size, true);
}

void String::reserve(size_t size, bool zero)
{
    assert(resizable());

    size = pedigree_std::max(size, STRING_MINIMUM_ALLOCATION_SIZE);

    if (size > m_Size)
    {
        char *tmp = m_Data;
        m_Data = new char[size];
        if (tmp)
        {
            MemoryCopy(m_Data, tmp, m_Size > size ? size : m_Size);
            delete[] tmp;
        }
        else if (zero)
        {
            ByteSet(m_Data, 0, size);
        }
        m_Size = size;
    }
}

void String::downsize()
{
    assert(resizable());

    size_t newSize = pedigree_std::max(m_Length + 1, STRING_MINIMUM_ALLOCATION_SIZE);

    char *oldData = m_Data;

    m_Data = new char[newSize];
    MemoryCopy(m_Data, oldData, newSize);

    delete [] oldData;

    m_Size = newSize;
}

void String::clear()
{
    assert(assignable());

    if (m_Data)
    {
        delete[] m_Data;
    }
    m_Data = 0;
    m_Length = 0;
    m_Size = 0;
    m_Hash = 0;
}

void String::ltrim(size_t n)
{
    assert(assignable());

    if (n > m_Length)
    {
        clear();
        return;
    }

    MemoryCopy(m_Data, &m_Data[n], m_Length - n);
    m_Length -= n;
    m_Data[m_Length] = 0;
}

void String::rtrim(size_t n)
{
    assert(assignable());

    if (n > m_Length)
    {
        clear();
        return;
    }

    m_Data[m_Length - n] = 0;
    m_Length -= n;
}

String String::split(size_t offset)
{
    String result;
    split(offset, result);
    return result;
}

void String::split(size_t offset, String &back)
{
    assert(assignable());

    if (offset >= m_Length)
    {
        back.clear();
        return;
    }

    char *buf = extract();

    back.assign(&buf[offset]);
    m_Length = offset;
    buf[m_Length] = 0;

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
}

void String::strip()
{
    assert(assignable());

    lstrip();
    rstrip();
}

void String::lstrip()
{
    assert(assignable());

    char *buf = extract();
    if (!buf)
    {
        // nothing to strip
        return;
    }

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

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
}

void String::rstrip()
{
    assert(assignable());

    char *buf = extract();
    if (!buf)
    {
        // nothing to strip
        return;
    }

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

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
}

Vector<String> String::tokenise(char token)
{
    Vector<String> list;
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

void String::tokenise(char token, Vector<StringView> &output) const
{
    const char *orig_buffer = extract();
    const char *buffer = orig_buffer;

    output.clear();

    const char *pos = buffer ? StringFind(buffer, token) : nullptr;
    while (pos && (*buffer))
    {
        if (pos == buffer)
        {
            ++buffer;
            continue;
        }

        if (pos > buffer)
        {
            output.pushBack(StringView(buffer, pos - buffer));
        }

        buffer = pos + 1;

        pos = StringFind(buffer, token);
    }

    if (buffer && !pos)
    {
        // might be able to just copy this string rather than copy & move
        if (buffer == orig_buffer)
        {
            output.pushBack(view());
        }
        else
        {
            size_t length = m_Length - (buffer - orig_buffer);
            if (length)
            {
                output.pushBack(StringView(buffer, length));
            }
        }
    }
}

void String::tokenise(char token, Vector<String> &output) const
{
    Vector<StringView> views;
    tokenise(token, views);

    output.clear();
    for (auto &it : views)
    {
        output.pushBack(it.toString());
    }
}

void String::lchomp()
{
    assert(assignable());

    char *buf = extract();

    StringCopy(buf, &buf[1]);
    --m_Length;

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
}

void String::chomp()
{
    assert(assignable());

    char *buf = extract();

    m_Length--;
    buf[m_Length] = '\0';

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
}

void String::Format(const char *fmt, ...)
{
    assert(assignable());

    reserve(256);
    va_list vl;
    va_start(vl, fmt);
    m_Length = VStringFormat(m_Data, fmt, vl);
    va_end(vl);

    m_Hash = 0;
#if STRING_DISABLE_JIT_HASHING
    computeHash();
#endif
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
    return m_Data;
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
        m_Hash = spookyHash(extract(), m_Length);
    }
    else
    {
        m_Hash = 0;
    }
}

uint32_t String::computeHash() const
{
    if (m_Length)
    {
        return spookyHash(extract(), m_Length);
    }
    else
    {
        return 0;
    }
}

String String::copy() const
{
    String result;
    result.assign(*this);
    return result;
}

StringView String::view() const
{
    // hash already calculated, enable hashing
    const char *buf = extract();
    assert(buf);
    return StringView(buf, m_Length, m_Hash, true);
}

bool String::resizable() const
{
    return true;
}

bool String::assignable() const
{
    return true;
}

void String::setLength(size_t n)
{
    m_Length = n;
}

void String::setSize(size_t n)
{
    m_Size = n;
}

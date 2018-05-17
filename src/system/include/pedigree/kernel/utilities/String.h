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

#ifndef KERNEL_UTILITIES_STRING_H
#define KERNEL_UTILITIES_STRING_H

/** @addtogroup kernelutilities
 * @{ */

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/template.h"  // for operator !=

// If non-zero, a constexpr constructor will be available in String. It is an
// experimental change that needs some more work to be stable.
#define STRING_WITH_CONSTEXPR_CONSTRUCTOR 0

// If non-zero, all copy-construction will be disabled. An explicit call to
// copy() is needed to actively copy a String object in that case. This can be
// helpful for removing copies that are not necessary.
#define STRING_DISABLE_COPY_CONSTRUCTION 0

/** String class for ASCII strings
 *\todo provide documentation */
class EXPORTED_PUBLIC String
{
  public:
    /** The default constructor does nothing */
    String();
    String(const char *s);
    String(const char *s, size_t length);
#if STRING_DISABLE_COPY_CONSTRUCTION
    String(const String &x) = delete;
#else
    String(const String &x);
#endif
    String(String &&x);
    ~String();

#if STRING_WITH_CONSTEXPR_CONSTRUCTOR
    template <size_t N>
    constexpr String(const char (&s)[N]);
#endif

    String &operator=(String &&x);
#if !STRING_DISABLE_COPY_CONSTRUCTION
    String &operator=(const String &x);
    String &operator=(const char *s);
#endif
    operator const char *() const
    {
        if (m_Size == StaticSize)
            return m_Static;
        else if (m_Data == 0)
            return "";
        else
            return m_Data;
    }
    /** Allow implicit typecasts to StringView for passing String to functions taking a StringView. */
    operator StringView() const
    {
        return view();
    }
    String &operator+=(const String &x);
    String &operator+=(const char *s);

    bool operator==(const String &s) const;
    bool operator==(const StringView &s) const;
    bool operator==(const char *s) const;

    size_t length() const
    {
        return m_Length;
    }

    size_t size() const
    {
        return m_Size;
    }

    uint32_t hash() const
    {
        return m_Hash;
    }

    /** Given a character index, return the index of the next character,
       interpreting the string as UTF-8 encoded. */
    size_t nextCharacter(size_t c) const;

    /** Given a character index, return the index of the previous character,
       interpreting the string as UTF-8 encoded. */
    size_t prevCharacter(size_t c) const;

    /** Removes the first character from the string. */
    void lchomp();

    /** Removes the last character from the string. */
    void chomp();

    /** Removes the whitespace from the both ends of the string. */
    void strip();

    /** Removes the whitespace from the start of the string. */
    void lstrip();

    /** Removes the whitespace from the end of the string. */
    void rstrip();

    /** Splits the string at the given offset - the front portion will be kept
     * in this string, the back portion (including the character at 'offset'
     * will be returned in a new string. */
    String split(size_t offset);
    void split(size_t offset, String &back);

    Vector<String> tokenise(char token);
    void tokenise(char token, Vector<String> &output) const;
    /** No-copy version of tokenise() that provides views instead of Strings */
    void tokenise(char token, Vector<StringView> &output) const;

    /** Converts a UTF-32 character to its UTF-8 representation.
     *\param[in] utf32 Input UTF-32 character.
     *\param[out] utf8 Pointer to a buffer at least 6 bytes long.
     *\return The number of bytes in the UTF-8 string. */
    static size_t Utf32ToUtf8(uint32_t utf32, char *utf8);

    void Format(const char *format, ...) FORMAT(printf, 2, 3);

    void assign(const String &x);
    /** Assign a buffer to this string.
     * Optionally, unsafe can be passed which will completely trust the len
     * parameter. This may be useful for cases where the input string is not
     * necessarily known to have a null terminator, as otherwise assign() will
     * attempt to find the length of the given string to reduce memory usage.
     */
    void assign(const char *s, size_t len = 0, bool unsafe = false);
    void reserve(size_t size);
    void free();

    /** Does this string end with the given string? */
    bool endswith(const char c) const;
    bool endswith(const String &s) const;
    bool endswith(const char *s, size_t len = 0) const;

    /** Does this string start with the given string? */
    bool startswith(const char c) const;
    bool startswith(const String &s) const;
    bool startswith(const char *s, size_t len = 0) const;

    /** Searches */
    ssize_t find(const char c) const;
    ssize_t rfind(const char c) const;

    /** Copy the String object into a new String. */
    String copy() const;

    /** Get a StringView of this String.
     * \note this view may become invalid if the String is modified.
     */
    StringView view() const;

  private:
    /** Internal doer for reserve() */
    void reserve(size_t size, bool zero);
    /** Extract the correct string buffer for this string. */
    char *extract() const;
    /** Recompute internal hash. */
    void computeHash();
    /** Move another string into this one. */
    void move(String &&other);
    /** Size of static string storage (over this threshold, the heap is used) */
    static constexpr const size_t StaticSize = 64;
    /** Pointer to the zero-terminated ASCII string */
    char *m_Data;
    /** Pointer to a constant version of the string. */
    const char *m_ConstData;
    /** The string's length */
    size_t m_Length;
    /** The size of the reserved space for the string */
    size_t m_Size;
    /** Static string storage (avoid heap overhead for small strings) */
    char m_Static[StaticSize];
    /** Is m_Data heap allocated? Used for e.g. constexpr strings. */
    bool m_HeapData;
    /** Is the given character whitespace? (for *strip()) */
    bool iswhitespace(const char c) const;

    /** Hash of the string. */
    uint32_t m_Hash;
};

#if STRING_WITH_CONSTEXPR_CONSTRUCTOR
template <size_t N>
constexpr String::String(const char (&s)[N])
    : m_Data(nullptr), m_ConstData(s), m_Length(N ? N - 1 : 0), m_Size(N),
      m_HeapData(false)
{
}
#endif

/** @} */

#endif

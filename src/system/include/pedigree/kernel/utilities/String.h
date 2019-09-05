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
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/template.h"  // IWYU pragma: keep

// If non-zero, disable expensive forms of copy construction.
#ifdef IN_STRING_TESTSUITE
#define STRING_DISABLE_EXPENSIVE_COPY_CONSTRUCTION 0
#else
#define STRING_DISABLE_EXPENSIVE_COPY_CONSTRUCTION 1
#endif

// Disable just-in-time hashing on all string objects, which causes String
// creation (including from substrings and copies) to be much slower, but can
// avoid many re-hashes on const String objects that would otherwise be unable
// to store the hash.
#define STRING_DISABLE_JIT_HASHING 0

class Cord;

/** String class for ASCII strings
 *\todo provide documentation */
class EXPORTED_PUBLIC String
{
  public:
    /** The default constructor does nothing */
    String();
    // The constructors marked explicit are slow, but sometimes the only way
    // to instantiate a String object. The intention to these being explicit
    // is that they will not be used accidentally.
    explicit String(const char *s);
    explicit String(const char *s, size_t length);
    explicit String(const Cord &x);
    String(const String &x);
    String(String &&x);
    virtual ~String();

    String &operator=(String &&x);
    String &operator=(const Cord &x);
    String &operator=(const String &x);
#if STRING_DISABLE_EXPENSIVE_COPY_CONSTRUCTION
    String &operator=(const char *s) = delete;
#else
    String &operator=(const char *s);
#endif
    /** The const char * operator needs to be explicit to avoid unintentional
     * conversion to const char * where a String& would be preferable.
     * cstr() is the better option for obtaining a const char * from a String.
     */
    explicit operator const char *() const
    {
        return cstr();
    }
    /** Allow implicit typecasts to StringView for passing String to functions
     * taking a StringView. */
    operator StringView() const
    {
        return view();
    }
    String &operator+=(const String &x);
    String &operator+=(const char *s);

    bool operator==(const String &s) const;
    bool operator==(const StringView &s) const;
    bool operator==(const char *s) const;

    /** Perform a string comparison with the given string. */
    bool compare(const char *s, size_t len) const;

    /**
     * Perform a string comparison with the given constant string.
     * You should use this rather than operator== for these types of
     * comparisons as this uses a length hint to potentially avoid
     * an actual comparison of the string contents.
     */
    template<size_t N>
    bool compare(const char (&s)[N]) const
    {
        // ignore null in c-string for comparison
        return compare(s, N - 1);
    }

    char operator[](size_t i) const;

    const char *cstr() const
    {
        return extract();
    }

    size_t length() const
    {
        return m_Length;
    }

    size_t size() const
    {
        return m_Size;
    }

    /**
     * Variant of hash() that might compute the hash if needed, but won't
     * update the stored hash.
     */
    uint32_t hash() const;

    /** Variant of hash() that computes the hash if needed. */
    uint32_t hash();

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

    /** Remove the first N characters from the string. */
    void ltrim(size_t n);

    /** Remove the last N characters from the string. */
    void rtrim(size_t n);

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
    void assign(const Cord &x);
    /** Assign a buffer to this string.
     * Optionally, unsafe can be passed which will completely trust the len
     * parameter. This may be useful for cases where the input string is not
     * necessarily known to have a null terminator, as otherwise assign() will
     * attempt to find the length of the given string to reduce memory usage.
     */
    void assign(const char *s, size_t len = 0, bool unsafe = false);
    void reserve(size_t size);
    virtual void clear();

    /** Resize the buffer to fit the actual string. */
    void downsize();

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

  protected:
    /** Is the given character whitespace? (for *strip()) */
    bool iswhitespace(const char c) const;
    /** Internal doer for reserve() */
    void reserve(size_t size, bool zero);
    /** Recompute internal hash. */
    void computeHash();
    /** Recompute internal hash but don't store it. */
    uint32_t computeHash() const;
  private:
    /** Extract hash without recomputing it. */
    uint32_t maybeHash() const;
    /** Extract the correct string buffer for this string. */
    virtual char *extract() const;
    /** Move another string into this one. */
    void move(String &&other);
    /** Pointer to the zero-terminated ASCII string */
    char *m_Data;
    /** The string's length */
    size_t m_Length;
    /** The size of the reserved space for the string */
    size_t m_Size;
    /** Hash of the string. */
    uint32_t m_Hash;
    /** Is this string resizable? */
    virtual bool resizable() const;
    /** Is this string assignable? */
    virtual bool assignable() const;

  protected:
    /** Internal setter for length. */
    void setLength(size_t n);
    /** Internal setter for size. */
    void setSize(size_t n);
};

/** Class for immmutable strings with values known at compile time.
 *
 * \todo make String hashes more expressive so we can do a compile-time hash
 * of these ConstantString objects and avoid the runtime computation.
 */
template <size_t N>
class ConstantString : public String
{
    public:
        ConstantString(const char str[N])
        {
            setLength(N);
            setSize(N);
            MemoryCopy(m_Data, str, N);
            computeHash();
        }

        virtual void clear() override {}
    private:
        virtual char *extract() const override
        {
            return const_cast<char *>(m_Data);
        }

        virtual bool resizable() const override
        {
            return false;
        }

        virtual bool assignable() const override
        {
            return false;
        }

        char m_Data[N];
};

template <size_t N>
ConstantString<N> MakeConstantString(const char (&str)[N])
{
    return ConstantString<N>(str);
}

/** @} */

#endif

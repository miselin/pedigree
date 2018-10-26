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

#ifndef KERNEL_UTILITIES_STRINGVIEW_H
#define KERNEL_UTILITIES_STRINGVIEW_H

/** @addtogroup kernelutilities
 * @{ */

#define HASH_STRINGVIEWS_BY_DEFAULT false

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/template.h"  // IWYU pragma: keep

class String;

class EXPORTED_PUBLIC StringView
{
    friend class String;

  public:
    StringView();
    explicit StringView(const char *s);
    StringView(const char *s, size_t length);
    StringView(const StringView &other);
    explicit StringView(const String &other);
    virtual ~StringView();

    StringView &operator=(const StringView &s);

    bool operator==(const char *s) const;
    bool operator==(const String &s) const;
    bool operator==(const StringView &s) const;

    /**
     * Perform a sized comparison on the given string.
     * Generally this will perform better than operator==(const char *)
     */
    bool compare(const char *s, size_t length) const;

    size_t length() const;

    /** Pass hashed = true to hash the substring, which makes this slower. */
    StringView substring(size_t start, size_t end, bool hashed = HASH_STRINGVIEWS_BY_DEFAULT) const;

    String toString() const;

    char operator[](size_t index) const;

    size_t nextCharacter(size_t i) const;
    size_t prevCharacter(size_t i) const;

    uint32_t hash() const;
    uint32_t hash();

    const char *str() const;

    /**
     * Set whether hashing is enabled for this StringView.
     * Hashing makes comparisons against different strings faster, at the
     * expense of much slower creation.
     */
    void setHashingEnable(bool enabled);

  private:
    bool compareHash(const StringView &other) const;
    bool compareHash(const String &other) const;

    uint32_t computeHash() const;

    // String::view() can set m_Hash to avoid recalculating.
    StringView(const char *s, size_t length, uint32_t hash, bool hashingEnabled);

    const char *m_String;
    size_t m_Length;
    uint32_t m_Hash;
    bool m_HashingEnabled;

  protected:
    virtual bool defaultHashingEnabled() const;
};

/**
 * Variant of StringView that automatically upgrades StringViews to hashed
 * versions if they are not already hashed.
 * Use in cases where a parameter must be able to be hashed but would otherwise
 * be const.
 */
class EXPORTED_PUBLIC HashedStringView : public StringView
{
   public:
    HashedStringView() = default;
    explicit HashedStringView(const char *s);
    HashedStringView(const char *s, size_t length);
    HashedStringView(const StringView &other);
    HashedStringView(const HashedStringView &other);
    HashedStringView(const String &other);

   private:
    bool defaultHashingEnabled() const override;
};

/** @} */

#endif  // KERNEL_UTILITIES_STRINGVIEW_H
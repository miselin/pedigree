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

#ifndef KERNEL_UTILITIES_CORD_H
#define KERNEL_UTILITIES_CORD_H

/** @addtogroup kernelutilities
 * @{ */

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/template.h"  // IWYU pragma: keep
#include "pedigree/kernel/utilities/Vector.h"

class String;

class EXPORTED_PUBLIC Cord
{
    friend class CordIterator;
  public:
    class CordIterator
    {
        friend class Cord;
        public:
            CordIterator(const Cord &owner);
            virtual ~CordIterator();

            CordIterator &operator++();
            CordIterator &operator--();

            char operator*() const;

            bool operator==(const CordIterator &other) const;
            bool operator!=(const CordIterator &other) const;

        protected:
            CordIterator(const Cord &owner, bool end);

        private:
            const Cord &cord;
            size_t segment;
            size_t index;
    };

    Cord();
    Cord(const Cord &other);
    virtual ~Cord();

    Cord &operator=(const Cord &s);

    bool operator==(const char *s) const;
    bool operator==(const Cord &s) const;
    bool operator==(const String &s) const;

    /**
     * Pre-reserve the given number of segments.
     * Useful if the segment count is known in advance to avoid vector resizes.
     */
    void reserve(size_t segments);

    void assign(const Cord &other);
    void clear();

    size_t length() const;

    String toString() const;

    char operator[](size_t index) const;

    void append(const char *s, size_t len=0);
    void prepend(const char *s, size_t len=0);

    CordIterator begin() const;
    CordIterator end() const;

  private:
    struct CordSegment
    {
        CordSegment() = default;
        CordSegment(const char *s, size_t len) : ptr(s), length(len) {}

        const char *ptr = nullptr;
        size_t length = 0;
    };

    Vector<CordSegment> m_Segments;
    size_t m_Length = 0;
};

/** @} */

#endif  // KERNEL_UTILITIES_CORD_H

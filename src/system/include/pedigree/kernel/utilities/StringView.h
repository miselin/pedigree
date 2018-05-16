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

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

class String;

class EXPORTED_PUBLIC StringView
{
    public:
        StringView();
        StringView(const char *s);
        StringView(const char *s, size_t length);
        virtual ~StringView();

        bool operator== (const char *s) const;
        bool operator== (const String &s) const;
        bool operator== (const StringView &s) const;

        size_t length() const;

        StringView substring(size_t start, size_t end) const;

        String toString() const;

    private:
        const char *m_String;
        size_t m_Length;
};

/** @} */

#endif  // KERNEL_UTILITIES_STRINGVIEW_H
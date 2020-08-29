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

#ifndef MACHINE_SERIAL_H
#define MACHINE_SERIAL_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/StaticCord.h"
#include "pedigree/kernel/utilities/StaticString.h"

/**
 * Serial device abstraction.
 */
class EXPORTED_PUBLIC Serial
{
  public:
    virtual ~Serial();

    /// \todo make this generic for Mmaped and port IO.
    virtual void setBase(uintptr_t nBaseAddr) = 0;
    virtual char read() = 0;
    virtual char readNonBlock() = 0;
    virtual void write(char c) = 0;
    virtual void write_str(const char *c);
    virtual void write_str(const char *c, size_t len);
    virtual void write_str(const Cord &cord);

    template<unsigned int N>
    void write_str(const StaticCord<N> &cord)
    {
        for (auto it = cord.segbegin(); it != cord.segend(); ++it)
        {
            write_str(it.ptr(), it.length());
        }
    }

    // Const string overload (so no strlen needed)
    template<unsigned int N>
    void write_str(const char (&c)[N])
    {
        write_str(c, N);
    }

    template<unsigned int N>
    void write_str(const StaticString<N> (&c))
    {
        write_str(static_cast<const char *>(c), N);
    }
};

#endif

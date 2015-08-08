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

#include <processor/Processor.h>

#include <Log.h>

#include <panic.h>

void _assert(bool b, const char *file, int line, const char *func)
{
    if(b)
        return;

    if(Processor::m_Initialised)
    {
        ERROR_NOLOCK("Assertion failed in file " << file);
        ERROR_NOLOCK("In function '" << func << "'");
        ERROR_NOLOCK("On line " << Dec << line << Hex << ".");
        Processor::breakpoint();

        ERROR_NOLOCK("You may not resume after a failed assertion.");
    }

    // Best reason for a return is that the debugger isn't active. Either way, it's an error condition, panic.
    panic("assertion failed");
    Processor::halt();
}

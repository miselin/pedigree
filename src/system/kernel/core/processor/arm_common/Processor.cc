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

#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"

void ProcessorBase::halt()
{
    while (1)
        ;
}

void ProcessorBase::breakpoint()
{
    //
}

void ProcessorBase::reset()
{
    //
}

void ProcessorBase::haltUntilInterrupt()
{
    bool bOldInterrupts = getInterrupts();
    setInterrupts(true);
    asm volatile("wfi");
    setInterrupts(bOldInterrupts);
}

void ProcessorBase::pause()
{
    asm volatile("yield");
}

void ProcessorBase::deinitialise()
{
}

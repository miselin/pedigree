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

#include "pedigree/kernel/processor/state.h"

#include <ucontext.h>

const char *HostedInterruptStateRegisterName[3] = {
    "state",
    "extra",
    "meta",
};

HostedInterruptState::HostedInterruptState() : state(0), which(0)
{
}

HostedInterruptState::~HostedInterruptState()
{
}

size_t HostedInterruptState::getRegisterCount() const
{
    return 3;
}
processor_register_t HostedInterruptState::getRegister(size_t index) const
{
    if (index == 0)
        return state;
    if (index == 1)
        return extra;
    if (index == 2)
        return meta;
    return 0;
}
const char *HostedInterruptState::getRegisterName(size_t index) const
{
    return HostedInterruptStateRegisterName[index];
}

size_t HostedSyscallState::getRegisterCount() const
{
    return 0;
}

processor_register_t HostedSyscallState::getRegister(size_t index) const
{
    return 0;
}

const char *HostedSyscallState::getRegisterName(size_t index) const
{
    return "<no registers>";
}

HostedInterruptState *
HostedInterruptState::construct(HostedProcessorState &state, bool userMode)
{
    return 0;
}

HostedSchedulerState::HostedSchedulerState()
{
    stackBase = 0;
    stackSize = 0;

    ucontext_t *ctx = reinterpret_cast<ucontext_t *>(state);
    getcontext(ctx);
}

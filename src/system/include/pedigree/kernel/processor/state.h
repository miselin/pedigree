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

#ifndef KERNEL_PROCESSOR_STATE_H
#define KERNEL_PROCESSOR_STATE_H

#include "pedigree/kernel/processor/state_forward.h"

// Bring in rest of state definitions to complete forward-declared state.
#if defined(X86)
#include "pedigree/kernel/processor/x86/state.h"  // IWYU pragma: export
#elif defined(X64)
#include "pedigree/kernel/processor/x64/state.h"  // IWYU pragma: export
#elif defined(MIPS32)
#include "pedigree/kernel/processor/mips32/state.h"  // IWYU pragma: export
#elif defined(MIPS64)
#include "pedigree/kernel/processor/mips64/state.h"  // IWYU pragma: export
#elif defined(ARM926E)
#include "pedigree/kernel/processor/arm_926e/state.h"  // IWYU pragma: export
#elif defined(PPC32)
#include "pedigree/kernel/processor/ppc32/state.h"  // IWYU pragma: export
#elif defined(ARMV7)
#include "pedigree/kernel/processor/armv7/state.h"  // IWYU pragma: export
#elif defined(HOSTED)
#include "pedigree/kernel/processor/hosted/state.h"  // IWYU pragma: export
#endif

#endif

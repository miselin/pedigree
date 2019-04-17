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

#ifndef KERNEL_PROCESSOR_STACKFRAME_H
#define KERNEL_PROCESSOR_STACKFRAME_H

#include "pedigree/kernel/processor/StackFrameBase.h"  // IWYU pragma: export

#include "pedigree/kernel/processor/x64/StackFrame.h"  // IWYU pragma: export
#include "pedigree/kernel/processor/mips32/StackFrame.h"  // IWYU pragma: export
#include "pedigree/kernel/processor/mips64/StackFrame.h"  // IWYU pragma: export
#include "pedigree/kernel/processor/arm_926e/StackFrame.h"  // IWYU pragma: export
#include "pedigree/kernel/processor/ppc32/StackFrame.h"  // IWYU pragma: export
#include "pedigree/kernel/processor/armv7/StackFrame.h"  // IWYU pragma: export
#include "pedigree/kernel/processor/hosted/StackFrame.h"  // IWYU pragma: export

#if X64
#define PROCESSOR_SPECIFIC_NAME(x) X64##x
#elif MIPS32
#define PROCESSOR_SPECIFIC_NAME(x) MIPS32##x
#elif MIPS64
#define PROCESSOR_SPECIFIC_NAME(x) MIPS64##x
#elif ARM926E
#define PROCESSOR_SPECIFIC_NAME(x) ARM926E##x
#elif PPC32
#define PROCESSOR_SPECIFIC_NAME(x) PPC32##x
#elif ARMV7
#define PROCESSOR_SPECIFIC_NAME(x) ARMV7##x
#elif HOSTED
#define PROCESSOR_SPECIFIC_NAME(x) Hosted##x
#endif

// NOTE: This throws a compile-time error if this header is not adapted for
//       the selected processor architecture
#if !defined(PROCESSOR_SPECIFIC_NAME)
#error Unknown processor architecture
#endif

/** @addtogroup kernelprocessor
 * @{ */

// NOTE: If a newly added processor architecture does not supply all the
//       needed types, you will get an error here

/** Lift the processor-specifc StackFrame class into the global namespace */
typedef PROCESSOR_SPECIFIC_NAME(StackFrame) StackFrame;

/** @} */

#undef PROCESSOR_SPECIFIC_NAME

#endif

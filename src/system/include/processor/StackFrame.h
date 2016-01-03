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

#if defined(X86)
  #include <processor/x86/StackFrame.h>
  #define PROCESSOR_SPECIFIC_NAME(x) X86##x
#elif defined(X64)
  #include <processor/x64/StackFrame.h>
  #define PROCESSOR_SPECIFIC_NAME(x) X64##x
#elif defined(MIPS32)
  #include <processor/mips32/StackFrame.h>
  #define PROCESSOR_SPECIFIC_NAME(x) MIPS32##x
#elif defined(MIPS64)
  #include <processor/mips64/StackFrame.h>
  #define PROCESSOR_SPECIFIC_NAME(x) MIPS64##x
#elif defined(ARM926E)
  #include <processor/arm_926e/StackFrame.h>
  #define PROCESSOR_SPECIFIC_NAME(x) ARM926E##x
#elif defined(PPC32)
  #include <processor/ppc32/StackFrame.h>
  #define PROCESSOR_SPECIFIC_NAME(x) PPC32##x
#elif defined(ARMV7)
  #include <processor/armv7/StackFrame.h>
  #define PROCESSOR_SPECIFIC_NAME(x) ARMV7##x
#elif defined(HOSTED)
  #include <processor/hosted/StackFrame.h>
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

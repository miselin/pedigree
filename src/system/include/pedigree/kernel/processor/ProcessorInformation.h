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

#ifndef KERNEL_PROCESSOR_PROCESSORINFORMATION_H
#define KERNEL_PROCESSOR_PROCESSORINFORMATION_H

#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessor
 * @{ */

/** Identifier of a processor */
typedef size_t ProcessorId;

/** @} */

#ifndef _PROCESSOR_INFORMATION_ONLY_WANT_PROCESSORID

#if X86_COMMON
#include "pedigree/kernel/processor/x86_common/ProcessorInformation.h"  // IWYU pragma: export
#define PROCESSOR_SPECIFIC_NAME(x) X86Common##x
#elif MIPS_COMMON
#include "pedigree/kernel/processor/mips_common/ProcessorInformation.h"  // IWYU pragma: export
#define PROCESSOR_SPECIFIC_NAME(x) MIPSCommon##x
#elif ARM_COMMON
#include "pedigree/kernel/processor/arm_common/ProcessorInformation.h"  // IWYU pragma: export
#define PROCESSOR_SPECIFIC_NAME(x) ArmCommon##x
#elif PPC_COMMON
#include "pedigree/kernel/processor/ppc_common/ProcessorInformation.h"  // IWYU pragma: export
#define PROCESSOR_SPECIFIC_NAME(x) PPCCommon##x
#elif HOSTED
#include "pedigree/kernel/processor/hosted/ProcessorInformation.h"  // IWYU pragma: export
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

/** Define ProcessorInformation */
typedef PROCESSOR_SPECIFIC_NAME(ProcessorInformation) ProcessorInformation;

/** @} */

#undef PROCESSOR_SPECIFIC_NAME

#else
#undef KERNEL_PROCESSOR_PROCESSORINFORMATION_H
#endif

#endif

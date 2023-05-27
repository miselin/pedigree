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

#ifndef KERNEL_MACHINE_TYPES_H
#define KERNEL_MACHINE_TYPES_H

#if X86_COMMON
#include "pedigree/kernel/machine/x86_common/types.h"
#define MACHINE_SPECIFIC_NAME(x) X86Common##x
#endif
#if MIPS_COMMON
#include "pedigree/kernel/machine/mips_common/types.h"
#define MACHINE_SPECIFIC_NAME(x) MIPSCommon##x
#endif
#if ARM_COMMON
#include "pedigree/kernel/machine/arm_common/types.h"
#define MACHINE_SPECIFIC_NAME(x) ARMCommon##x
#endif
#if PPC_COMMON
#include "pedigree/kernel/machine/ppc_common/types.h"
#define MACHINE_SPECIFIC_NAME(x) PPCCommon##x
#endif

#if HOSTED
#include "pedigree/kernel/machine/hosted/types.h"
#ifndef MACHINE_SPECIFIC_NAME
#define MACHINE_SPECIFIC_NAME(x) HostedCommon##x
#endif
#endif

// NOTE: This throws a compile-time error if this header is not adapted for
//       the selected machine architecture
#ifndef MACHINE_SPECIFIC_NAME
#error Unknown machine architecture
#endif

/** @addtogroup kernelmachine
 * @{ */

// NOTE: If a newly added machine architecture does not supply all the
//       needed types, you will get an error here

/** Define a type for IRQ identifications */
typedef MACHINE_SPECIFIC_NAME(irq_id_t) irq_id_t;

/** @} */

#undef MACHINE_SPECIFIC_NAME

#endif

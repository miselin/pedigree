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

#ifndef KERNEL_PROCESSOR_ARM_TYPES_H
#define KERNEL_PROCESSOR_ARM_TYPES_H

/** @addtogroup kernelprocessorarm
 * @{ */

/** Define an 8bit signed integer type */
typedef __INT8_TYPE__ ARMint8_t;
/** Define an 8bit unsigned integer type */
typedef __UINT8_TYPE__ ARMuint8_t;
/** Define an 16bit signed integer type */
typedef __INT16_TYPE__ ARMint16_t;
/** Define an 16bit unsigned integer type */
typedef __UINT16_TYPE__ ARMuint16_t;
/** Define a 32bit signed integer type */
typedef __INT32_TYPE__ ARMint32_t;
/** Define a 32bit unsigned integer type */
typedef __UINT32_TYPE__ ARMuint32_t;
/** Define a 64bit signed integer type */
typedef __INT64_TYPE__ ARMint64_t;
/** Define a 64bit unsigned integer type */
typedef __UINT64_TYPE__ ARMuint64_t;

/** Define a signed integer type for pointer arithmetic */
typedef __INTPTR_TYPE__ ARMintptr_t;
/** Define an unsigned integer type for pointer arithmetic */
typedef __UINTPTR_TYPE__ ARMuintptr_t;

/** Define a unsigned integer type for physical pointer arithmetic */
typedef ARMuintptr_t ARMphysical_uintptr_t;

/** Define an unsigned integer type for the processor registers */
typedef ARMuintptr_t ARMprocessor_register_t;

/** Define ssize_t */
typedef __INTPTR_TYPE__ ARMssize_t;
/** Define size_t */
typedef __SIZE_TYPE__ ARMsize_t;

/** Define an I/O port type */
typedef ARMuint16_t ARMio_port_t;

/** No I/O port type */
#define KERNEL_PROCESSOR_NO_PORT_IO 1

/** Define the size of one physical page */
#define PAGE_SIZE 4096

/** Help other headers see what we've defined. */
#define __DEFINED_size_t 1

/** @} */

#endif

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


#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <pedigree/kernel/compiler.h>
#include <pedigree/kernel/processor/types.h>
#ifdef UTILITY_LINUX
#include <assert.h>
#else
#include <pedigree/kernel/utilities/assert.h>
#endif

typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;

typedef int8_t s8_t;
typedef int16_t s16_t;
typedef int32_t s32_t;

typedef intptr_t mem_ptr_t;

// 0/1 - interrupt state before protecting
typedef int sys_prot_t;

#define U16_F "u"
#define S16_F "d"
#define X16_F "x"

#define U32_F "u"
#define S32_F "d"
#define X32_F "x"

#define SZT_F "zu"

#define LWIP_CHKSUM_ALGORITHM 2

/*
#define LWIP_PLATFORM_DIAG(x)

#define LWIP_PLATFORM_ASSERT(x) assert(0)
*/

#include <stdio.h>

#define LWIP_PLATFORM_DIAG(msg) printf msg ; fflush(stdout);

#define LWIP_PLATFORM_ASSERT(msg) fprintf(stderr, "Assertion failed; %s:%d %s\n", __LINE__, __FILE__, msg); assert(0)

#define PACK_STRUCT_STRUCT PACKED

#define LWIP_NO_STDINT_H 1

#define LWIP_DBG_MIN_LEVEL LWIP_DBG_LEVEL_ALL

#define LWIP_DEBUG 1

#if 0
#define ETHARP_DEBUG LWIP_DBG_ON
#define NETIF_DEBUG LWIP_DBG_ON
#define PBUF_DEBUG LWIP_DBG_ON
#define MEM_DEBUG LWIP_DBG_ON
#define MEMP_DEBUG LWIP_DBG_ON
#define IP_DEBUG LWIP_DBG_ON
#define INET_DEBUG LWIP_DBG_ON
#define TCP_DEBUG LWIP_DBG_ON
#define TCPIP_DEBUG LWIP_DBG_ON
#endif

#endif  // LWIP_ARCH_CC_H
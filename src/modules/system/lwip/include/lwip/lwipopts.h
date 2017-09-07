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


#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

#include <pedigree/kernel/utilities/utility.h>

#define MEMCPY(dst, src, len) MemoryCopy(dst, src, len)
#define SMEMCPY(dst, src, len) MemoryCopy(dst, src, len)

#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1

/// \todo should be architecture specific
#define MEM_ALIGNMENT 8

/// \todo only needs to be 1 if sizeof(void*) > 4
#define IPV6_FRAG_COPYHEADER 1

#define LWIP_RAW 1
#define LWIP_IPV6 1
#define LWIP_NETCONN 1

#define LWIP_COMPAT_SOCKETS 1

#define LWIP_PROVIDE_ERRNO 1

// We can safely do this rather than use an mbox as packets are pushed into
// a RequestQueue, not directly pushed from an IRQ context.
#define LWIP_TCPIP_CORE_LOCKING_INPUT 0

#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1

#define LWIP_DHCP 1
#define LWIP_AUTOIP 1
#define LWIP_DHCP_AUTOIP_COOP 1

#define LWIP_TCP_TIMESTAMPS 1

#define LWIP_NETIF_LOOPBACK 1

#define LWIP_IPV6_DHCP6 1

#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HWADDRHINT 1


// General tuning
#define TCP_MSS 1400
#define TCP_WND 32768
#define TCP_SND_BUF 65536

#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 4

#endif  // LWIP_LWIPOPTS_H

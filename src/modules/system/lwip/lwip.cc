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


#include "modules/Module.h"

#include "pedigree/kernel/process/Mutex.h"

#include "lwip/include/lwip/init.h"
#include "lwip/include/lwip/tcpip.h"

// Switch the module-specific pieces of the module over to hidden visibility
#pragma GCC visibility push(hidden)

static Mutex tcpipInitPending(false);

static void tcpipInitComplete(void *)
{
    tcpipInitPending.release();
}

static bool entry()
{
    tcpipInitPending.acquire();

    // make sure the multi threaded lwIP implementation is ready to go
    /// \todo check if tcpip_init fails somehow
    tcpip_init(tcpipInitComplete, nullptr);

    tcpipInitPending.acquire();

    return true;
}

static void exit()
{
    /// \todo can we shut down lwip cleanly here?
}

MODULE_INFO("lwip", &entry, &exit);

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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Completion.h"

#include "modules/Module.h"
#include "modules/system/lwip/include/lwip/init.h"
#include "modules/system/lwip/include/lwip/sys.h"
#include "modules/system/lwip/include/lwip/tcpip.h"

// Switch the module-specific pieces of the module over to hidden visibility
#pragma GCC visibility push(hidden)

static Completion tcpipInitPending;

static void tcpipInitComplete(void*) {
  tcpipInitPending.complete();
}

static err_t stopTcpip() {
  err_t result = ERR_INPROGRESS;
  while (result == ERR_INPROGRESS) {
    result = tcpip_shutdown();
    if (result == ERR_INPROGRESS) {
      sys_msleep(1);
    }
  }
  return result;
}

static bool entry() {
  // make sure the multi threaded lwIP implementation is ready to go
  /// \todo check if tcpip_init fails somehow
  tcpip_init(tcpipInitComplete, nullptr);

  if (!tcpipInitPending.wait()) {
    if (stopTcpip() != ERR_OK) {
      FATAL("lwIP could not stop its tcpip worker after interrupted init");
    }
    return false;
  }

  return true;
}

static void exit() {
  if (stopTcpip() != ERR_OK) {
    FATAL("lwIP could not stop and join its tcpip worker");
  }
}

MODULE_INFO("lwip", &entry, &exit);

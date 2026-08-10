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
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
#include "pedigree/kernel/Log.h"

#include "modules/system/usb/UsbPnP.h"
#endif

static bool entry() {
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN usb-pnp-reciprocal-unregister-smp");
  if (!UsbPnP::runQemuRegistrationRegression()) {
    FATAL("QEMU UsbPnP reciprocal-unregister regression failed");
  }
#endif
  return true;
}

static void exit() {}

MODULE_INFO("usb", &entry, &exit);

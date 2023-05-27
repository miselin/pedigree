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

#include "IsaDma.h"
#include "modules/Module.h"
#include "pedigree/kernel/Log.h"

IsaDma::IsaDma() = default;
IsaDma::~IsaDma() = default;

#if X86_COMMON
#include "x86/X86IsaDma.h"
IsaDma &IsaDma::instance()
{
    return X86IsaDma::instance();
}
#else
static IsaDma ins;
IsaDma &IsaDma::instance()
{
    WARNING("Unsupported platform for IsaDma, yet it's being used.");
    return ins;
}
#endif

bool IsaDma::initTransfer(uint8_t channel, uint8_t mode, size_t length, uintptr_t addr)
{
    WARNING("IsaDma::initTransfer is not implemented");
    return false;
}

static bool pedigree_init()
{
    return true;
}

static void pedigree_destroy()
{
}

MODULE_INFO("dma", &pedigree_init, &pedigree_destroy);

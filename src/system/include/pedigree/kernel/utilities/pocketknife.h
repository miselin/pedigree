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

#include "pedigree/kernel/compiler.h"

/**
 * pocketknife contains utilities that perform common tasks.
 *
 * It's somewhat designed so it can be also used as a light compatibility layer
 * but it should be assumed that you can do Pedigree-specific stuff here.
 */
namespace pocketknife
{
/**
 * Run the given function concurrently with the given parameter.
 *
 * The thread that is created by this function is detached and cannot be
 * joined. Use this to run a small function asynchronously if you don't care
 * about its return value or stopping it later.
 */
EXPORTED_PUBLIC void runConcurrently(int (*func)(void *), void *param);

/**
 * Run the given function concurrently with the given parameter.
 *
 * The handle returned can be used to join the thread and retrieve its return
 * value.
 */
EXPORTED_PUBLIC void *runConcurrentlyAttached(int (*func)(void *), void *param);

/**
 * Join the given handle returned from runConcurrentlyAttached.
 */
EXPORTED_PUBLIC int attachTo(void *handle);
}  // namespace pocketknife

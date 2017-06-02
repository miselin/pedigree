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

#ifndef PROCESS_INTERRUPTIBLE_H
#define PROCESS_INTERRUPTIBLE_H

#include "pedigree/kernel/compiler.h"

/**
 * Uninterruptible provides an RAII helper to move the current thread into
 * an uninterruptible state. That means the thread will be able to be
 * scheduled, but will not be able to have any events sent to it until it
 * ceases to be uninterruptible. This can be important in certain cases where
 * an event would cause a re-entry into a mutual exclusion critical section or
 * some other undesirable event.
 *
 * \note Threads cannot be set uninterruptible without using this.
 */
class Uninterruptible
{
  public:
    Uninterruptible();
    ~Uninterruptible();

  private:
    NOT_COPYABLE_OR_ASSIGNABLE(Uninterruptible);
};

#endif  // PROCESS_INTERRUPTIBLE_H

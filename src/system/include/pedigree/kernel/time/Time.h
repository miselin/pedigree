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

#ifndef TIME_H
#define TIME_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

namespace Time
{
typedef uint64_t Timestamp;

namespace Multiplier
{
const Timestamp Nanosecond = 1U;
const Timestamp Microsecond = 1000U;
const Timestamp Millisecond = 1000000U;
const Timestamp Second = Millisecond * 1000U;
const Timestamp Minute = Second * 60U;
const Timestamp Hour = Minute * 60U;
const Timestamp Day = Hour * 24U;
}  // namespace Multiplier

/** Magic identifier for infinite time durations. */
const Timestamp Infinity = 0xFFFFFFFFFFFFFFFFULL;

/** Performs a sleep for the given time. */
EXPORTED_PUBLIC bool delay(Timestamp nanoseconds);

/**
 * Add an alarm that interrupts the thread after the given time.
 * Can be used to request a wakeup from a sleep().
 */
EXPORTED_PUBLIC void *addAlarm(Timestamp nanoseconds);

/** Remove an alarm created by addAlarm. */
EXPORTED_PUBLIC void removeAlarm(void *handle);

/** Run the given function (asynchronously) after the specified delay. */
EXPORTED_PUBLIC void
runAfter(int (*func)(void *), void *param, Timestamp nanoseconds);

/** Gets the system's current time. */
EXPORTED_PUBLIC Timestamp getTime(bool sync = false);

/** Gets the system's current time in nanoseconds. */
EXPORTED_PUBLIC Timestamp getTimeNanoseconds(bool sync = false);

/**
 * Gets a tick count in nanoseconds.
 * Subsequent calls will always see this number grow.
 */
EXPORTED_PUBLIC Timestamp getTicks();

namespace Conversion
{
/**
 * Converts the given expanded date to a UNIX timestamp.
 */
EXPORTED_PUBLIC Timestamp toUnix(
    size_t second, size_t minute, size_t hour, size_t dom, size_t month,
    size_t year);

}  // namespace Conversion

}  // namespace Time

#endif

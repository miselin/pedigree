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

#include <time/Time.h>
#include <utilities/assert.h>

static const uint16_t cumulativeDays[] = {0,   31,  59,  90,  120, 151, 181,
                                          212, 243, 273, 304, 334, 365};

namespace Time
{
namespace Conversion
{
Timestamp toUnix(
    size_t second, size_t minute, size_t hour, size_t dom, size_t month,
    size_t year)
{
    assert(year >= 1970);
    assert(month >= 1 && month <= 12);
    assert(dom >= 1 && dom <= 31);

    --dom;

    // # of leap days.
    size_t leaps = (year / 4) - (year / 100) + (year / 400);
    // We only care about leap days since the epoch.
    leaps -= (1970 / 4) - (1970 / 100) + (1970 / 400);
    // # of days so far this year.
    size_t cumuldays = cumulativeDays[month - 1];

    Time::Timestamp result = 0;
    result += second;
    result += minute * 60;
    result += hour * 60 * 60;
    result += dom * 24 * 60 * 60;
    result += cumuldays * 24 * 60 * 60;
    result += leaps * 24 * 60 * 60;
    result += (year - 1970) * 365 * 24 * 60 * 60;
    return result;
}

}  // namespace Conversion

}  // namespace Time

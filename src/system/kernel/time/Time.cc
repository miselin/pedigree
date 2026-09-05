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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/InfoBlock.h"
#include "pedigree/kernel/time/Time.h"

namespace Time {
namespace {
Spinlock realtimeLock(false, true);
bool realtimeSet = false;
Timestamp realtimeBase = 0;
Timestamp monotonicBase = 0;
}  // namespace

Timestamp getTime(bool sync) {
  return getTimeNanoseconds(sync) / Multiplier::Second;
}

Timestamp getTimeNanoseconds(bool sync) {
  Timer* pTimer = Machine::instance().getTimer();
  if (!pTimer) {
    return 0;
  }
  if (sync)
    pTimer->synchronise();
  LockGuard<Spinlock> guard(realtimeLock);
  if (realtimeSet) {
    const Timestamp elapsed = pTimer->getTickCountNano() - monotonicBase;
    return elapsed >= Infinity - realtimeBase ? Infinity - 1 : realtimeBase + elapsed;
  }
  Timestamp r = pTimer->getUnixTimestamp() * Multiplier::Second;
  r += pTimer->getNanosecond();
  return r;
}

bool setTimeNanoseconds(Timestamp value) {
  Timer* timer = Machine::instance().getTimer();
  if (!timer || value == Infinity) {
    return false;
  }
  {
    LockGuard<Spinlock> guard(realtimeLock);
    const Timestamp now = timer->getTickCountNano();
    if (value < now) {
      return false;
    }
    realtimeBase = value;
    monotonicBase = now;
    realtimeSet = true;
  }
  // Publish to vDSO readers before the setting syscall returns.
  InfoBlockManager::instance().refreshTime();
  return true;
}

Timestamp getTicks() {
  Timer* pTimer = Machine::instance().getTimer();
  if (!pTimer) {
    return 0;
  }
  return pTimer->getTickCountNano();
}

}  // namespace Time

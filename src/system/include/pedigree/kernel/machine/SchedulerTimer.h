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

#ifndef KERNEL_MACHINE_SCHEDULERTIMER_H
#define KERNEL_MACHINE_SCHEDULERTIMER_H

class SchedulerTimerHandler;

/** @addtogroup kernelmachine
 * @{ */

/** Timer for scheduling */
class SchedulerTimer
{
  public:
    /**
     * Publish the sole hard scheduler-tick owner for the calling processor.
     * Registration, dispatch, and removal are processor-local operations.
     */
    virtual bool registerHandler(SchedulerTimerHandler *handler) = 0;
    /**
     * Remove the calling processor's exact current owner.
     *
     * A successful return is a callback-lifetime barrier: the processor's
     * timer hard frame cannot overlap ordinary removal, and later frames no
     * longer admit the callback. Cross-processor removal is rejected and
     * returns false; callers must arrange teardown on the registered CPU in
     * IRQ-enabled ordinary thread context. The callback's scoped dispatch
     * admission rejects self-removal; hard and synthetic interrupt contexts
     * are rejected before unpublication.
     */
    virtual bool removeHandler(SchedulerTimerHandler *handler) = 0;

  protected:
    /** The default constructor */
    SchedulerTimer();
    /** The destructor */
    virtual ~SchedulerTimer();

    /** Whether current execution can provide the same-CPU drain guarantee. */
    static bool canRemoveHandlerInCurrentContext();

  private:
    /** The copy-constructor
     *\note NOT implemented */
    SchedulerTimer(const SchedulerTimer &);
    /** The assignment operator
     *\note NOT implemented */
    SchedulerTimer &operator=(const SchedulerTimer &);
};

/** @} */

#endif

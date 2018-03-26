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

#ifndef KERNEL_UTILITIES_PRODUCERCONSUMER_H
#define KERNEL_UTILITIES_PRODUCERCONSUMER_H

#if defined(THREADS) || defined(UTILITY_LINUX)
#define PRODUCERCONSUMER_ASYNCHRONOUS 1
#else
#define PRODUCERCONSUMER_ASYNCHRONOUS 0
#endif

#if PRODUCERCONSUMER_ASYNCHRONOUS
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#endif

/**
 * ProducerConsumer provides a very light abstraction for a work queue.
 *
 * Producer(s) push tasks into the queue by calling produce(). All of these
 * work items are fire-and-forget - there is no way for callers to collect a
 * return status or any other kind of information about the completion of a
 * task. If this is important, a RequestQueue is a better choice.
 *
 * The consumer, which inherits from ProducerConsumer, implements consume().
 * As tasks are produced, consume() is called for each task, one at a time.
 *
 * In single-threaded environments, produce() just calls consume().
 */
class EXPORTED_PUBLIC ProducerConsumer
{
    public:
        ProducerConsumer();
        virtual ~ProducerConsumer();

        bool initialise();

        void produce(uint64_t p0 = 0, uint64_t p1 = 0, uint64_t p2 = 0, uint64_t p3 = 0,
                     uint64_t p4 = 0, uint64_t p5 = 0, uint64_t p6 = 0, uint64_t p7 = 0,
                     uint64_t p8 = 0);

    private:
        virtual void consume(uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3,
                             uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7,
                             uint64_t p8) = 0;

        void consumerThread();

        static int thread(void *p);

        struct Task
        {
            uint64_t p0, p1, p2, p3, p4, p5, p6, p7, p8;
        };

#if PRODUCERCONSUMER_ASYNCHRONOUS
        Mutex m_Lock;
        ConditionVariable m_Condition;
        List<Task *> m_Tasks;

        void *m_pThreadHandle = nullptr;
        bool m_Running = false;
#endif
};

#endif

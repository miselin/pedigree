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

#ifndef KERNEL_MACHINE_HOSTED_TICKSOURCE_H
#define KERNEL_MACHINE_HOSTED_TICKSOURCE_H
#include <config.h>

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/** A periodic signal source targeted at the hosted kernel execution thread. */
class HostedTickSource {
 public:
  enum class TakeResult {
    NotSource,
    Expirations,
    Invalid,
  };

  HostedTickSource();
  ~HostedTickSource();

  bool prepare(int signal, void* owner);
  bool arm(uint64_t intervalNanoseconds);
  bool disarm();
  void destroy();

  TakeResult takeExpirations(const siginfo_t* info, size_t& expirations);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Queues one source-owned expiration for deterministic scheduler tests. */
  bool queueExpirationForTest();
#endif

 private:
  HostedTickSource(const HostedTickSource&) = delete;
  HostedTickSource& operator=(const HostedTickSource&) = delete;

#if defined(__linux__)
  timer_t m_Timer;
#else
  static void* helperEntry(void* source);
  void helper();
  bool recordExpirations(size_t expirations);

  pthread_t m_TargetThread;
  pthread_t m_HelperThread;
  pthread_mutex_t m_Mutex;
  pthread_cond_t m_Condition;
  uint64_t m_IntervalNanoseconds;
  size_t m_PendingExpirations;
  size_t m_Generation;
  bool m_Armed;
  bool m_Delivering;
  bool m_Stop;
  bool m_Failed;
  bool m_HelperStarted;
#endif

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && defined(__linux__)
  size_t m_InjectedExpirations;
#endif
  int m_Signal;
  void* m_Owner;
  bool m_Prepared;
};

#endif

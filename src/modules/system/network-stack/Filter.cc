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

#include "Filter.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

NetworkFilter NetworkFilter::m_Instance;

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
NetworkFilter::CallbackPinHook NetworkFilter::m_CallbackPinHook = nullptr;
#endif

NetworkFilter::NetworkFilter()
    : m_Callbacks(), m_Lock(), m_NextCallbackId(1), m_pActiveInvocations(nullptr) {}

NetworkFilter::~NetworkFilter() {
  m_Lock.acquire();
  const bool callbackContext = isCallbackInvocation(Processor::information().getCurrentThread());
  m_Lock.release();
  if (callbackContext) {
    FATAL("NetworkFilter cannot be destroyed from callback context.");
  }

  for (size_t level = 1; level <= 4; ++level) {
    while (true) {
      m_Lock.acquire();
      CallbackItem* item =
          m_Callbacks[level - 1].count() ? *m_Callbacks[level - 1].begin() : nullptr;
      const size_t id = item ? item->id : 0;
      m_Lock.release();
      if (!item) {
        break;
      }
      if (!removeCallback(level, id)) {
        FATAL("NetworkFilter callback teardown did not complete.");
      }
    }
  }
}

bool NetworkFilter::filter(size_t level, uintptr_t packet, size_t sz) {
  if (!level || level > 4) {
    return true;
  }

  m_Lock.acquire();
  const size_t callbackBoundary = m_NextCallbackId;
  m_Lock.release();

  TerminationDeferral dispatchDeferral;
  Thread* current = Processor::information().getCurrentThread();
  bool accepted = true;
  size_t afterId = 0;
  while (accepted) {
    CallbackItem* item = nullptr;
    ActiveInvocation invocation = {nullptr, current, nullptr};

    m_Lock.acquire();
    for (auto candidate : m_Callbacks[level - 1]) {
      if (candidate->id <= afterId || candidate->id >= callbackBoundary) {
        continue;
      }

      afterId = candidate->id;
      if (!candidate->enabled) {
        continue;
      }

      item = candidate;
      invocation.item = item;
      ++item->inFlight;
      invocation.next = m_pActiveInvocations;
      m_pActiveInvocations = &invocation;
      break;
    }
    m_Lock.release();

    if (!item) {
      break;
    }

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
    CallbackPinHook hook = __atomic_load_n(&m_CallbackPinHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(item->callback, item->id);
    }
#endif
    accepted = item->callback(packet, sz);

    {
      auto completionGuard = item->drainWaiters.acquire();
      bool wakeDrainers = false;
      m_Lock.acquire();
      ActiveInvocation** link = &m_pActiveInvocations;
      while (*link && *link != &invocation) {
        link = &((*link)->next);
      }
      if (*link) {
        *link = invocation.next;
      } else {
        FATAL("NetworkFilter lost an active callback invocation.");
      }

      if (!item->inFlight) {
        FATAL("NetworkFilter callback pin underflow.");
      }
      --item->inFlight;
      if (!item->inFlight) {
        wakeDrainers = item->draining;
      }
      m_Lock.release();

      if (wakeDrainers) {
        completionGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(item));
      }
    }
  }

  return accepted;
}

size_t NetworkFilter::installCallback(size_t level, bool (*callback)(uintptr_t, size_t)) {
  if (!level || level > 4 || !callback) {
    return static_cast<size_t>(-1);
  }

  CallbackItem* item = new CallbackItem;
  item->callback = callback;
  item->inFlight = 0;
  item->removers = 0;
  item->enabled = true;
  item->draining = false;

  m_Lock.acquire();
  size_t id = m_NextCallbackId++;
  if (!id || id == static_cast<size_t>(-1)) {
    FATAL("NetworkFilter callback identifiers exhausted.");
  }
  item->id = id;
  m_Callbacks[level - 1].pushBack(item);
  m_Lock.release();

  return id;
}

bool NetworkFilter::removeCallback(size_t level, size_t id) {
  if (!level || level > 4 || id == static_cast<size_t>(-1)) {
    return false;
  }

  TerminationDeferral terminationDeferral;
  CallbackItem* item = nullptr;
  bool deleteNow = false;
  bool callbackRemoval = false;
  Thread* current = Processor::information().getCurrentThread();

  m_Lock.acquire();
  for (List<CallbackItem*>::Iterator it = m_Callbacks[level - 1].begin();
       it != m_Callbacks[level - 1].end(); ++it) {
    if ((*it)->id != id) {
      continue;
    }

    item = *it;
    item->enabled = false;
    callbackRemoval = isCallbackInvocation(current);
    if (callbackRemoval) {
      if (!item->draining && !item->inFlight) {
        m_Callbacks[level - 1].erase(it);
        deleteNow = true;
      }
    } else {
      item->draining = true;
      ++item->removers;
    }
    break;
  }
  m_Lock.release();

  if (!item) {
    return true;
  }
  if (deleteNow) {
    delete item;
    return true;
  } else if (!callbackRemoval) {
    drainCallback(level, item);
    return true;
  }
  return false;
}

void NetworkFilter::drainCallback(size_t level, CallbackItem* item) {
  while (true) {
    bool complete = false;
    {
      auto waitGuard = item->drainWaiters.acquire();
      m_Lock.acquire();
      if (!item->inFlight) {
        complete = true;
        m_Lock.release();
      } else {
        m_Lock.release();
        const WaitQueue::WakeReason reason =
            waitGuard.waitForCompletion(WaitQueue::Channel(item), Thread::CallbackDrain,
                                        reinterpret_cast<uintptr_t>(item->callback));
        (void)reason;
      }
    }
    if (complete) {
      break;
    }
  }

  bool deleteItem = false;
  m_Lock.acquire();
  if (!item->removers) {
    FATAL("NetworkFilter callback remover underflow.");
  }
  --item->removers;
  if (!item->removers) {
    for (List<CallbackItem*>::Iterator it = m_Callbacks[level - 1].begin();
         it != m_Callbacks[level - 1].end(); ++it) {
      if (*it == item) {
        m_Callbacks[level - 1].erase(it);
        deleteItem = true;
        break;
      }
    }
  }
  m_Lock.release();

  if (deleteItem) {
    delete item;
  }
}

bool NetworkFilter::isCallbackInvocation(Thread* thread) const {
  for (ActiveInvocation* invocation = m_pActiveInvocations; invocation;
       invocation = invocation->next) {
    if (invocation->thread == thread) {
      return true;
    }
  }
  return false;
}

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
void NetworkFilter::setCallbackPinHook(CallbackPinHook hook) {
  __atomic_store_n(&m_CallbackPinHook, hook, __ATOMIC_RELEASE);
}
#endif

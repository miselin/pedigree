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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"
#include "pedigree/kernel/utilities/new"

ZombieQueue ZombieQueue::m_Instance;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
ZombieProcess::ReapHook ZombieProcess::m_ReapHook = nullptr;
#endif

ZombieQueue::ZombieQueue() : RequestQueue(MakeConstantString("ZombieQueue")) {
  // Destruction publication transfers mandatory lifetime ownership. Unlike a
  // best-effort work queue, ZombieQueue cannot discard a burst above the
  // ordinary asynchronous backlog limit without leaking the target object.
  m_nMaxAsyncRequests = ~static_cast<size_t>(0);
}

ZombieQueue::~ZombieQueue() {
  RequestQueue::destroy();
}

ZombieQueue& ZombieQueue::instance() {
  return m_Instance;
}

void ZombieQueue::addObject(ZombieObject* pObject) {
  if (!pObject) {
    FATAL("ZombieQueue cannot publish a null object.");
    return;
  }

  if (!addAsyncRequest(1, reinterpret_cast<uint64_t>(pObject))) {
    FATAL("ZombieQueue rejected mandatory destruction work while stopped.");
  }
}

uint64_t ZombieQueue::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                     uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8) {
  if (!p1)
    return 0;

  delete reinterpret_cast<ZombieObject*>(p1);

  return 0;
}

void ZombieQueue::cancelRequest(const Request& request) {
  delete reinterpret_cast<ZombieObject*>(request.p1);
}

ZombieProcess::ZombieProcess(Process* pProcess) : m_pProcess(pProcess) {}

ZombieProcess::~ZombieProcess() {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  ReapHook reapHook = __atomic_load_n(&m_ReapHook, __ATOMIC_ACQUIRE);
  if (reapHook) {
    reapHook(m_pProcess, ReapPhase::Entered);
  }
#endif
  if (!m_pProcess->waitUntilTerminationReapable()) {
    WARNING(
        "ZombieQueue rejected an exiting Process before its off-stack "
        "completion; leaking it rather than deleting a live stack.");
    return;
  }
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (reapHook) {
    reapHook(m_pProcess, ReapPhase::Reapable);
  }
#endif
  m_pProcess->prepareForDestruction();
  delete m_pProcess;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void ZombieProcess::setReapHook(ReapHook hook) {
  __atomic_store_n(&m_ReapHook, hook, __ATOMIC_RELEASE);
}
#endif

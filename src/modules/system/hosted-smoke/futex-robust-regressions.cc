/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/time/Time.h"

#include <limits.h>
#include <stddef.h>
#include <time.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/pthread-syscalls.h"
#include "modules/subsys/posix/signal-syscalls.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"

namespace {
constexpr int PrivateWait = 128;
constexpr int PrivateWake = 129;
constexpr int PrivateRequeue = 131;
constexpr int PreservedErrno = 173;
constexpr uint32_t OwnerDied = 0x40000000;
constexpr uint32_t Waiters = 0x80000000;

struct CrossOwnerContext {
  WaitQueue queue;
  size_t firstOwner = 0;
  size_t secondOwner = 0;
  bool passed = false;
};
CrossOwnerContext* g_CrossOwner = nullptr;

void requeueAcrossOwners(WaitQueue* queue, Thread* thread, const WaitQueue::Channel&, size_t) {
  CrossOwnerContext* context = g_CrossOwner;
  if (!context || queue != &context->queue) {
    return;
  }
  WaitQueue::setBeforeBlockHook(nullptr);
  const WaitQueue::Channel source(&context->firstOwner, 11);
  const WaitQueue::Channel destination(&context->secondOwner, 29);
  auto guard = queue->acquire();
  bool passed = guard.wakeAndRequeue(source, 0, destination, 1) == 1;
  Thread::WaitDebugInfo info = {};
  passed &= thread->getWaitDebugInfo(info) && info.queue == queue && info.queued &&
            info.channelOwner == destination.owner && info.channelValue == destination.value;
  passed &= !guard.wakeOne(WaitQueue::WakeReason::Signalled, source);
  passed &= guard.wakeOne(WaitQueue::WakeReason::Signalled, destination);
  context->passed = passed;
}

bool crossOwnerRequeue() {
  CrossOwnerContext context;
  g_CrossOwner = &context;
  WaitQueue::setBeforeBlockHook(requeueAcrossOwners);
  auto guard = context.queue.acquire();
  const auto reason = guard.wait(WaitQueue::Channel(&context.firstOwner, 11), Thread::FutexWait);
  WaitQueue::setBeforeBlockHook(nullptr);
  g_CrossOwner = nullptr;
  return context.passed && reason == WaitQueue::WakeReason::Signalled &&
         context.queue.waiterCount() == 0;
}

struct PeerContext {
  WaitQueue queue;
  uintptr_t head = 0;
  bool release = false;
  Atomic<size_t> ready{0};
};

int peerWorker(void* parameter) {
  PeerContext* context = reinterpret_cast<PeerContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  thread->setRobustList(context->head, thread->getTaskId());
  context->ready += 1;
  while (true) {
    auto guard = context->queue.acquire();
    if (context->release) {
      return 0;
    }
    const auto reason = guard.wait();
    (void)reason;
  }
}

void releasePeer(PeerContext& context) {
  auto guard = context.queue.acquire();
  context.release = true;
  guard.wakeAll();
}

bool awaitReady(Atomic<size_t>& ready) {
  const Time::Timestamp deadline = Time::getTicks() + Time::Multiplier::Second * 2;
  while (!ready && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return ready == 1;
}

struct RobustNode {
  uint32_t owner;
  uint32_t padding;
  uintptr_t next;
};

struct UserFixture {
  uintptr_t next;
  intptr_t offset;
  uintptr_t pending;
  RobustNode held;
  RobustNode foreign;
  RobustNode acquiring;
  uintptr_t peerHead[3];
  uintptr_t returnedHead;
  size_t returnedLength;
  int source;
  int destination;
  timespec timeout;
  timespec zeroTimeout;
};

struct FutexContext {
  uintptr_t address;
  bool passed = false;
};

int futexWaiter(void* parameter) {
  FutexContext* context = reinterpret_cast<FutexContext*>(parameter);
  auto* fixture = reinterpret_cast<UserFixture*>(context->address);
  context->passed = posix_futex(&fixture->source, PrivateWait, 0,
                                reinterpret_cast<uintptr_t>(&fixture->timeout), nullptr, 0) == 0;
  return context->passed ? 0 : 1;
}

struct ContractContext {
  Thread* foreign;
  uintptr_t address = 0;
  uint32_t expectedExitOwner = 0;
  bool passed = false;
};

int contractWorker(void* parameter) {
  ContractContext* context = reinterpret_cast<ContractContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  VirtualAddressSpace* space = process->getAddressSpace();
  MemoryMapManager& mappings = MemoryMapManager::instance();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  if (!process->getSpaceAllocator().allocate(pageSize, address)) {
    return 1;
  }
  uintptr_t mappedAddress = address;
  if (!mappings.mapAnon(mappedAddress, pageSize,
                        MemoryMappedObject::Read | MemoryMappedObject::Write) ||
      mappedAddress != address) {
    return 1;
  }
  context->address = address;
  auto* fixture = reinterpret_cast<UserFixture*>(address);
  bool passed = thread->getId() == 1 && context->foreign->getId() == 1 &&
                thread->getTaskId() == process->getId() &&
                thread->getTaskId() != context->foreign->getTaskId() &&
                posix_gettid() == static_cast<pid_t>(thread->getId()) &&
                posix_gettid(true) == static_cast<pid_t>(thread->getTaskId()) &&
                posix_set_tid_address(nullptr, true) == static_cast<pid_t>(thread->getTaskId());

  // The first predicate check must fault in the anonymous page, then compare
  // atomically without enrolling a waiter when the value differs.
  thread->setErrno(0);
  passed &= posix_futex(&fixture->source, PrivateWait, 1, 0, nullptr, 0) == -1 &&
            thread->getErrno() == Error::NoMoreProcesses;

  UserFixture initial = {};
  initial.next = reinterpret_cast<uintptr_t>(&fixture->held.next);
  initial.offset = -static_cast<intptr_t>(offsetof(RobustNode, next));
  initial.pending = reinterpret_cast<uintptr_t>(&fixture->acquiring.next);
  initial.held = {static_cast<uint32_t>(thread->getTaskId()) | Waiters, 0,
                  reinterpret_cast<uintptr_t>(&fixture->foreign.next)};
  initial.foreign = {static_cast<uint32_t>(context->foreign->getTaskId()), 0, address};
  initial.acquiring = {static_cast<uint32_t>(thread->getTaskId()), 0, 0};
  initial.peerHead[0] = reinterpret_cast<uintptr_t>(fixture->peerHead);
  initial.timeout.tv_sec = 2;
  passed &= PosixSubsystem::copyToUser(fixture, &initial, sizeof(initial));

  thread->setErrno(0);
  passed &= posix_futex(&fixture->source, PrivateWait, 0,
                        reinterpret_cast<uintptr_t>(&fixture->zeroTimeout), nullptr, 0) == -1 &&
            thread->getErrno() == Error::TimedOut;
  uint32_t observed = 0;
  bool exchanged = false;
  passed &= space->tryReadUser32(reinterpret_cast<uintptr_t>(&fixture->source), observed) &&
            observed == 0;
  passed &= mappings.setPermissions(address, pageSize, MemoryMappedObject::Read) == 1;
  passed &= !space->tryCompareExchangeUser32(reinterpret_cast<uintptr_t>(&fixture->source),
                                             observed, 7, exchanged) &&
            !exchanged;
  passed &= mappings.setPermissions(address, pageSize, MemoryMappedObject::None) == 1;
  passed &= !space->tryReadUser32(reinterpret_cast<uintptr_t>(&fixture->source), observed);
  passed &= mappings.setPermissions(address, pageSize,
                                    MemoryMappedObject::Read | MemoryMappedObject::Write) == 1;
  passed &= space->tryReadUser32(reinterpret_cast<uintptr_t>(&fixture->source), observed) &&
            observed == 0;

  FutexContext futex{address};
  Thread* waiter = new Thread(process, futexWaiter, &futex, nullptr, false, true, true);
  bool waiterStarted = waiter->start();
  bool enrolled = false;
  const Time::Timestamp deadline = Time::getTicks() + Time::Multiplier::Second;
  while (waiterStarted && Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    if (waiter->getWaitDebugInfo(info) && info.queued && info.channelOwner == space &&
        info.channelValue == reinterpret_cast<uintptr_t>(&fixture->source)) {
      enrolled = true;
      break;
    }
    Scheduler::instance().yield();
  }
  passed &= enrolled &&
            posix_futex(&fixture->source, PrivateRequeue, 0, 1, &fixture->destination, 0) == 1;
  passed &= posix_futex(&fixture->source, PrivateWake, 1, 0, nullptr, 0) == 0;
  const int destinationWoken = posix_futex(&fixture->destination, PrivateWake, 1, 0, nullptr, 0);
  passed &= destinationWoken == 1;
  if (!destinationWoken) {
    posix_futex(&fixture->source, PrivateWake, 1, 0, nullptr, 0);
  }
  const bool waiterJoined = waiterStarted && waiter->joinForCompletion();
  if (!waiterStarted) {
    delete waiter;
  }
  passed &= waiterJoined && futex.passed;

  auto** outputHead = reinterpret_cast<robust_list_head**>(&fixture->returnedHead);
  thread->setErrno(PreservedErrno);
  passed &= posix_get_robust_list(0, outputHead, &fixture->returnedLength, true) == 0 &&
            thread->getErrno() == PreservedErrno && fixture->returnedHead == 0 &&
            fixture->returnedLength == 3 * sizeof(uintptr_t);
  thread->setErrno(0);
  passed &= posix_set_robust_list(reinterpret_cast<robust_list_head*>(address), 0, true) == -1 &&
            thread->getErrno() == Error::InvalidArgument;
  passed &= posix_set_robust_list(reinterpret_cast<robust_list_head*>(address),
                                  3 * sizeof(uintptr_t), true) == 0;

  PeerContext peer;
  peer.head = reinterpret_cast<uintptr_t>(fixture->peerHead);
  Thread* buddy = new Thread(process, peerWorker, &peer, nullptr, false, true, true);
  const bool buddyStarted = buddy->start();
  const bool buddyReady = buddyStarted && awaitReady(peer.ready);
  passed &= buddyReady && buddy->getTaskId() != thread->getTaskId() &&
            buddy->getTaskId() != context->foreign->getTaskId();
  passed &= posix_get_robust_list(static_cast<int>(buddy->getTaskId()), outputHead,
                                  &fixture->returnedLength, true) == 0 &&
            fixture->returnedHead == peer.head && thread->getRobustList() == address;
  thread->setErrno(0);
  passed &= posix_get_robust_list(static_cast<int>(context->foreign->getTaskId()), outputHead,
                                  &fixture->returnedLength, true) == -1 &&
            thread->getErrno() == Error::NotEnoughPermissions;
  thread->setErrno(0);
  passed &= posix_get_robust_list(INT_MAX, outputHead, &fixture->returnedLength, true) == -1 &&
            thread->getErrno() == Error::NoSuchProcess;
  thread->setErrno(0);
  passed &= posix_get_robust_list(0, nullptr, &fixture->returnedLength, true) == -1 &&
            thread->getErrno() == Error::BadAddress;
  passed &= posix_tkill(static_cast<int>(buddy->getTaskId()), 0, true) == 0 &&
            posix_tgkill(static_cast<int>(process->getId()), static_cast<int>(buddy->getTaskId()),
                         0, true) == 0 &&
            posix_tkill(static_cast<int>(context->foreign->getTaskId()), 0, true) == 0;
  thread->setErrno(0);
  passed &= posix_tgkill(static_cast<int>(process->getId()),
                         static_cast<int>(context->foreign->getTaskId()), 0, true) == -1 &&
            thread->getErrno() == Error::NoSuchProcess;
  releasePeer(peer);
  const bool buddyJoined = buddyStarted && buddy->joinForCompletion();
  if (!buddyStarted) {
    delete buddy;
  }
  passed &= buddyJoined;

  posix_robust_list_exit(thread);
  passed &= thread->getRobustList() == 0 && fixture->held.owner == (OwnerDied | Waiters) &&
            fixture->acquiring.owner == OwnerDied &&
            fixture->foreign.owner == context->foreign->getTaskId();
  // Keep one valid owned entry registered to check the actual shutdown hook
  // from the parent after this Thread has completed.
  fixture->held.owner = static_cast<uint32_t>(thread->getTaskId());
  fixture->held.next = address;
  fixture->pending = 0;
  context->expectedExitOwner = OwnerDied;
  passed &= posix_set_robust_list(reinterpret_cast<robust_list_head*>(address),
                                  3 * sizeof(uintptr_t), true) == 0;
  context->passed = passed;
  return passed ? 0 : 1;
}

int cleanupWorker(void* parameter) {
  ContractContext* context = reinterpret_cast<ContractContext*>(parameter);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (context->address) {
    MemoryMapManager::instance().remove(context->address, pageSize);
    Processor::information().getCurrentThread()->getParent()->getSpaceAllocator().free(
        context->address, pageSize);
  }
  return 0;
}
}  // namespace

bool runHostedFutexRobustRegressions(Process* kernelProcess) {
  bool passed = crossOwnerRequeue();
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->publish();
  PosixProcess* foreign = new PosixProcess(kernelProcess);
  foreign->setSubsystem(new PosixSubsystem);
  foreign->publish();
  PeerContext foreignContext;
  Thread* foreignThread =
      new Thread(foreign, peerWorker, &foreignContext, nullptr, false, true, true);
  const bool foreignStarted = foreignThread->start();
  passed &= foreignStarted && awaitReady(foreignContext.ready);
  ContractContext context{foreignThread};
  Thread* worker = new Thread(process, contractWorker, &context, nullptr, false, true, true);
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  uint32_t exitOwner = 0;
  const bool recovered =
      context.address &&
      process->getAddressSpace()->tryReadUser32(
          context.address + offsetof(UserFixture, held) + offsetof(RobustNode, owner), exitOwner) &&
      exitOwner == context.expectedExitOwner;
  passed &= joined && context.passed && recovered;
  releasePeer(foreignContext);
  const bool foreignJoined = foreignStarted && foreignThread->joinForCompletion();
  if (!foreignStarted) {
    delete foreignThread;
  }
  passed &= foreignJoined;
  Thread* cleanup = new Thread(process, cleanupWorker, &context, nullptr, false, true);
  passed &= cleanup->joinForCompletion();
  delete foreign;
  delete process;
  if (passed) {
    NOTICE("HOSTED-SYSCALL-TEST: PASS futex-robust-contracts");
  } else {
    ERROR("HOSTED-SYSCALL-TEST: FAIL futex-robust-contracts: queries=" << context.passed
                                                                       << ", exit=" << recovered);
  }
  return passed;
}

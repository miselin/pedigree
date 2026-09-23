/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Rcu.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"

namespace {
struct Value {
  size_t value;
  Atomic<size_t>* reclaimed;
};

void reclaim(void* pointer) {
  auto* value = static_cast<Value*>(pointer);
  *value->reclaimed += 1;
  delete value;
}

struct Context {
  RcuPointer<Value> publication;
  Atomic<size_t> reclaimed{0}, ready{0}, release{0}, failed{0}, draining{0}, drained{0};
  RcuRetireQueue retired;
  Atomic<size_t> readerCpu{~size_t{0}};
  size_t writerCpu = Processor::index();
};

int readRemotely(void* parameter) {
  auto& context = *static_cast<Context*>(parameter);
  const size_t cpu = Processor::index();
  if (cpu == context.writerCpu || !context.readerCpu.compareAndSwap(~size_t{0}, cpu)) {
    return 0;
  }
  {
    RcuReadGuard outer;
    const Value* value = context.publication.load(outer);
    {
      RcuReadGuard inner;
      if (context.publication.load(inner) != value || Processor::getInterrupts()) {
        context.failed = 1;
      }
    }
    context.ready = 1;
    const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
    while (!context.release.value() && Time::getTicks() < deadline) {
      Processor::pause();
    }
    if (!context.release.value() || !value || value->value != 17 || context.reclaimed.value()) {
      context.failed = 1;
    }
  }
  if (!Processor::getInterrupts()) {
    context.failed = 1;
  }
  return 0;
}

int drainRemotely(void* parameter) {
  auto& context = *static_cast<Context*>(parameter);
  context.draining = 1;
  context.retired.drain();
  context.drained = 1;
  return 0;
}

bool waitFor(Atomic<size_t>& flag) {
  const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
  while (!flag.value() && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return flag.value() != 0;
}
}  // namespace

bool runRcuConcurrencyRegression() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN rcu-publication-reclamation-smp");
  const size_t cpus = Processor::getCount();
  if (cpus < 4 || cpus > 64) {
    return false;
  }
  Context context;
  context.publication.exchange(new Value{17, &context.reclaimed});
  Thread* readers[64] = {};
  for (size_t i = 0; i < cpus; ++i) {
    readers[i] = new Thread(Scheduler::instance().getKernelProcess(), readRemotely, &context,
                            nullptr, false, false, true);
    if (!readers[i]->start()) {
      FATAL("RCU smoke reader could not start");
    }
  }
  if (!waitFor(context.ready)) {
    FATAL("RCU smoke reader did not enter on another CPU");
  }
  Value* previous = context.publication.exchange(new Value{29, &context.reclaimed});
  context.retired.retire(previous, reclaim);
  ThreadPlacement placement;
  placement.allowed.set(context.writerCpu);
  Thread* drainer = new Thread(Scheduler::instance().getKernelProcess(), drainRemotely, &context,
                               nullptr, false, false, true, &placement);
  if (!drainer->start() || !waitFor(context.draining)) {
    FATAL("RCU smoke reclaimer could not start");
  }
  for (size_t i = 0; i < 64; ++i) {
    Scheduler::instance().yield();
  }
  if (context.drained.value() || context.reclaimed.value()) {
    context.failed = 1;
  }
  context.release = 1;
  bool joined = drainer->joinForCompletion();
  for (size_t i = 0; i < cpus; ++i) {
    joined = readers[i]->joinForCompletion() && joined;
  }
  if (!joined || context.failed.value() || context.reclaimed.value() != 1) {
    return false;
  }

  previous = context.publication.exchange(nullptr);
  context.retired.retire(previous, reclaim);
  for (size_t i = 1; i <= RcuRetireQueue::Capacity; ++i) {
    context.retired.retire(new Value{i, &context.reclaimed}, reclaim);
  }
  if (context.reclaimed.value() != 1 + RcuRetireQueue::Capacity) {
    return false;
  }
  context.retired.drain();
  if (context.reclaimed.value() != 2 + RcuRetireQueue::Capacity) {
    return false;
  }
  NOTICE("QEMU-CONCURRENCY-TEST: RCU reader CPU=" << Dec << context.readerCpu.value()
                                                  << ", writer CPU=" << context.writerCpu);
  NOTICE("QEMU-CONCURRENCY-TEST: PASS rcu-publication-reclamation-smp");
  return true;
}

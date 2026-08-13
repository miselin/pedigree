/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/AdmittedThread.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
namespace {
AdmittedThread::BeforeStartHook g_BeforeStartHook = nullptr;
void* g_BeforeStartHookContext = nullptr;
}  // namespace
#endif

struct AdmittedThread::Context {
  Context(Entry entry, void* parameter, Cancel cancel, OperationBarrier::Lease&& admission)
      : entry(entry),
        parameter(parameter),
        cancel(cancel),
        admission(pedigree_std::move(admission)) {}

  Entry entry;
  void* parameter;
  Cancel cancel;
  OperationBarrier::Lease admission;
};

bool AdmittedThread::launchDetached(Entry entry, void* parameter, Cancel cancel,
                                    OperationBarrier& barrier, const char* name) {
  if (!entry) {
    return false;
  }

  OperationBarrier::Lease admission;
  if (!barrier.tryAcquire(admission)) {
    return false;
  }

  Context* context = new Context(entry, parameter, cancel, pedigree_std::move(admission));
  if (!context) {
    return false;
  }

  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  Thread* thread =
      new Thread(kernelProcess, trampoline, context, nullptr, false, true, true, cancelBeforeStart);
  if (!thread) {
    delete context;
    return false;
  }

  if (name) {
    thread->setName(String(name));
  }

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
  if (g_BeforeStartHook) {
    g_BeforeStartHook(thread, g_BeforeStartHookContext);
  }
#endif
  if (!thread->startDetached()) {
    panic("AdmittedThread could not publish its detached worker.");
  }

  return true;
}

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
void AdmittedThread::setBeforeStartHookForTest(BeforeStartHook hook, void* context) {
  g_BeforeStartHook = hook;
  g_BeforeStartHookContext = context;
}
#endif

void AdmittedThread::cancelBeforeStart(void* parameter) {
  Context* context = reinterpret_cast<Context*>(parameter);
  if (context->cancel) {
    context->cancel(context->parameter);
  }
  delete context;
}

int AdmittedThread::trampoline(void* parameter) {
  Context* context = reinterpret_cast<Context*>(parameter);
  const Entry entry = context->entry;
  void* entryParameter = context->parameter;

  const int result = entry(entryParameter);

  // This destructor releases admission in kernel text. Once a close waiter
  // wakes, no return address or live frame still points into the entry module.
  delete context;
  return result;
}

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
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/initialiseMultitasking.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/state.h"

#include "HostedPlatform.h"
#include "InterruptManager.h"
#include "PhysicalMemoryManager.h"
#include "SyscallManager.h"
#include "VirtualAddressSpace.h"

namespace __pedigree_hosted {};
using namespace __pedigree_hosted;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
ProcessorBase::HostedContextSwitchHook g_HostedContextSwitchHook = nullptr;
}
#endif

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#endif

namespace {
void addHostedIrqSignals(sigset_t& set) {
  sigaddset(&set, SIGUSR1);
  sigaddset(&set, SIGUSR2);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  sigaddset(&set, SIGURG);
#endif
}
}  // namespace

bool ProcessorBase::m_bInterrupts;

#if HAS_SANITIZERS
// We are basically using fibers as we do scheduling ourselves. So we need to
// make sure we are annotating these correctly.
extern "C" {
// TODO: to use these we need to know the full stack size, but that's not
// available to us in the switch functions. Need to change Scheduler?
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);

void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old,
                                     size_t* size_old);
}
#endif

typedef void (*jump_func_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

extern "C" {
#if defined(__APPLE__)
uintptr_t hosted_kernel_fs_base = 0;
uintptr_t hosted_user_fs_base = 0;

long hostedCaptureKernelFs() {
  return 0;
}
#else
extern uintptr_t hosted_kernel_fs_base;
extern uintptr_t hosted_user_fs_base;
long hostedCaptureKernelFs();
#endif
}

namespace {
uintptr_t currentHostedExecutionThread() {
  static_assert(sizeof(pthread_t) <= sizeof(uintptr_t),
                "pthread_t does not fit in the hosted execution-thread identity");
  const pthread_t thread = pthread_self();
  uintptr_t identity = 0;
  __builtin_memcpy(&identity, &thread, sizeof(thread));
  return identity;
}

pthread_t hostedExecutionThread(uintptr_t identity) {
  pthread_t thread = {};
  __builtin_memcpy(&thread, &identity, sizeof(thread));
  return thread;
}
}  // namespace

void ProcessorBase::initialisationDone() {
  HostedPhysicalMemoryManager::instance().initialisationDone();
}

void ProcessorBase::initialise1(const BootstrapStruct_t& Info) {
  const size_t hostPageSize = HostedPlatform::pageSize();
  if (!hostPageSize || TargetInfo::getPageSize() < hostPageSize ||
      (TargetInfo::getPageSize() % hostPageSize) != 0) {
    panic("Hosted: incompatible target and host page sizes");
  }
  NOTICE("HOSTED-PAGE-GEOMETRY: target=" << Dec << TargetInfo::getPageSize()
                                         << " host=" << hostPageSize << Hex);

  const uintptr_t executionThreadId = currentHostedExecutionThread();
  if (!executionThreadId) {
    panic("Hosted: failed to capture the processor execution thread");
  }
  __atomic_store_n(&information().m_HostedExecutionThreadId, executionThreadId, __ATOMIC_RELEASE);

  if (hostedCaptureKernelFs() != 0) {
    panic("Hosted: failed to capture the host FS base");
  }
  hosted_user_fs_base = hosted_kernel_fs_base;

  HostedInterruptManager::initialiseProcessor();
  PageFaultHandler::instance().initialise();
  HostedPhysicalMemoryManager::instance().initialise(Info);
  HostedSyscallManager::initialiseProcessor();
  setInterrupts(false);
  m_Initialised = 1;
}

void ProcessorBase::initialise2(const BootstrapStruct_t& Info) {
  initialiseMultitasking();
  m_Initialised = 2;
}

void ProcessorBase::deinitialise() {}

void ProcessorBase::identify(HugeStaticString& str) {
  str.clear();
  str.append("Hosted Processor");
}

uintptr_t ProcessorBase::getInstructionPointer() {
  return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
}

uintptr_t ProcessorBase::getStackPointer() {
  return 0;
}

uintptr_t ProcessorBase::getBasePointer() {
  return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
}

bool ProcessorBase::saveState(SchedulerState& state) {
  FATAL("ProcessorBase::saveState is NOT safe on HOSTED builds.");
  return false;
}

void ProcessorBase::restoreState(SchedulerState& state, volatile uintptr_t* pLock) {
  if (pLock)
    *pLock = 1;

#if HAS_SANITIZERS
  __sanitizer_start_switch_fiber(nullptr, reinterpret_cast<void*>(state.stackBase),
                                 state.stackSize);
#endif

  setcontext(reinterpret_cast<ucontext_t*>(state.state));
  FATAL("Hosted: setcontext failed in Processor::restoreState");
  // Does not return.
}

#if defined(__APPLE__)
void ProcessorBase::restoreState(SyscallState& state, volatile uintptr_t* pLock) {
  (void)state;
  (void)pLock;
  FATAL("Hosted Darwin does not emulate userspace syscall return");
  __builtin_unreachable();
}

void ProcessorBase::jumpUser(volatile uintptr_t* pLock, uintptr_t address, uintptr_t stack,
                             uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  (void)pLock;
  (void)address;
  (void)stack;
  (void)p1;
  (void)p2;
  (void)p3;
  (void)p4;
  FATAL("Hosted Darwin does not emulate userspace entry");
  __builtin_unreachable();
}
#endif

#if SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH
void ProcessorBase::switchState(bool bInterrupts, SchedulerState& a, SchedulerState& b,
                                volatile uintptr_t* pLock) {
  void* fake_stack_save = nullptr;

  ucontext_t* a_ctx = reinterpret_cast<ucontext_t*>(a.state);
  ucontext_t* b_ctx = reinterpret_cast<ucontext_t*>(b.state);

  if (pLock)
    *pLock = 1;
#if HAS_SANITIZERS
  __sanitizer_start_switch_fiber(&fake_stack_save, reinterpret_cast<void*>(b.stackBase),
                                 b.stackSize);
#endif
  swapcontext(a_ctx, b_ctx);
#if HAS_SANITIZERS
  __sanitizer_finish_switch_fiber(fake_stack_save, nullptr, nullptr);
#endif
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  notifyHostedContextSwitchStage(HostedContextSwitchStage::SwitchStateReturnedMasked);
#endif
}

static void syscallStateWrapper(uintptr_t state, uintptr_t lock, uintptr_t sourceState);

void ProcessorBase::switchState(bool bInterrupts, SchedulerState& a, SyscallState& b,
                                volatile uintptr_t* pLock) {
  Thread* target = Processor::information().getCurrentThread();
  size_t stackSize = 0;
  void* stackBase = target ? target->getKernelStackBase(&stackSize) : nullptr;
  const uintptr_t stackBegin = reinterpret_cast<uintptr_t>(stackBase);
  const uintptr_t stateAddress = reinterpret_cast<uintptr_t>(&b);
  if (!stackBegin || stackSize < sizeof(SyscallState) || stateAddress < stackBegin ||
      stateAddress - stackBegin != stackSize - sizeof(SyscallState)) {
    FATAL("Hosted syscall state is not on the target kernel stack");
  }

  ucontext_t newContext;
  getcontext(&newContext);
  newContext.uc_stack.ss_sp = stackBase;
  // The copied state is consumed later by syscall_tail and must not become
  // part of makecontext's stack frame.
  newContext.uc_stack.ss_size = stateAddress - stackBegin;
  newContext.uc_link = nullptr;
  makecontext(&newContext, reinterpret_cast<void (*)()>(syscallStateWrapper), 3, stateAddress,
              reinterpret_cast<uintptr_t>(pLock), reinterpret_cast<uintptr_t>(&a));

#if HAS_SANITIZERS
  void* fakeStackSave = nullptr;
  __sanitizer_start_switch_fiber(&fakeStackSave, newContext.uc_stack.ss_sp,
                                 newContext.uc_stack.ss_size);
#endif
  swapcontext(reinterpret_cast<ucontext_t*>(a.state), &newContext);
#if HAS_SANITIZERS
  __sanitizer_finish_switch_fiber(fakeStackSave, nullptr, nullptr);
#endif
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  notifyHostedContextSwitchStage(HostedContextSwitchStage::SwitchStateReturnedMasked);
#endif
  Processor::setInterrupts(bInterrupts);
}

#if HAS_SANITIZERS
static void finishInitialFiberSwitch(uintptr_t sourceState) {
  const void* sourceStack = nullptr;
  size_t sourceStackSize = 0;
  __sanitizer_finish_switch_fiber(nullptr, &sourceStack, &sourceStackSize);

  SchedulerState* source = reinterpret_cast<SchedulerState*>(sourceState);
  if (source) {
    source->stackBase = reinterpret_cast<uintptr_t>(sourceStack);
    source->stackSize = sourceStackSize;
  }
}
#endif

static void syscallStateWrapper(uintptr_t state, uintptr_t lock, uintptr_t sourceState) {
#if HAS_SANITIZERS
  finishInitialFiberSwitch(sourceState);
#else
  (void)sourceState;
#endif

  volatile uintptr_t* sourceLock = reinterpret_cast<volatile uintptr_t*>(lock);
  if (sourceLock) {
    *sourceLock = 1;
  }
  // A syscall return enters userspace with IRQs enabled even if clone's
  // kernel-side publication temporarily masked them.
  Processor::setInterrupts(true);
  Processor::restoreState(*reinterpret_cast<SyscallState*>(state), nullptr);
}

static void threadWrapper(uintptr_t func, volatile uintptr_t* pLock, uintptr_t bInterrupts,
                          uintptr_t sourceState, uintptr_t p1, uintptr_t p2, uintptr_t p3,
                          uintptr_t p4) {
#if HAS_SANITIZERS
  finishInitialFiberSwitch(sourceState);
#else
  (void)sourceState;
#endif

  if (pLock) {
    // unlock other thread now that we are on the new stack
    *pLock = 1;
  }
  // The saved interrupt state belongs to the thread which created us.
  // Native kernel-thread entry always starts with interrupts enabled.
  (void)bInterrupts;
  Processor::setInterrupts(true);
  auto entry = reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(func);
  entry(p1, p2, p3, p4);
  Thread::threadExited();
}

static void userThreadWrapper(uintptr_t func, volatile uintptr_t* pLock, uintptr_t bInterrupts,
                              uintptr_t sourceState, uintptr_t stack, uintptr_t p1, uintptr_t p2,
                              uintptr_t p3, uintptr_t p4) {
#if HAS_SANITIZERS
  finishInitialFiberSwitch(sourceState);
#else
  (void)sourceState;
#endif

  if (pLock) {
    // The wrapper is already running on the new kernel stack.
    *pLock = 1;
  }
  Processor::setInterrupts(bInterrupts != 0);

  // The ELF interpreter consumes the initial process state directly from
  // RSP, so the final transition cannot use a C call frame.
  Processor::jumpUser(nullptr, func, stack, p1, p2, p3, p4);
}

void ProcessorBase::jumpKernel(volatile uintptr_t* pLock, uintptr_t address, uintptr_t stack,
                               uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  FATAL("Hosted: jumpKernel() is not supported - an atomic context switch is needed");
}

void ProcessorBase::saveAndJumpKernel(bool bInterrupts, SchedulerState& s,
                                      volatile uintptr_t* pLock, uintptr_t address, uintptr_t stack,
                                      uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  assert(stack);

  uintptr_t stackBottom = stack - KERNEL_STACK_SIZE;

  ucontext_t new_context;
  getcontext(&new_context);
  new_context.uc_stack.ss_sp = adjust_pointer(reinterpret_cast<void*>(stack), -KERNEL_STACK_SIZE);
  new_context.uc_stack.ss_size = KERNEL_STACK_SIZE;
  new_context.uc_link = NULL;
  makecontext(&new_context, reinterpret_cast<void (*)()>(threadWrapper), 8, address,
              reinterpret_cast<uintptr_t>(pLock), static_cast<uintptr_t>(bInterrupts),
              reinterpret_cast<uintptr_t>(&s), p1, p2, p3, p4);

#if HAS_SANITIZERS
  void* fake_stack_save = nullptr;
  // NOTICE("make sp [saveAndJumpKernel]: " << new_context.uc_stack.ss_sp);
  // NOTICE(" old -> " << reinterpret_cast<ucontext_t *>(s.state) << " / sp=" <<
  // reinterpret_cast<ucontext_t *>(s.state)->uc_stack.ss_sp);
  __sanitizer_start_switch_fiber(&fake_stack_save, new_context.uc_stack.ss_sp,
                                 new_context.uc_stack.ss_size);
#endif
  swapcontext(reinterpret_cast<ucontext_t*>(s.state), &new_context);
#if HAS_SANITIZERS
  __sanitizer_finish_switch_fiber(fake_stack_save, nullptr, nullptr);
#endif
}

void ProcessorBase::saveAndJumpUser(bool bInterrupts, SchedulerState& s, volatile uintptr_t* pLock,
                                    uintptr_t address, uintptr_t stack, uintptr_t p1, uintptr_t p2,
                                    uintptr_t p3, uintptr_t p4) {
  assert(stack);

  ucontext_t new_context;
  getcontext(&new_context);
  new_context.uc_stack.ss_sp = adjust_pointer(reinterpret_cast<void*>(stack), -KERNEL_STACK_SIZE);
  new_context.uc_stack.ss_size = KERNEL_STACK_SIZE;
  new_context.uc_link = NULL;
  makecontext(&new_context, reinterpret_cast<void (*)()>(userThreadWrapper), 9, address,
              reinterpret_cast<uintptr_t>(pLock), static_cast<uintptr_t>(bInterrupts),
              reinterpret_cast<uintptr_t>(&s), stack, p1, p2, p3, p4);

#if HAS_SANITIZERS
  void* fake_stack_save = nullptr;
  __sanitizer_start_switch_fiber(&fake_stack_save, new_context.uc_stack.ss_sp,
                                 new_context.uc_stack.ss_size);
#endif
  swapcontext(reinterpret_cast<ucontext_t*>(s.state), &new_context);
#if HAS_SANITIZERS
  __sanitizer_finish_switch_fiber(fake_stack_save, nullptr, nullptr);
#endif
}
#endif  // SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH

void ProcessorBase::switchAddressSpace(VirtualAddressSpace& AddressSpace) {
  ProcessorInformation& info = Processor::information();
  if (&info.getVirtualAddressSpace() != &AddressSpace) {
    HostedVirtualAddressSpace::switchAddressSpace(info.getVirtualAddressSpace(), AddressSpace);
    info.setVirtualAddressSpace(AddressSpace);
  }
}

void ProcessorBase::setTlsBase(uintptr_t newBase) {
  // Kernel code retains the host runtime's FS base. The raw user transition
  // installs this value after all C++ work is complete.
  hosted_user_fs_base = newBase;
}

size_t ProcessorBase::getDebugBreakpointCount() {
  return 0;
}

uintptr_t ProcessorBase::getDebugBreakpoint(size_t nBpNumber, DebugFlags::FaultType& nFaultType,
                                            size_t& nLength, bool& bEnabled) {
  // no-op on hosted
  return 0;
}

void ProcessorBase::enableDebugBreakpoint(size_t nBpNumber, uintptr_t nLinearAddress,
                                          DebugFlags::FaultType nFaultType, size_t nLength) {
  // no-op on hosted
}

void ProcessorBase::disableDebugBreakpoint(size_t nBpNumber) {
  // no-op on hosted
}

void ProcessorBase::setInterrupts(bool bEnable) {
  // Block signals to toggle "interrupts".
  sigset_t set;
  if (bEnable) {
    sigemptyset(&set);
#if THREADS
    Thread* current = Processor::information().getCurrentThread();
    const bool signalFrameOnCurrentStack =
        current ? current->getHostedSignalDepth() : inHostedSignalFrame();
    if (signalFrameOnCurrentStack) {
      // SS_AUTODISARM lets another Thread or state stack take an IRQ
      // while this physical stack retains its suspended signal frame.
      addHostedIrqSignals(set);
    }
#endif
  } else {
    sigemptyset(&set);

    // Timer signals and the smoke-only synthetic device signal are true
    // interrupts. Exceptions remain deliverable while IRQs are disabled.
    addHostedIrqSignals(set);
  }

  // We must mark interrupts enabled before we unmask signals, as any pending
  // signals may trigger immediately (and will cause problems if interrupts
  // are marked as disabled)
  if (bEnable) {
    m_bInterrupts = true;
  }

  int r = pthread_sigmask(SIG_SETMASK, &set, 0);
  if (r != 0) {
    ERROR("ProcessorBase::setInterrupts failed to set new mask");
  }

  // We can only mark interrupts disabled after masking signals as during the
  // mask operation signals may still come in. Setting the flag here means
  // those signals are handled correctly.
  if (!bEnable) {
    m_bInterrupts = false;
  }
}

bool ProcessorBase::getInterrupts() {
  return m_bInterrupts;
}

void ProcessorBase::maskInterruptsForSignalReturn() {
  sigset_t set;
  sigemptyset(&set);
  addHostedIrqSignals(set);
  if (pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) {
    FATAL_NOLOCK("Hosted signal return failed to mask IRQ signals");
  }
}

uintptr_t ProcessorBase::hostedExecutionThreadId() {
  return __atomic_load_n(&information().m_HostedExecutionThreadId, __ATOMIC_ACQUIRE);
}

bool ProcessorBase::onHostedExecutionThread() {
  const uintptr_t executionThreadId = hostedExecutionThreadId();
  return executionThreadId && currentHostedExecutionThread() == executionThreadId;
}

void ProcessorBase::enterHostedSignalFrame() {
  __atomic_add_fetch(&information().m_HostedSignalFrameDepth, 1, __ATOMIC_ACQ_REL);
}

void ProcessorBase::leaveHostedSignalFrame() {
  const size_t previous =
      __atomic_fetch_sub(&information().m_HostedSignalFrameDepth, 1, __ATOMIC_ACQ_REL);
  if (UNLIKELY(!previous)) {
    __atomic_store_n(&information().m_HostedSignalFrameDepth, 0, __ATOMIC_RELEASE);
    FATAL_NOLOCK("Hosted signal-frame depth underflowed");
  }
}

bool ProcessorBase::inHostedSignalFrame() {
  return __atomic_load_n(&information().m_HostedSignalFrameDepth, __ATOMIC_ACQUIRE) != 0;
}

#if PEDIGREE_HOSTED_SMOKE_TESTS
size_t ProcessorBase::hostedSignalFrameDepthForTest() {
  return __atomic_load_n(&information().m_HostedSignalFrameDepth, __ATOMIC_ACQUIRE);
}
#endif

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void ProcessorBase::setHostedContextSwitchHook(HostedContextSwitchHook hook) {
  __atomic_store_n(&g_HostedContextSwitchHook, hook, __ATOMIC_RELEASE);
}

void ProcessorBase::notifyHostedContextSwitchStage(HostedContextSwitchStage stage) {
  HostedContextSwitchHook hook = __atomic_load_n(&g_HostedContextSwitchHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(stage);
  }
}

#endif

void ProcessorBase::setSingleStep(bool bEnable, InterruptState& state) {
  // no-op on hosted
}

void ProcessorBase::invalidate(void* pAddress) {
  // no-op on hosted
}

TlbInvalidationResult ProcessorBase::beginTlbInvalidation(TlbInvalidationGuard& guard) {
  const ExecutionContext context = executionContext();
  if (guard.m_Active ||
      (context != ExecutionContext::WaitableThread && context != ExecutionContext::AtomicThread)) {
    return TlbInvalidationResult::InvalidContext;
  }

  guard.m_Active = true;
  guard.m_Global = false;
  return TlbInvalidationResult::Success;
}

void ProcessorBase::endTlbInvalidation(TlbInvalidationGuard& guard) {
  guard.m_Global = false;
  guard.m_Active = false;
}

bool ProcessorBase::closeTlbInvalidationAdmissionForTerminalFailure(TlbInvalidationGuard& guard,
                                                                    TlbInvalidationResult result) {
  (void)result;
  return guard.m_Active;
}

bool ProcessorBase::tlbInvalidationFailureActive() {
  return false;
}

bool ProcessorBase::tlbInvalidationTerminal() {
  return false;
}

TlbInvalidationResult ProcessorBase::invalidateAll(void* pAddress) {
  TlbInvalidationGuard guard;
  const TlbInvalidationResult result = beginTlbInvalidation(guard);
  if (result != TlbInvalidationResult::Success) {
    return result;
  }
  return invalidateAll(pAddress, guard);
}

TlbInvalidationResult ProcessorBase::invalidateAll(void* pAddress, TlbInvalidationGuard& guard) {
  if (!guard.m_Active) {
    return TlbInvalidationResult::InvalidContext;
  }
  invalidate(pAddress);
  return TlbInvalidationResult::Success;
}

ProcessorId ProcessorBase::id() {
  return 0;
}

size_t ProcessorBase::index() {
  return 0;
}

ProcessorInformation& ProcessorBase::information() {
  return m_SafeBspProcessorInformation;
}

size_t ProcessorBase::getCount() {
  return 1;
}

#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

void ProcessorBase::_breakpoint() {
  sigset_t set;
  sigset_t oset;
  sigemptyset(&set);
  sigemptyset(&oset);
  sigaddset(&set, SIGTRAP);
  pthread_sigmask(SIG_UNBLOCK, &set, &oset);
  raise(SIGTRAP);
  pthread_sigmask(SIG_SETMASK, &oset, 0);
}

void ProcessorBase::_reset() {
  // Just exit.
  exit(0);
}

void ProcessorBase::_haltUntilInterrupt() {
  sigset_t set;
  sigemptyset(&set);
  sigsuspend(&set);
}

void ProcessorBase::breakpoint() {
  Processor::_breakpoint();
}

void ProcessorBase::halt() {
  // Abnormal exit.
  __builtin_trap();
}

void ProcessorBase::pause() {
#if defined(__APPLE__) && defined(__aarch64__)
  asm volatile("yield");
#else
  asm volatile("pause");
#endif
}

void ProcessorBase::reset() {
  Processor::_reset();
}

void ProcessorBase::haltUntilInterrupt() {
  Processor::_haltUntilInterrupt();
}

void ProcessorBase::invalidateICache(uintptr_t nAddr) {
#if defined(__APPLE__) && defined(__aarch64__)
  sys_icache_invalidate(reinterpret_cast<void*>(nAddr), 1);
#else
  __asm__ __volatile__("clflush (%0)" ::"a"(nAddr));
#endif
}

void ProcessorBase::invalidateDCache(uintptr_t nAddr) {
#if defined(__APPLE__) && defined(__aarch64__)
  sys_dcache_flush(reinterpret_cast<void*>(nAddr), 1);
#else
  __asm__ __volatile__("clflush (%0)" ::"a"(nAddr));
#endif
}

void ProcessorBase::flushDCache(uintptr_t nAddr) {
#if defined(__APPLE__) && defined(__aarch64__)
  sys_dcache_flush(reinterpret_cast<void*>(nAddr), 1);
#else
  __asm__ __volatile__("clflush (%0)" ::"a"(nAddr));
#endif
}

void ProcessorBase::flushDCacheAndInvalidateICache(uintptr_t startAddr, uintptr_t endAddr) {
#if defined(__APPLE__) && defined(__aarch64__)
  if (endAddr > startAddr) {
    void* start = reinterpret_cast<void*>(startAddr);
    const size_t size = endAddr - startAddr;
    sys_dcache_flush(start, size);
    sys_icache_invalidate(start, size);
  }
#else
  for (size_t i = 0; i < endAddr; ++i) {
    __asm__ __volatile__("clflush (%0)" ::"a"(startAddr + i));
  }
#endif
}

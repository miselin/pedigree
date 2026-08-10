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
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/NMFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/lib.h"

NMFaultHandler NMFaultHandler::m_Instance;

#define NM_FAULT_EXCEPTION 0x07
/// \todo Move these to some X86 Processor header
#define CR0_NE (1 << 5)
#define CR0_TS (1 << 3)
#define CR0_EM (1 << 2)
#define CR0_MP (1 << 1)
#define CR4_OSFXSR (1 << 9)
#define CR4_OSXMMEXCPT (1 << 10)
#define CPUID_FEAT_EDX_FXSR (1 << 24)
#define CPUID_FEAT_EDX_FPU (1 << 0)
#define CPUID_FEAT_EDX_SSE (1 << 25)

#define MXCSR_PM (1 << 12)
#define MXCSR_UM (1 << 11)
#define MXCSR_OM (1 << 10)
#define MXCSR_ZM (1 << 9)
#define MXCSR_DM (1 << 8)
#define MXCSR_IM (1 << 7)

#define MXCSR_RC 13

#define MXCSR_RC_NEAREST 0
#define MXCSR_RC_DOWN 1
#define MXCSR_RC_UP 2
#define MXCSR_RC_TRUNCATE 3

#define MXCSR_MASK 28

static bool FXSR_Support, FPU_Support;
static X64SchedulerState x87FPU_MMX_XMM_MXCSR_StateBlank;
static bool g_InitialFxStateValid = false;
static Spinlock g_InitialFxStateLock(false, true);

static_assert(__builtin_offsetof(X64SchedulerState, x87FPU_MMX_XMM_MXCSR_State) == 112,
              "x64 Scheduler.s FXSAVE offset is stale");

static void* fpuStateBuffer(X64SchedulerState& state) {
  uintptr_t address = reinterpret_cast<uintptr_t>(state.x87FPU_MMX_XMM_MXCSR_State);
  return reinterpret_cast<void*>((address + 15) & ~uintptr_t(15));
}

static const void* fpuStateBuffer(const X64SchedulerState& state) {
  uintptr_t address = reinterpret_cast<uintptr_t>(state.x87FPU_MMX_XMM_MXCSR_State);
  return reinterpret_cast<const void*>((address + 15) & ~uintptr_t(15));
}

static inline void _SetFPUControlWord(uint16_t cw) {
  // FLDCW = Load FPU Control Word
  asm volatile("  fldcw %0;   " ::"m"(cw));
}

bool NMFaultHandler::initialise() {
  // Register the handler
  if (!initialiseProcessor()) {
    return false;
  }
  InterruptManager::instance().registerInterruptHandler(NM_FAULT_EXCEPTION, this);
  return false;
}

bool NMFaultHandler::initialiseProcessor() {
  // Check for FPU and XSAVE
  uint32_t eax, ebx, ecx, edx, mxcsr = 0;
  uint64_t cr0, cr4;

  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  asm volatile("mov %%cr4, %0" : "=r"(cr4));
  Processor::cpuid(1, 0, eax, ebx, ecx, edx);

  if (edx & CPUID_FEAT_EDX_FPU) {
    FPU_Support = true;

    cr0 = (cr0 | CR0_NE | CR0_MP) & ~(CR0_EM | CR0_TS);
    asm volatile("mov %0, %%cr0" ::"r"(cr0));

    // init the FPU
    asm volatile("finit");

    // set the FPU Control Word
    _SetFPUControlWord(0x37F);

    asm volatile("mov %0, %%cr0" ::"r"(cr0));
  } else
    FPU_Support = false;

  if (edx & CPUID_FEAT_EDX_FXSR) {
    FXSR_Support = true;
    cr4 |= CR4_OSFXSR;  // set the FXSAVE/FXRSTOR support bit
  } else
    FXSR_Support = false;

  if (edx & CPUID_FEAT_EDX_SSE) {
    cr4 |= CR4_OSXMMEXCPT;  // set the SIMD floating-point exception
                            // handling bit
    asm volatile("mov %0, %%cr4;" ::"r"(cr4));

    // Match the AMD64 userspace reset state: round to nearest and mask
    // all exceptions.
    mxcsr = 0x1F80;

    // write the control word
    asm volatile("ldmxcsr %0;" ::"m"(mxcsr));
  } else
    asm volatile("mov %0, %%cr4;" ::"r"(cr4));

  if (FXSR_Support) {
    while (!g_InitialFxStateLock.acquire(false, false))
      ;
    if (!g_InitialFxStateValid) {
      void* blankState = fpuStateBuffer(x87FPU_MMX_XMM_MXCSR_StateBlank);
      asm volatile("fxsave64 (%0)" ::"r"(blankState) : "memory");
      x87FPU_MMX_XMM_MXCSR_StateBlank.flags |= (1 << 1);
      g_InitialFxStateValid = true;
    }
    g_InitialFxStateLock.release();
  }

  // set the bit that causes a DeviceNotAvailable upon SSE, MMX, or FPU
  // instruction execution
  cr0 |= CR0_TS;
  asm volatile("mov %0, %%cr0" ::"r"(cr0));

  return true;
}

bool NMFaultHandler::saveCurrentThreadFpuState(void* buffer, bool resetForSignalHandler) {
  if (!FXSR_Support || !g_InitialFxStateValid || !buffer ||
      (reinterpret_cast<uintptr_t>(buffer) & 0xF)) {
    return false;
  }

  Thread* thread = Processor::information().getCurrentThread();
  if (!thread) {
    return false;
  }

  bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);

  X64SchedulerState& state = thread->state();
  void* threadState = fpuStateBuffer(state);
  const void* blankState = fpuStateBuffer(x87FPU_MMX_XMM_MXCSR_StateBlank);

  uint64_t cr0;
  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  if (cr0 & CR0_TS) {
    if (state.flags & (1 << 1)) {
      MemoryCopy(buffer, threadState, 512);
    } else {
      MemoryCopy(buffer, blankState, 512);
    }
  } else {
    asm volatile("fxsave64 (%0)" ::"r"(buffer) : "memory");
    MemoryCopy(threadState, buffer, 512);
    state.flags |= (1 << 1);
  }

  if (resetForSignalHandler) {
    MemoryCopy(threadState, blankState, 512);
    state.flags |= (1 << 1);
    if (!(cr0 & CR0_TS)) {
      asm volatile("fxrstor64 (%0)" ::"r"(blankState) : "memory");
    }
  }

  Processor::setInterrupts(interrupts);
  return true;
}

bool NMFaultHandler::restoreCurrentThreadFpuState(const void* buffer) {
  if (!FXSR_Support || !g_InitialFxStateValid || !buffer ||
      (reinterpret_cast<uintptr_t>(buffer) & 0xF)) {
    return false;
  }

  uint32_t mxcsr = 0;
  uint32_t mxcsrMask = 0;
  MemoryCopy(&mxcsr, adjust_pointer(buffer, 24), sizeof(mxcsr));
  MemoryCopy(&mxcsrMask, adjust_pointer(fpuStateBuffer(x87FPU_MMX_XMM_MXCSR_StateBlank), 28),
             sizeof(mxcsrMask));
  if (!mxcsrMask) {
    mxcsrMask = 0xFFBF;
  }
  if (mxcsr & ~mxcsrMask) {
    return false;
  }

  Thread* thread = Processor::information().getCurrentThread();
  if (!thread) {
    return false;
  }

  bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);

  X64SchedulerState& state = thread->state();
  MemoryCopy(fpuStateBuffer(state), buffer, 512);
  state.flags |= (1 << 1);

  uint64_t cr0 = 0;
  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  if (!(cr0 & CR0_TS)) {
    asm volatile("fxrstor64 (%0)" ::"r"(buffer) : "memory");
  }

  Processor::setInterrupts(interrupts);
  return true;
}

bool NMFaultHandler::inheritCurrentThreadFpuState(Thread* thread) {
  if (!thread) {
    return false;
  }

  alignas(16) uint8_t inherited[512];
  if (!saveCurrentThreadFpuState(inherited, false)) {
    return false;
  }

  X64SchedulerState& state = thread->state();
  MemoryCopy(fpuStateBuffer(state), inherited, sizeof(inherited));
  state.flags |= (1 << 1);
  return true;
}

void NMFaultHandler::interrupt(size_t interruptNumber, InterruptState& state) {
  // Check the TS bit
  uint64_t cr0;

  Thread* pCurrentThread = Processor::information().getCurrentThread();
  if (!pCurrentThread) {
    FATAL_NOLOCK("NM: no current thread");
  }
  X64SchedulerState* pCurrentState = &pCurrentThread->state();

  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  if (cr0 & CR0_TS) {
    cr0 &= ~CR0_TS;
    asm volatile("mov %0, %%cr0" ::"r"(cr0));
  } else {
    FATAL_NOLOCK("NM: TS already disabled");
  }

  // bochs breakpoint
  // asm volatile("xchg %bx, %bx;");

  // if this task has never used SSE before, we need to init the state space
  if (FXSR_Support) {
    if (!(pCurrentState->flags & (1 << 1))) {
      MemoryCopy(fpuStateBuffer(*pCurrentState), fpuStateBuffer(x87FPU_MMX_XMM_MXCSR_StateBlank),
                 512);
      pCurrentState->flags |= (1 << 1);
    }

    asm volatile("fxrstor64 (%0)" ::"r"(fpuStateBuffer(*pCurrentState)) : "memory");
  } else if (FPU_Support) {
    if (!(pCurrentState->flags & (1 << 1))) {
      MemoryCopy(fpuStateBuffer(*pCurrentState), fpuStateBuffer(x87FPU_MMX_XMM_MXCSR_StateBlank),
                 108);

      pCurrentState->flags |= (1 << 1);
    }

    asm volatile("frstor (%0)" ::"r"(fpuStateBuffer(*pCurrentState)) : "memory");
  } else {
    ERROR("FXSAVE and FSAVE are not supported");
  }
}

NMFaultHandler::NMFaultHandler() {}

void NMFaultHandler::threadTerminated(Thread* pThread) {
  // FPU images are saved into scheduler state at every context switch, so
  // the fault handler keeps no pointers into thread-owned storage.
  (void)pThread;
}

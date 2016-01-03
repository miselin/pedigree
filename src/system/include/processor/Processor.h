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

#ifndef KERNEL_PROCESSOR_PROCESSOR_H
#define KERNEL_PROCESSOR_PROCESSOR_H

#include <compiler.h>
#include <utilities/StaticString.h>
#include <processor/types.h>
#include <processor/state.h>
#include <processor/ProcessorInformation.h>

/** @addtogroup kernelprocessor
 * @{ */

struct BootstrapStruct_t;

namespace DebugFlags
{
  enum FaultType
  {
    InstructionFetch = 0,
    DataWrite        = 1,
    IOReadWrite      = 2,
    DataReadWrite    = 3
  };
}

/// Defines for debug status flags.
#define DEBUG_BREAKPOINT_0 0x01   /// Breakpoint 0 was triggered.
#define DEBUG_BREAKPOINT_1 0x02   /// Breakpoint 1 was triggered.
#define DEBUG_BREAKPOINT_2 0x04   /// Breakpoint 2 was triggered.
#define DEBUG_BREAKPOINT_3 0x08   /// Breakpoint 3 was triggered.
#define DEBUG_REG_ACCESS   0x2000 /// The next instruction in the stream accesses a debug register, and GD is turned on.
#define DEBUG_SINGLE_STEP  0x4000 /// The exception was caused by single-step execution mode (TF enabled in EFLAGS).
#define DEBUG_TASK_SWITCH  0x8000 /// The exception was caused by a hardware task switch.

/** Interface to the processor's capabilities
 *\note static in member function declarations denotes that these functions return/process
 *      data on the processor that is executing this code. */
class Processor
{
  friend class Multiprocessor;
  friend class X86GdtManager;
#ifdef THREADS
  friend class Scheduler;
#endif
  public:
    /** Initialises the processor specific interface. After this function call the whole
     *  processor-specific interface is initialised. Note though, that only the
     *  bootstrap processor is started. Multiprocessor/-core facilities are available after
     *  initialiseProcessor2().
     *\brief first stage in the initialisation of the processor-specific interface
     *\note This function should only be called once and by main()
     *\param[in] Info reference to the multiboot information structure */
    static void initialise1(const BootstrapStruct_t &Info) INITIALISATION_ONLY;
    /** Initialises the Multiprocessor/-core functionality of the processor-specific
     *  interface. This function may only be called after initialiseProcessor1 and
     *  after the whole machine specific interface has been initialised.
     *\brief second/last stage in the initialisation of the processor-specific interface
     *\note This function should only be called once and by main() */
    static void initialise2(const BootstrapStruct_t &Info) INITIALISATION_ONLY;
    /** End of the kernel core initialisation reached, the initialisation functions
     *  and data may now get unmapped/freed. */
    static void initialisationDone();
    /** Is the processor-specific interface initialised?
     *\return 0, if nothing has been initialised, 1, if initialise1() has been executed
     *        successfully, 2, if initialise2() has been executed successfully */
    inline static size_t isInitialised(){return m_Initialised;}

    /** Get the base-pointer of the calling function
     *\return base-pointer of the calling function */
    static uintptr_t getBasePointer();
    /** Get the stack-pointer of the calling function
     *\return stack-pointer of the calling function */
    static uintptr_t getStackPointer();
    /** Get the instruction-pointer of the calling function
     *\return instruction-pointer of the calling function */
    static uintptr_t getInstructionPointer();

    /** Switch to a different virtual address space
     *\param[in] AddressSpace the new address space */
    static void switchAddressSpace(VirtualAddressSpace &AddressSpace);

    /** Save the current processor state.
        \param[out] state SchedulerState to save into.
        \return False if the save saved the state. True if a restoreState occurs. */
    static bool saveState(SchedulerState &state)
#ifdef SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH
    DEPRECATED
#endif
    ;
    /** Restore a previous scheduler state.
        \param[in] state The state to restore.
        \param[out] pLock Optional lock to release. */
    static void restoreState(SchedulerState &state, volatile uintptr_t *pLock=0) NORETURN;
    /** Restore a previous syscall state.
        \param[out] pLock Optional lock to release (for none, set as 0)
        \param[in]  state Syscall state to restore. */
    static void restoreState(SyscallState &state, volatile uintptr_t *pLock=0) NORETURN;
#ifdef SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH
    /** Switch between two states, safely. */
    static void switchState(bool bInterrupts, SchedulerState &a, SchedulerState &b, volatile uintptr_t *pLock=0);
    /** Switch between two states, safely. */
    static void switchState(bool bInterrupts, SchedulerState &a, SyscallState &b, volatile uintptr_t *pLock=0);
    /** Jumps to an address, in kernel mode, and sets up a calling frame with
        the given parameters. Saves the current state before doing so.
        \param bInterrupts Interrupt state to restore in saved context.
        \param pLock       Optional lock to release.
        \param address     Address to jump to.
        \param stack       Stack to use (set to 0 for current stack).
        \param param1      First parameter.
        \param param2      Second parameter.
        \param param3      Third parameter.
        \param param4      Fourth parameter. */
    static void saveAndJumpKernel(bool bInterrupts, SchedulerState &s, volatile uintptr_t *pLock,
                                  uintptr_t address, uintptr_t stack, uintptr_t p1=0,
                                  uintptr_t p2=0, uintptr_t p3=0, uintptr_t p4=0);
    /** Jumps to an address, in user mode, and sets up a calling frame with
        the given parameters. Saves the current state before doing so.
        \param bInterrupts Interrupt state to restore in saved context.
        \param pLock       Optional lock to release.
        \param address     Address to jump to.
        \param stack       Stack to use (set to 0 for current stack).
        \param param1      First parameter.
        \param param2      Second parameter.
        \param param3      Third parameter.
        \param param4      Fourth parameter. */
    static void saveAndJumpUser(bool bInterrupts, SchedulerState &s, volatile uintptr_t *pLock,
                                  uintptr_t address, uintptr_t stack, uintptr_t p1=0,
                                  uintptr_t p2=0, uintptr_t p3=0, uintptr_t p4=0);
#endif // SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH
    /** Jumps to an address, in kernel mode, and sets up a calling frame with
        the given parameters.
        \param pLock   Optional lock to release.
        \param address Address to jump to.
        \param stack   Stack to use (set to 0 for current stack).
        \param param1  First parameter.
        \param param2  Second parameter.
        \param param3  Third parameter.
        \param param4  Fourth parameter. */
    static void jumpKernel(volatile uintptr_t *pLock, uintptr_t address, uintptr_t stack,
                           uintptr_t p1=0, uintptr_t p2=0, uintptr_t p3=0, uintptr_t p4=0) NORETURN;
    /** Jumps to an address, in user mode, and sets up a calling frame with
        the given parameters.
        \param pLock   Optional lock to release.
        \param address Address to jump to.
        \param stack   Stack to use (set to 0 for current stack).
        \param param1  First parameter.
        \param param2  Second parameter.
        \param param3  Third parameter.
        \param param4  Fourth parameter. */
    static void jumpUser(volatile uintptr_t *pLock, uintptr_t address, uintptr_t stack,
                           uintptr_t p1=0, uintptr_t p2=0, uintptr_t p3=0, uintptr_t p4=0) NORETURN;

    /** Trigger a breakpoint */
    inline static void breakpoint() ALWAYS_INLINE;
    /** Halt this processor */
    inline static void halt() ALWAYS_INLINE;
    /** Reset this processor */
    inline static void reset() ALWAYS_INLINE;

    /** Return the (total) number of breakpoints
     *\return (total) number of breakpoints */
    static size_t getDebugBreakpointCount();
    /** Get information for a specific breakpoint
     *\param[in] nBpNumber the breakpoint number [0 - (getDebugBreakpointCount() - 1)]
     *\param[in,out] nFaultType the breakpoint type
     *\param[in,out] nLength the breakpoint size/length
     *\param[in,out] bEnabled is the breakpoint enabled? */
    static uintptr_t getDebugBreakpoint(size_t nBpNumber,
                                        DebugFlags::FaultType &nFaultType,
                                        size_t &nLength,
                                        bool &bEnabled);
    /** Enable a specific breakpoint
     *\param[in] nBpNumber the breakpoint number [0 - (getDebugBreakpointCount() - 1)]
     *\param[in] nLinearAddress the linear Adress that should trigger a breakpoint exception
     *\param[in] nFaultType the type of access that should trigger a breakpoint exception
     *\param[in] nLength the size/length of the breakpoint */
    static void enableDebugBreakpoint(size_t nBpNumber,
                                      uintptr_t nLinearAddress,
                                      DebugFlags::FaultType nFaultType,
                                      size_t nLength);
    /** Disable a specific breakpoint
     *\param[in] nBpNumber the breakpoint number [0 - (getDebugBreakpointCount() - 1)] */
    static void disableDebugBreakpoint(size_t nBpNumber);
    /** Get the debug status
     *\todo is the debug status somehow abtractable?
     *\return the debug status */
    static uintptr_t getDebugStatus();

    /** Wait for an IRQ to fire. Possible HALT or low-power state. */
    static inline void haltUntilInterrupt();
    /** Pause CPU during a tight polling loop. */
    static inline void pause();

    /** Enable/Disable IRQs
     *\param[in] bEnable true to enable IRSs, false otherwise */
    static void setInterrupts(bool bEnable);
    /** Get the IRQ state
     *\return true, if interrupt requests are enabled, false otherwise */
    static bool getInterrupts();
    /** Enable/Disable single-stepping
     *\param[in] bEnable true to enable single-stepping, false otherwise
     *\param[in] state the interrupt-state */
    static void setSingleStep(bool bEnable, InterruptState &state);

    #if defined(X86_COMMON)
      /** Read a Machine/Model-specific register
       *\param[in] index the register index
       *\return the value of the register */
      static uint64_t readMachineSpecificRegister(uint32_t index);
      /** Write a Machine/Model-specific register
       *\param[in] index the register index
       *\param[in] value the new value of the register */
      static void writeMachineSpecificRegister(uint32_t index, uint64_t value);
      /** Executes the CPUID machine instruction
       *\param[in] inEax eax before the CPUID instruction
       *\param[in] inEcx ecx before the CPUID instruction
       *\param[out] eax eax afterwards
       *\param[out] ebx ebx afterwards
       *\param[out] ecx ecx afterwards
       *\param[out] edx edx afterwards */
      static void cpuid(uint32_t inEax,
                        uint32_t inEcx,
                        uint32_t &eax,
                        uint32_t &ebx,
                        uint32_t &ecx,
                        uint32_t &edx);
    #endif
    /** Invalidate the TLB entry containing a specific virtual address
     *\param[in] pAddress the specific virtual address
     *\todo Figure out if we want to flush the TLB of every processor or if
     *      this should be handled by the upper layers */
    #ifdef PPC_COMMON
      inline
    #endif
    static void invalidate(void *pAddress);

    #if defined(X86_COMMON)
      static physical_uintptr_t readCr3();
    #endif

    #if defined(ARMV7)
      /** Read TTBR0 */
      static physical_uintptr_t readTTBR0();
      /** Read TTBR1 */
      static physical_uintptr_t readTTBR1();
      /** Read TTBCR */
      static uint32_t readTTBCR();
      /** Write TTBR0 */
      static void writeTTBR0(physical_uintptr_t value);
      /** Write TTBR1 */
      static void writeTTBR1(physical_uintptr_t value);
      /** Write TTBCR */
      static void writeTTBCR(uint32_t value);
    #endif

    #if defined(MIPS_COMMON) || defined(PPC_COMMON)
      /** Invalidate a line in the instruction cache.
       *\param[in] nAddr The address of a memory location to invalidate from the Icache. */
      static void invalidateICache(uintptr_t nAddr);
      /** Invalidate a line in the data cache.
       *\param[in] nAddr The address of a memory location to invalidate from the Dcache. */
      static void invalidateDCache(uintptr_t nAddr);
      /** Flush a line in the data cache.
       *\param[in] nAddr The address of a memory location to flush from the Dcache. */
      static void flushDCache(uintptr_t nAddr);
      /** Flush from the data cache and invalidate into the instruction cache.
       * This must be used whenever code is written to memory and needs to be executed.
       *\param startAddr The first address to be flushed and invalidated.
       *\param endAddr The last address to be flushed and invalidated. */
      static void flushDCacheAndInvalidateICache(uintptr_t startAddr, uintptr_t endAddr);
    #endif

    /** Populate 'str' with a string describing the characteristics of this processor. */
    static void identify(HugeStaticString &str);

    /** Get the ProcessorId of this processor
     *\return the ProcessorId of this processor */
    #if !defined(MULTIPROCESSOR)
      inline static ProcessorId id();
    #else
      static ProcessorId id();
    #endif
    /** Get the ProcessorInformation structure of this processor
     *\return the ProcessorInformation structure of this processor */
    #if !defined(MULTIPROCESSOR)
      static inline ProcessorInformation &information();
    #else
      static ProcessorInformation &information();
    #endif

    #ifdef PPC_COMMON
      inline static void setSegmentRegisters(uint32_t segmentBase, bool supervisorKey, bool
 userKey);
    #endif
    
    /** Set a new TLS area base address. */
    static void setTlsBase(uintptr_t newBase);

    /** How far has the processor-specific interface been initialised */
    static size_t m_Initialised;
  private:
    #if defined(HOSTED)
      /** Implementation of breakpoint(), reset(), haltUntilInterrupt() */
      static void _breakpoint();
      static void _reset() NORETURN;
      static void _haltUntilInterrupt();

      static bool m_bInterrupts;
    #endif

    /** If we have only one processor, we define the ProcessorInformation class here
     *  otherwise we use an array of ProcessorInformation structures */
    #if !defined(MULTIPROCESSOR)
      static ProcessorInformation m_ProcessorInformation;
    #else
      static Vector<ProcessorInformation*> m_ProcessorInformation;
      
      /// Used before multiprocessor stuff is turned on as a "safe" info structure.
      /// For stuff like early heap setup and all that.
      static ProcessorInformation m_SafeBspProcessorInformation;
    #endif
    
    static size_t m_nProcessors;
};

/** @} */

#if defined(X86_COMMON)
  #include <processor/x86_common/Processor.h>
#elif defined(MIPS_COMMON)
  #include <processor/mips_common/Processor.h>
#elif defined(ARM_COMMON)
  #include <processor/arm_common/Processor.h>
#elif defined(PPC_COMMON)
  #include <processor/ppc_common/Processor.h>
#elif defined(HOSTED)
  #include <processor/hosted/Processor.h>
#endif

#ifdef X86
  #include <processor/x86/Processor.h>
#endif
#ifdef X64
  #include <processor/x64/Processor.h>
#endif

//
// Part of the implementation
//

#if !defined(MULTIPROCESSOR)
  ProcessorId Processor::id()
  {
    return 0;
  }
  ProcessorInformation &Processor::information()
  {
    return m_ProcessorInformation;
  }
#endif

/**
 * EnsureInterrupts ensures interrupts are enabled or disabled in an RAII way.
 * After the block completes, the interrupts enable state is restored.
 */
class EnsureInterrupts
{
public:
    EnsureInterrupts(bool desired);
    virtual ~EnsureInterrupts();

private:
    bool m_bPrevious;
};

#endif

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

#ifndef KERNEL_PROCESSOR_ARMV7_STATE_H
#define KERNEL_PROCESSOR_ARMV7_STATE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessorARMV7
 * @{ */

/** ARMV7 Interrupt State */
class EXPORTED_PUBLIC ARMV7InterruptState
{
    friend class ARMV7ProcessorState;
    friend class ARMV7SyscallState;
    friend class PageFaultHandler;

  public:
    //
    // General Interface (both InterruptState and SyscallState)
    //
    /** Get the stack-pointer before the interrupt occured
     *\return the stack-pointer before the interrupt */
    uintptr_t getStackPointer() const;
    /** Set the userspace stack-pointer
     *\param[in] stackPointer the new stack-pointer */
    void setStackPointer(uintptr_t stackPointer);
    /** Get the instruction-pointer of the next instruction that is executed
     * after the interrupt is processed
     *\return the instruction-pointer */
    uintptr_t getInstructionPointer() const;
    /** Set the instruction-pointer
     *\param[in] instructionPointer the new instruction-pointer */
    void setInstructionPointer(uintptr_t instructionPointer);
    /** Get the base-pointer
     *\return the base-pointer */
    uintptr_t getBasePointer() const;
    /** Set the base-pointer
     *\param[in] basePointer the new base-pointer */
    void setBasePointer(uintptr_t basePointer);
    /** Get the number of registers
     *\return the number of registers */
    size_t getRegisterCount() const;
    /** Get a specific register
     *\param[in] index the index of the register (from 0 to getRegisterCount() -
     *1) \return the value of the register */
    processor_register_t getRegister(size_t index) const;
    /** Get the name of a specific register
     *\param[in] index the index of the register (from 0 to getRegisterCount() -
     *1) \return the name of the register */
    const char *getRegisterName(size_t index) const;
    /** Get the register's size in bytes
     *\param[in] index the index of the register (from 0 to getRegisterCount() -
     *1) \return the register size in bytes */
    size_t getRegisterSize(size_t index) const;

    //
    // InterruptState Interface
    //
    /** Did the interrupt happen in kernel-mode?
     *\return true, if the interrupt happened in kernel-mode, false otherwise */
    bool kernelMode() const;
    /** Get the interrupt number
     *\return the interrupt number */
    size_t getInterruptNumber() const;

    //
    // SyscallState Interface
    //
    /** Get the syscall service number
     *\return the syscall service number */
    size_t getSyscallService() const;
    /** Get the syscall function number
     *\return the syscall function number */
    size_t getSyscallNumber() const;
    /** Get the n'th parameter for this syscall. */
    uintptr_t getSyscallParameter(size_t n) const;
    void setSyscallReturnValue(uintptr_t val);

  private:
    /** The default constructor
     *\note NOT implemented */
    ARMV7InterruptState();

    /** The copy-constructor
     *\note NOT implemented */
    ARMV7InterruptState(const ARMV7InterruptState &);
    /** The assignement operator
     *\note NOT implemented */
    ARMV7InterruptState &operator=(const ARMV7InterruptState &);
    /** The destructor
     *\note NOT implemented */
    ~ARMV7InterruptState() = default;

    /** ARMV7 interrupt frame **/
    uint32_t m_usersp;
    uint32_t m_userlr;
    uint32_t m_r0;
    uint32_t m_r1;
    uint32_t m_r2;
    uint32_t m_r3;
    uint32_t m_r4;
    uint32_t m_r5;
    uint32_t m_r6;
    uint32_t m_r7;
    uint32_t m_r8;
    uint32_t m_r9;
    uint32_t m_r10;
    uint32_t m_r11;
    uint32_t m_r12;
    uint32_t m_lr;
    uint32_t m_pc;
    uint32_t m_spsr;
} PACKED;

class EXPORTED_PUBLIC ARMV7SyscallState : public ARMV7InterruptState
{
public:
  ARMV7SyscallState() = default;
  ~ARMV7SyscallState() = default;
  ARMV7SyscallState(const ARMV7InterruptState &state);
};

class EXPORTED_PUBLIC ARMV7ProcessorState : public ARMV7InterruptState
{
public:
  ARMV7ProcessorState() = default;
  ~ARMV7ProcessorState() = default;
  ARMV7ProcessorState(const ARMV7InterruptState &state);
};

class __attribute__((aligned(8))) ARMV7SchedulerState
{
  public:
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t r12;
    uint32_t sp;
    uint32_t lr;
} __attribute__((aligned(8)));

/** @} */

#endif

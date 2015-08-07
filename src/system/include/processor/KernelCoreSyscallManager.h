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

#ifndef KERNEL_CORE_SYSCALL_MANAGER_H
#define KERNEL_CORE_SYSCALL_MANAGER_H

#include <processor/types.h>
#include <processor/SyscallHandler.h>

class Process;

class KernelCoreSyscallManager : public SyscallHandler
{
public:
  /** Get the syscall manager instance.
   *\return instance of the syscall manager */
  static KernelCoreSyscallManager &instance() {return m_Instance;}

  enum Function_t
  {
    /** Yields the processor to another thread. */
    yield = 0,

    /** Dynamic linking request.
     *  \note If you change this, update
     *        system/modules/linker/asm-*.s ! */
    link = 1,

    sleep = 2,

    /** The last function, for error checking. */
    functionEnd
  };

  void initialise();

  /** Calls a syscall. */
  uintptr_t call(Function_t function, uintptr_t p1=0, uintptr_t p2=0, uintptr_t p3=0, uintptr_t p4=0, uintptr_t p5=0);

  /** Called when a syscall arrives. */
  virtual uintptr_t syscall(SyscallState &state);

  typedef uintptr_t (*SyscallCallback)(SyscallState &);

  /** Register a syscall with a callback. */
  uintptr_t registerSyscall(Function_t function, SyscallCallback func);

protected:
  /** The constructor */
  inline KernelCoreSyscallManager();
  /** The destructor */
  inline virtual ~KernelCoreSyscallManager();

private:
  /** The copy-constructor
   *\note Not implemented (singleton) */
  KernelCoreSyscallManager(const KernelCoreSyscallManager &);
  /** The copy-constructor
   *\note Not implemented (singleton) */
  KernelCoreSyscallManager &operator = (const KernelCoreSyscallManager &);

  /** The static singleton instance. */
  static KernelCoreSyscallManager m_Instance;
  
  /** Syscall lookup table for registerable syscalls. */
  SyscallCallback m_Functions[16];
};

#endif



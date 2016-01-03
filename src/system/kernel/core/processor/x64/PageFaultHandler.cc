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

#include <Log.h>
#include <Debugger.h>
#include <processor/PageFaultHandler.h>
#include <process/Scheduler.h>
#include <process/Process.h>
#include <panic.h>
#include <processor/PhysicalMemoryManager.h>
#include "VirtualAddressSpace.h"

PageFaultHandler PageFaultHandler::m_Instance;

#define PAGE_FAULT_EXCEPTION  0x0E
#define PFE_PAGE_PRESENT      0x01
#define PFE_ATTEMPTED_WRITE   0x02
#define PFE_USER_MODE         0x04
#define PFE_RESERVED_BIT      0x08
#define PFE_INSTRUCTION_FETCH 0x10

bool PageFaultHandler::initialise()
{
  InterruptManager &IntManager = InterruptManager::instance();

  return(IntManager.registerInterruptHandler(PAGE_FAULT_EXCEPTION, this));
}

void PageFaultHandler::interrupt(size_t interruptNumber, InterruptState &state)
{
  uintptr_t cr2, code;
  asm volatile("mov %%cr2, %%rax" : "=a" (cr2));
  code = state.m_Errorcode;

  uintptr_t page = cr2 & ~(PhysicalMemoryManager::instance().getPageSize()-1);

  // Check for copy-on-write.
  VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
  if (va.isMapped(reinterpret_cast<void*>(page)))
  {
    physical_uintptr_t phys;
    size_t flags;
    va.getMapping(reinterpret_cast<void*>(page), phys, flags);
    if (flags & VirtualAddressSpace::CopyOnWrite)
    {
#ifdef SUPERDEBUG
      NOTICE_NOLOCK(Processor::information().getCurrentThread()->getParent()->getId() << " PageFaultHandler: copy-on-write for v=" << page);
#endif

      Process *pProcess = Processor::information().getCurrentThread()->getParent();
      size_t pageSz = PhysicalMemoryManager::instance().getPageSize();

      // Get a temporary page in which we can store the current mapping for copy.
      uintptr_t tempAddr = 0;
      pProcess->getSpaceAllocator().allocate(pageSz, tempAddr);

      // Map temporary page to the old page.
      if (!va.map(phys, reinterpret_cast<void *>(tempAddr), VirtualAddressSpace::KernelMode))
      {
        FATAL("PageFaultHandler: CoW temporary map() failed");
        return;
      }

      // OK, we can now unmap the old page - we hold a valid temporary mapping.
      va.unmap(reinterpret_cast<void*>(page));

      // Allocate new page for the new memory region.
      physical_uintptr_t p = PhysicalMemoryManager::instance().allocatePage();
      if (!p)
      {
        FATAL("PageFaultHandler: CoW OOM'd!");
        return;
      }

      // Map in the new page, making sure to mark it not CoW.
      flags |= VirtualAddressSpace::Write;
      flags &= ~VirtualAddressSpace::CopyOnWrite;
      if (!va.map(p, reinterpret_cast<void*>(page), flags))
      {
        FATAL("PageFaultHandler: CoW new map() failed.");
        return;
      }

      // Perform the actual copy.
      memcpy(reinterpret_cast<uint8_t*>(page),
             reinterpret_cast<uint8_t*>(tempAddr),
             pageSz);

      // Release temporary page.
      va.unmap(reinterpret_cast<void *>(tempAddr));
      pProcess->getSpaceAllocator().free(tempAddr, pageSz);

      // Clean up old reference to memory (may free the page, if we were the
      // last one to reference the CoW page)
      PhysicalMemoryManager::instance().freePage(phys);
      return;
    }
  }

  if (cr2 < reinterpret_cast<uintptr_t>(KERNEL_SPACE_START))
  {
      // Check our handler list.
      for (List<MemoryTrapHandler*>::Iterator it = m_Handlers.begin();
           it != m_Handlers.end();
           it++)
      {
          if ((*it)->trap(cr2, code & PFE_ATTEMPTED_WRITE))
          {
              return;
          }
      }
  }

  //  Get PFE location and error code
  static LargeStaticString sError;
  sError.clear();
  sError.append("Page Fault Exception at 0x");
  sError.append(cr2, 16, 8, '0');
  sError.append(", error code 0x");
  sError.append(code, 16, 8, '0');
  sError.append(", EIP 0x");
  sError.append(state.getInstructionPointer(), 16, 8, '0');

  //  Extract error code information
  static LargeStaticString sCode;
  sCode.clear();
  sCode.append("Details: PID=");
  sCode.append(Processor::information().getCurrentThread()->getParent()->getId());
  sCode.append(" ");

  if(!(code & PFE_PAGE_PRESENT)) sCode.append("NOT ");
  sCode.append("PRESENT | ");

  if(code & PFE_ATTEMPTED_WRITE)
    sCode.append("WRITE | ");
  else
    sCode.append("READ | ");

  if(code & PFE_USER_MODE) sCode.append("USER "); else sCode.append("KERNEL ");
  sCode.append("MODE | ");

  if(code & PFE_RESERVED_BIT) sCode.append("RESERVED BIT SET | ");
  if(code & PFE_INSTRUCTION_FETCH) sCode.append("FETCH |");

  // Ensure the log spinlock isn't going to die on us...
//  Log::instance().m_Lock.release();

  ERROR(static_cast<const char*>(sError));
  ERROR(static_cast<const char*>(sCode));

  //static LargeStaticString eCode;
  #ifdef DEBUGGER
    // Page faults in usermode are usually useless to debug in the debugger (some exceptions exist)
    if(!(code & PFE_USER_MODE))
        Debugger::instance().start(state, sError);
  #endif

  Scheduler &scheduler = Scheduler::instance();
  if(UNLIKELY(scheduler.getNumProcesses() == 0))
  {
    //  We are in the early stages of the boot process (no processes started)
    panic(sError);
  }
  else
  {
    //  Unrecoverable PFE in a process - Kill the process and yield
    //Processor::information().getCurrentThread()->getParent()->kill();
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    Subsystem *pSubsystem = pProcess->getSubsystem();
    if(pSubsystem && !state.kernelMode())
        pSubsystem->threadException(pThread, Subsystem::PageFault);
    else
    {
        pProcess->kill();

        //  kill member function also calls yield(), so shouldn't get here.
        for(;;) ;
    }
  }
}

PageFaultHandler::PageFaultHandler() :
    m_Handlers()
{
}

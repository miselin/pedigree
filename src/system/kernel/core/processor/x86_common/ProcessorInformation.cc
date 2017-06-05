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

#include "pedigree/kernel/processor/x86_common/ProcessorInformation.h"

#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#if defined(X86)
#include "pedigree/kernel/processor/x86/tss.h"
#else
#include "pedigree/kernel/processor/x64/tss.h"
#endif
#include "pedigree/kernel/process/InfoBlock.h"

/** Get the current processor's VirtualAddressSpace
 *\return reference to the current processor's VirtualAddressSpace */
VirtualAddressSpace &X86CommonProcessorInformation::getVirtualAddressSpace()
    const {
  if (m_VirtualAddressSpace)
    return *m_VirtualAddressSpace;
  else
    return VirtualAddressSpace::getKernelAddressSpace();
}
/** Set the current processor's VirtualAddressSpace
 *\param[in] virtualAddressSpace reference to the new VirtualAddressSpace */
void X86CommonProcessorInformation::setVirtualAddressSpace(
    VirtualAddressSpace &virtualAddressSpace) {
  m_VirtualAddressSpace = &virtualAddressSpace;
}

/** Set the processor's TSS selector
 *\param[in] TssSelector the new TSS selector */
void X86CommonProcessorInformation::setTssSelector(uint16_t TssSelector) {
  m_TssSelector = TssSelector;
}
/** Set the processor's TSS
 *\param[in] Tss pointer to the new TSS */
void X86CommonProcessorInformation::setTss(void *Tss) {
  m_Tss = reinterpret_cast<TaskStateSegment *>(Tss);
}
/** Get the processor's TSS selector
 *\return the TSS selector of the processor */
uint16_t X86CommonProcessorInformation::getTssSelector() const {
  return m_TssSelector;
}
/** Get the processor's TSS
 *\return the Tss of the processor */
void *X86CommonProcessorInformation::getTss() const {
  return reinterpret_cast<void *>(m_Tss);
}
/** Gets the processor's TLS base segment */
uint16_t X86CommonProcessorInformation::getTlsSelector() { return m_TlsSelector; }
/** Sets the processor's TLS base segment */
void X86CommonProcessorInformation::setTlsSelector(uint16_t tls) {
  m_TlsSelector = tls;
}

uintptr_t X86CommonProcessorInformation::getKernelStack() const {
#if defined(X86)
  return m_Tss->esp0;
#else
  return m_Tss->rsp0;
#endif
}
void X86CommonProcessorInformation::setKernelStack(uintptr_t stack) {
#if defined(X86)
  m_Tss->esp0 = stack;
#else
  m_Tss->rsp0 = stack;
  // Can't use Procesor::writeMachineSpecificRegister as Processor is
  // undeclared here!
  uint32_t eax = stack, edx = stack >> 32;
  asm volatile("wrmsr" ::"a"(eax), "d"(edx), "c"(0xc0000102));
#endif
}

Thread *X86CommonProcessorInformation::getCurrentThread() const { return m_pCurrentThread; }

void X86CommonProcessorInformation::setCurrentThread(Thread *pThread) {
  m_pCurrentThread = pThread;
  InfoBlockManager::instance().setPid(pThread->getParent()->getId());
}

PerProcessorScheduler &X86CommonProcessorInformation::getScheduler() { return *m_Scheduler; }

X86CommonProcessorInformation::X86CommonProcessorInformation(ProcessorId processorId, uint8_t apicId)
    : m_ProcessorId(processorId),
      m_TssSelector(0),
      m_Tss(0),
      m_VirtualAddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_LocalApicId(apicId),
      m_pCurrentThread(0),
      m_Scheduler(0),
      m_TlsSelector(0) {
  m_Scheduler = new PerProcessorScheduler();
}
/** The destructor does nothing */
X86CommonProcessorInformation::~X86CommonProcessorInformation() { delete m_Scheduler; }

void X86CommonProcessorInformation::setIds(ProcessorId processorId, uint8_t apicId) {
  m_ProcessorId = processorId;
  m_LocalApicId = apicId;
}

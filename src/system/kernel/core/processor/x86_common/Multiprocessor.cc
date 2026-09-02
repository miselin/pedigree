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

#include <config.h>

#if MULTIPROCESSOR

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "../x64/VirtualAddressSpace.h"
#include "Multiprocessor.h"
#include <machine/mach_pc/Acpi.h>
#include <machine/mach_pc/LocalApic.h>
#include <machine/mach_pc/Pc.h>
#include <machine/mach_pc/Rtc.h>
#include <machine/mach_pc/Smp.h>

Atomic<bool> Multiprocessor::m_ProcessorStarted(false);
// Don't track this lock - it is for startup synchronisation, not for protecting
// a specific resource.
Spinlock Multiprocessor::m_ProcessorLock2(true, true);

namespace {
constexpr uint64_t InitToStartupDelayMicroseconds = 10000;
constexpr uint64_t StartupIpiDelayMicroseconds = 200;
constexpr uint64_t NanosecondsPerMicrosecond = 1000;
constexpr uint64_t ApplicationProcessorTimeoutNanoseconds = 1000000000;
constexpr size_t EarlyBootPollLimit = 10000000;
constexpr size_t ApplicationProcessorPollLimit = 100000000;

bool earlyBootDelay(uint64_t microseconds) {
  // Time::delay needs the scheduler. Pc::initialise has already calibrated
  // this TSC-backed clock before Processor::initialise2 reaches us.
  if (microseconds > (~static_cast<uint64_t>(0) / NanosecondsPerMicrosecond)) {
    return false;
  }
  const uint64_t duration = microseconds * NanosecondsPerMicrosecond;
  const uint64_t start = Rtc::instance().getTickCountNano();
  for (size_t poll = 0; poll < EarlyBootPollLimit; ++poll) {
    if ((Rtc::instance().getTickCountNano() - start) >= duration)
      return true;
    Processor::pause();
  }
  return (Rtc::instance().getTickCountNano() - start) >= duration;
}

}  // namespace

extern "C" void mp_trampoline16(void);
extern "C" void mp_trampoline32(void);
extern "C" void* trampolinegdt;
extern "C" void* trampolinegdtr;
extern "C" void* trampolinegdt64;
extern "C" void* trampolinegdtr64;

size_t Multiprocessor::initialise1() {
  if (!Pc::instance().localApicAvailable()) {
    NOTICE(
        "Multiprocessor: local APIC unavailable; keeping the bootstrap "
        "processor only");
    return 1;
  }

  // Did we find a processor list?
  bool bMPInfoFound = false;
  // List of information about each usable processor
  const Vector<ProcessorInformation*>* Processors = 0;

  EMIT_IF(ACPI) {
    // Search through the ACPI tables
    Acpi& acpi = Acpi::instance();
    if ((bMPInfoFound = acpi.validProcessorInfo()) == true)
      Processors = &acpi.getProcessorList();
  }

  EMIT_IF(SMP) {
    // Search through the SMP tables
    Smp& smp = Smp::instance();
    if (bMPInfoFound == false && (bMPInfoFound = smp.valid()) == true)
      Processors = &smp.getProcessorList();
  }

  // No processor list found
  if (bMPInfoFound == false || !Processors) {
    NOTICE(
        "Multiprocessor: couldn't find any information about multiple "
        "processors");
    return 1;
  }

  NOTICE("Multiprocessor: Found " << Dec << Processors->count() << Hex << " processors");

  // Copy the trampoline code to 0x7000
  /// \note This is a slightly hacky way to have the code linked directly to
  /// the
  ///       kernel - we hard-code specific offsets. Avoids the "relocation
  ///       truncated to fit" error from ld.
  MemoryCopy(reinterpret_cast<void*>(0x7000), reinterpret_cast<void*>(&mp_trampoline16), 0x100);
  MemoryCopy(reinterpret_cast<void*>(0x7100), reinterpret_cast<void*>(&mp_trampoline32), 0x100);
  // The first far jump enters 32-bit protected mode; the trampoline loads
  // the 64-bit GDT itself only after enabling long mode.
  MemoryCopy(reinterpret_cast<void*>(0x7200), &trampolinegdtr, 0x10);
  MemoryCopy(reinterpret_cast<void*>(0x7210), &trampolinegdt, 0xF0);

  volatile uintptr_t* trampolineStack;
  volatile uintptr_t* trampolineKernelEntry;

  // Parameters for the trampoline code
  EMIT_IF(X86) {
    // dead code path
  }
  else {
    trampolineStack = reinterpret_cast<volatile uintptr_t*>(0x7FF0);
    trampolineKernelEntry = reinterpret_cast<volatile uintptr_t*>(0x7FE8);

    // The AP trampoline ABI reserves 0x7FF8 for the boot PML4 address.
    // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
    *reinterpret_cast<volatile uintptr_t*>(0x7FF8) =
        static_cast<X64VirtualAddressSpace&>(VirtualAddressSpace::getKernelAddressSpace())
            .m_PhysicalPML4;
  }

  // Set the entry point
  *trampolineKernelEntry = reinterpret_cast<uintptr_t>(&applicationProcessorStartup);

  LocalApic& localApic = Pc::instance().getLocalApic();
  VirtualAddressSpace& kernelSpace = VirtualAddressSpace::getKernelAddressSpace();
  // Startup the application processors through startup interprocessor
  // interrupt
  for (size_t i = 0; i < Processors->count(); i++) {
    // Add a ProcessorInformation object
    ::ProcessorInformation* pProcessorInfo = 0;

    // Startup the processor
    if (localApic.getId() != (*Processors)[i]->apicId) {
      // AP: set up a proper information structure
      pProcessorInfo =
          new ::ProcessorInformation((*Processors)[i]->processorId, (*Processors)[i]->apicId);
      Processor::m_ProcessorInformation.pushBack(pProcessorInfo);

      // Allocate kernel stack
      VirtualAddressSpace::Stack* pStack = kernelSpace.allocateStack();

      // Set trampoline stack
      *trampolineStack = reinterpret_cast<uintptr_t>(pStack->getTop());

      NOTICE(" Booting processor #" << Dec << (*Processors)[i]->processorId << ", stack at 0x"
                                    << Hex << reinterpret_cast<uintptr_t>(pStack->getTop()));

      m_ProcessorStarted = false;

      // INIT ignores the vector field and requires an asserted and a
      // deasserted level write. The trampoline is at 0x7000, so the
      // STARTUP vector is page 7.
      if (!localApic.interProcessorInterrupt((*Processors)[i]->apicId, 0,
                                             LocalApic::deliveryModeInit, true, true)) {
        ERROR("Multiprocessor: INIT assert failed for processor #"
              << Dec << (*Processors)[i]->processorId << " (APIC " << (*Processors)[i]->apicId
              << ")");
        panic("Multiprocessor: INIT assert did not complete");
      }
      if (!localApic.interProcessorInterrupt((*Processors)[i]->apicId, 0,
                                             LocalApic::deliveryModeInit, false, true)) {
        ERROR("Multiprocessor: INIT deassert failed for processor #"
              << Dec << (*Processors)[i]->processorId << " (APIC " << (*Processors)[i]->apicId
              << ")");
        panic("Multiprocessor: INIT deassert did not complete");
      }
      if (!earlyBootDelay(InitToStartupDelayMicroseconds))
        panic("Multiprocessor: early-boot INIT delay timed out");

      if (!localApic.interProcessorInterrupt((*Processors)[i]->apicId, 0x07,
                                             LocalApic::deliveryModeStartup, true, false)) {
        ERROR(
            "Multiprocessor: first STARTUP IPI failed for "
            "processor #"
            << Dec << (*Processors)[i]->processorId << " (APIC " << (*Processors)[i]->apicId
            << ")");
        panic("Multiprocessor: first STARTUP IPI did not complete");
      }
      if (!earlyBootDelay(StartupIpiDelayMicroseconds))
        panic("Multiprocessor: early-boot STARTUP delay timed out");

      if (!m_ProcessorStarted) {
        if (!localApic.interProcessorInterrupt((*Processors)[i]->apicId, 0x07,
                                               LocalApic::deliveryModeStartup, true, false)) {
          ERROR(
              "Multiprocessor: second STARTUP IPI failed for "
              "processor #"
              << Dec << (*Processors)[i]->processorId << " (APIC " << (*Processors)[i]->apicId
              << ")");
          panic("Multiprocessor: second STARTUP IPI did not complete");
        }
        if (!earlyBootDelay(StartupIpiDelayMicroseconds))
          panic("Multiprocessor: second STARTUP delay timed out");
      }

      const uint64_t startupWaitStart = Rtc::instance().getTickCountNano();
      bool processorStarted = false;
      for (size_t poll = 0; poll < ApplicationProcessorPollLimit; ++poll) {
        if (m_ProcessorStarted) {
          processorStarted = true;
          break;
        }
        if ((Rtc::instance().getTickCountNano() - startupWaitStart) >=
            ApplicationProcessorTimeoutNanoseconds)
          break;
        Processor::pause();
      }
      if (!processorStarted && !m_ProcessorStarted) {
        ERROR("Multiprocessor: processor #" << Dec << (*Processors)[i]->processorId << " (APIC "
                                            << (*Processors)[i]->apicId
                                            << ") did not acknowledge startup");
        panic("Multiprocessor: application processor startup timed out");
      }
    } else {
      NOTICE("Currently running on CPU #" << Dec << localApic.getId() << Hex
                                          << ", skipping boot (not necessary)");

      Processor::m_ProcessorInformation.pushBack(&Processor::m_SafeBspProcessorInformation);
      Processor::m_SafeBspProcessorInformation.setIds((*Processors)[i]->processorId,
                                                      (*Processors)[i]->apicId);
    }
  }

  return Processors->count();
}

void Multiprocessor::initialise2() {
  m_ProcessorLock2.release();
}

#endif

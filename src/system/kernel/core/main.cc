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

/**
 * \mainpage
 *
 * \section main_intro Introduction
 * Pedigree is a hobby operating system primarily designed by James Molloy and
 * Joerg Pfahler and primarily implemented by James Molloy, Joerg Pfahler, and
 * Matthew Iselin.
 *
 * Just a user looking for help? Head straight to \ref user_guide.
 *
 * The objectives of Pedigree are to develop a solid yet portable operating
 * system from the ground up with an object oriented architecture where
 * possible. The goal is to support multiple different subsystems to allow many
 * different applications to run natively on Pedigree. At the moment a POSIX
 * subsystem exists, with plans for the implementation of a native subsystem.
 * Pedigree also caters for two different driver interfaces: our native, C++
 * interface, and the C "CDI" interface (ported from the Tyndur operating
 * system).
 *
 * At this stage Pedigree has a variety of substantial features. Pedigree has a
 * functional TCP/IP stack that can be used for anything from connecting to IRC
 * or browsing the internet. Some SDL applications can be compiled to run on
 * Pedigree, and the graphics framework provides a robust C++ API for
 * applications that need direct, unhindered access to the video framebuffer.
 * Many POSIX applications can run on Pedigree with a simple recompile, all
 * built upon the solid POSIX subsystem - including popular applications such as
 * bash, lynx, and Apache. Pedigree also supports a variety of USB devices
 * including mass storage devices, keyboards, mice, and DM9601-based USB
 * ethernet adapters.
 *
 * The OS currently supports the following architectures in various degrees;
 *
 * - x64 / x86-64 (x86/IA32 support has been deprecated)
 *
 * \section main_docs This Documentation
 *
 * This documentation is generated for each commit made to the repository, and
 * also on a nightly basis. Patches and pull requests to improve the state of
 * documentation across the codebase are always appreciated.
 *
 * Some parts of the online generated documentation may be incomplete as the
 * documentation is generated with preprocessor definitions typically used to
 * build for X86-64.
 *
 * Find a bug in documentation, or incorrect documentation? Please let us know
 * by following the escalation path described in the \ref main_resources
 * section.
 *
 * \section user_guide Pedigree User Guide
 *
 * - \ref pedigree_whatsdifferent
 *
 * \section main_links Components
 * The following lists the various components across the operating system.
 *
 * - \ref module_main
 * - \ref mmap_main
 * - \ref module_nativeapi
 * - \ref registry
 * - \ref event_system
 *
 * \section main_resources Resources
 * - \ref pedigree_porting
 * - The main repository for Pedigree is at https://github.com/miselin/pedigree.
 *
 * - If you are interested in contributing or have found a bug, please open an
 * issue at https://github.com/miselin/pedigree/issues.
 */

#include "pedigree/kernel/Archive.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/Version.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/core/BootIO.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/core/cppsupport.h"
#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Trace.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/InfoBlock.h"
#include "pedigree/kernel/process/MemoryPressureKiller.h"
#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/process/OwnedThread.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/KernelCoreSyscallManager.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#if DEBUGGER
#include "pedigree/kernel/debugger/Debugger.h"
#include "pedigree/kernel/debugger/commands/LocksCommand.h"
#endif

#if THREADS
#include "pedigree/kernel/utilities/ZombieQueue.h"
#endif

#if HOSTED
namespace __pedigree_hosted {};  // namespace __pedigree_hosted
using namespace __pedigree_hosted;
#include <stdio.h>
#endif

/** Output device for boot-time information. */
EXPORTED_PUBLIC BootIO bootIO;

/** Global copy of the bootstrap information. */
BootstrapStruct_t* g_pBootstrapInfo;

/** Do we need to shutdown? */
static Atomic<bool> g_NeedsShutdown(false);

#if HOSTED && PEDIGREE_HOSTED_IRQ_CLOSURE_TESTS
extern bool runHostedIrqClosureRegressions();
#endif

/** Handles doing recovery on SLAM if memory pressure is encountered. */
class SlamRecovery : public MemoryPressureHandler {
 public:
  virtual const char* getMemoryPressureDescription();
  virtual bool compact();
};

/** Kernel entry point for application processors (after processor/machine has
   been initialised on the particular processor */
#if MULTIPROCESSOR
void apMain() {
  NOTICE("Processor #" << Processor::id() << " started.");

  EMIT_IF(THREADS) {
    // Add us as the idle thread for this CPU.
    Processor::information().getScheduler().setIdle(Processor::information().getCurrentThread());
  }

  Processor::setInterrupts(true);
  for (;;) {
    Processor::haltUntilInterrupt();

    EMIT_IF(THREADS) {
      Scheduler::instance().yield();
    }
  }
}
#endif

ModuleInfo* g_StaticDrivers[128];
size_t g_StaticDriverN = 0;

/** Loads all kernel modules */
static int loadModules(void* inf) {
  Archive* initrd = nullptr;

  EMIT_IF(STATIC_DRIVERS) {
    extern uintptr_t start_module_ctors;
    extern uintptr_t end_module_ctors;

    // Call static constructors before we start. If we don't... there won't
    // be any properly initialised ModuleInfo structures :)
    uintptr_t* iterator = &start_module_ctors;
    while (iterator < &end_module_ctors) {
      void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
      fp();
      iterator++;
    }

    for (size_t i = 0; i < g_StaticDriverN; ++i) {
      assert(g_StaticDrivers[i]->tag == MODULE_TAG);
      KernelElf::instance().loadModule(g_StaticDrivers[i]);
    }

    KernelElf::instance().executeModules();
  }
  else {
    BootstrapStruct_t* bsInf = static_cast<BootstrapStruct_t*>(inf);

    NOTICE("initrd @ " << Hex << bsInf->getInitrdAddress() << " -> "
                       << (bsInf->getInitrdAddress() + bsInf->getInitrdSize()) << ", " << Dec
                       << bsInf->getInitrdSize() << " bytes");

    /// \note We have to do this before we call
    /// Processor::initialisationDone() otherwise the
    ///       BootstrapStruct_t might already be unmapped
    initrd = new Archive(bsInf->getInitrdAddress(), bsInf->getInitrdSize());
    bsInf = nullptr;

    size_t nFiles = initrd->getNumFiles();
    NOTICE("there are " << nFiles << " files");
    g_BootProgressTotal = nFiles * 2;  // Each file has to be preloaded and executed.
    for (size_t i = 0; i < nFiles; i++) {
      // Handle archives with `._<filename>` entries from Apple tar file
      // creation
      if (!StringCompareN(initrd->getFileName(i), "._", 2)) {
        continue;
      }

      NOTICE("loading module #" << i << " (" << initrd->getFileName(i) << ")...");
      Processor::setInterrupts(true);
      KernelElf::instance().loadModule(reinterpret_cast<uint8_t*>(initrd->getFile(i)),
                                       initrd->getFileSize(i));
      if (!Processor::getInterrupts())
        WARNING("A loaded module disabled interrupts.");
    }

    // Start any modules we can run already.
    KernelElf::instance().executeModules();
  }

  // Wait for all modules to finish loading before we continue.
  KernelElf::instance().waitForModulesToLoad();

  // The initialisation is done here, unmap/free the .init section and on
  // x86/64 the identity mapping of 0-4MB NOTE: BootstrapStruct_t unusable
  // after this point
  Processor::initialisationDone();

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  // The ISO-only smoke image deliberately has no root filesystem. Once every
  // eligible module has retired, request a controlled shutdown instead of
  // invoking userspace through the intentionally pending POSIX dependency.
  g_NeedsShutdown = true;
#else
  // Now that we've cleaned up and are done loading modules, we can run the
  // init module.
  KernelElf::instance().invokeInitModule();

  if (KernelElf::instance().hasPendingModules()) {
    FATAL("At least one module's dependencies were never met.");
  }
#endif

  // It's now safe to clean up the initrd archive
  if (initrd) {
    delete initrd;
  }

#if HOSTED
  fprintf(stderr, "Pedigree has started: all modules have been loaded.\n");
#endif

  NOTICE("module load thread is terminating");
  return 0;
}

/** Kernel entry point. */
extern "C" void _main(BootstrapStruct_t& bsInf) USED;
void _cxx_main(BootstrapStruct_t& bsInf);
extern "C" void _main(BootstrapStruct_t& bsInf) {
  _cxx_main(bsInf);
}

void _cxx_main(BootstrapStruct_t& bsInf) {
  TRACE("constructors");

  // Firstly call the constructors of all global objects.
  initialiseConstructors();

  g_pBootstrapInfo = &bsInf;

  EMIT_IF(TRACK_LOCKS) {
    g_LocksCommand.setReady();
  }

  TRACE("Processor init");

  // Initialise the processor-specific interface
  Processor::initialise1(bsInf);

  TRACE("log init");

  // Initialise the kernel log
  Log::instance().initialise1();

  TRACE("Machine init");

  // Initialise the machine-specific interface
  Machine& machine = Machine::instance();
  Machine::instance().initialiseDeviceTree();

  machine.initialise();

#if DEBUGGER && (!defined(PEDIGREE_HOSTED_DARWIN) || !PEDIGREE_HOSTED_DARWIN)
  TRACE("Debugger init");
  Debugger::instance().initialise();
#endif

  TRACE("Machine init2");

  machine.initialise2();

  TRACE("Log init2");

  // Initialise the kernel log's callbacks
  Log::instance().initialise2();

  TRACE("Processor init2");

  // Initialise the processor-specific interface
  // Bootup of the other Application Processors and related tasks
  Processor::initialise2(bsInf);

  TRACE("Machine init3");

  machine.initialise3();

  TRACE("KernelElf init");

  // Initialise the Kernel Elf class
  if (KernelElf::instance().initialise(bsInf) == false)
    panic("KernelElf::initialise() failed");

  EMIT_IF(!STATIC_DRIVERS) {
    // initrd needed if drivers aren't statically linked.
    if (bsInf.isInitrdLoaded() == false)
      panic("Initrd module not loaded!");
  }

  TRACE("kernel syscall init");

  KernelCoreSyscallManager::instance().initialise();

  TRACE("initial init done, enabling interrupts");

  Processor::setInterrupts(true);

  TRACE("bootIO init");

  // Initialise the boot output.
  bootIO.initialise();

  // Spew out a starting string.
  HugeStaticString str, ident;
  str += "Pedigree - revision ";
  str += g_pBuildRevision;
  EMIT_IF(DONT_LOG_TO_SERIAL) {
    str += "\n=======================\n";
  }
  else {
    str += "\r\n=======================\r\n";
  }
  bootIO.write(str, BootIO::White, BootIO::Black);

  str.clear();
  str += "Built at ";
  str += g_pBuildTime;
  str += " by ";
  str += g_pBuildUser;
  str += " on ";
  str += g_pBuildMachine;
  EMIT_IF(DONT_LOG_TO_SERIAL) {
    str += "\n";
  }
  else {
    str += "\r\n";
  }
  bootIO.write(str, BootIO::LightGrey, BootIO::Black);

  str.clear();
  str += "Build flags: ";
  str += g_pBuildFlags;
  EMIT_IF(DONT_LOG_TO_SERIAL) {
    str += "\n";
  }
  else {
    str += "\r\n";
  }
  bootIO.write(str, BootIO::LightGrey, BootIO::Black);

  str.clear();
  str += "Processor information: ";
  Processor::identify(ident);
  str += ident;
  EMIT_IF(DONT_LOG_TO_SERIAL) {
    str += "\n";
  }
  else {
    str += "\r\n";
  }
  bootIO.write(str, BootIO::LightGrey, BootIO::Black);

  TRACE("creating graphics service");

  // Set up the graphics service for drivers to register with
  EMIT_IF(!NOGFX) {
    GraphicsService* pService = new GraphicsService;
    ServiceFeatures* pFeatures = new ServiceFeatures;
    pFeatures->add(ServiceFeatures::touch);
    pFeatures->add(ServiceFeatures::withdraw);
    pFeatures->add(ServiceFeatures::probe);
    ServiceManager::instance().addService(String("graphics"), pService, pFeatures);
  }

  TRACE("creating memory pressure handlers");

  // Set up SLAM recovery memory pressure handler.
  SlamRecovery recovery;
  MemoryPressureManager::instance().registerHandler(MemoryPressureManager::HighestPriority,
                                                    &recovery);

  // Set up the process killer memory pressure handler.
  MemoryPressureProcessKiller killer;
  MemoryPressureManager::instance().registerHandler(MemoryPressureManager::LowestPriority, &killer);

  // Set up the global info block manager.
  TRACE("InfoBlockManager init");
  InfoBlockManager::instance().initialise();

  // Bring up the cache subsystem.
  TRACE("CacheManager init");
  CacheManager::instance().initialise();

  // Initialise the input manager
  TRACE("InputManager init");
  InputManager::instance().initialise();

  EMIT_IF(THREADS) {
    TRACE("ZombieQueue init");
    ZombieQueue::instance().initialise();
  }

  /// \todo Seed random number generator.

  OwnedThread moduleLoadThread;
#if HOSTED && PEDIGREE_HOSTED_IRQ_CLOSURE_TESTS
  runHostedIrqClosureRegressions();
#else
  TRACE("starting module load thread");

  EMIT_IF(THREADS) {
    Thread* pThread = new Thread(Processor::information().getCurrentThread()->getParent(),
                                 &loadModules, static_cast<void*>(&bsInf), 0);
    pThread->setName("module load thread");
    moduleLoadThread.adopt(pThread);
  }
  else {
    loadModules(&bsInf);
  }
#endif

  EMIT_IF(DEBUGGER_RUN_AT_START) {
    Processor::breakpoint();
  }

  TRACE("becoming idle");

  EMIT_IF(THREADS) {
    // Add us as the idle thread for this CPU.
    Processor::information().getScheduler().setIdle(Processor::information().getCurrentThread());
  }

  // This will run when nothing else is available to run
  while (!g_NeedsShutdown) {
    // Always enable interrupts in the idle thread, and halt. There is no
    // point yielding as if this code is running, no other thread is ready
    // (and cannot be made ready without an interrupt).
    Processor::setInterrupts(true);
    Processor::haltUntilInterrupt();

    // Give up our timeslice (needed especially for no-tick scheduling)
    Scheduler::instance().yield();
  }

  EMIT_IF(THREADS) {
    // Shutdown joins need this Thread to enter an ordinary WaitQueue and
    // be republished when a worker exits. An idle Thread is only a fallback
    // and never commits Sleeping, so retire that role before the first
    // join rather than leaving its wake behind unrelated ready workers.
    Processor::information().getScheduler().setIdle(nullptr);

    // A module can request shutdown from its entry point. The loader still
    // has bookkeeping and initrd cleanup to finish after that request, so
    // module teardown must not race it.
    moduleLoadThread.join();
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && !PEDIGREE_HOSTED_IRQ_CLOSURE_TESTS
    NOTICE("HOSTED-WAIT-TEST: PASS module-loader-shutdown-join");
#endif

    // The zombie worker must be joined before global teardown disables
    // interrupts and destroys the scheduler.
    ZombieQueue::instance().destroy();
  }

  NOTICE("Resetting...");

  // Clean up all loaded modules (unmounts filesystems and the like).
  KernelElf::instance().unloadModules();

  EMIT_IF(STATIC_DRIVERS) {
    extern uintptr_t start_module_dtors;
    extern uintptr_t end_module_dtors;

    // Call all the module destructors now
    uintptr_t* iterator = &start_module_dtors;
    while (iterator < &end_module_dtors) {
      void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
      fp();
      iterator++;
    }
  }

  // No need for user input anymore.
  InputManager::instance().shutdown();

  // Module teardown has retired device caches. Drain the cache timer and
  // workers while the platform timer registry is still available.
  CacheManager::destroyInstance();

  // Stop active platform services while their worker/callback drains can
  // still schedule.
  Machine::instance().deinitialise();

  // Teardown above joins module, input, keyboard, and threaded-IRQ workers.
  // Keep every scheduler CPU available until those drains have completed;
  // once the remaining work is strictly local, retire the APs permanently.
  EMIT_IF(MULTIPROCESSOR) {
    if (!Machine::instance().stopAllOtherProcessors()) {
      ERROR_NOLOCK("Shutdown aborted: not all other processors stopped");
      Processor::setInterrupts(false);
      while (true)
        Processor::halt();
    }
  }

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  NOTICE("QEMU-CONCURRENCY-TEST: PASS shutdown-worker-drain-smp");
#endif

  Processor::setInterrupts(false);

  // Shut down the various pieces created by Processor before their global
  // objects are destroyed.
  Processor::deinitialise();

  NOTICE("All modules unloaded. Running destructors and terminating...");
  runKernelDestructors();

  // Done - return to caller.
  // Boot code needs to handle this by resetting (or whatever makes sense)
  TRACE("kernel main() terminating");
}

void EXPORTED_PUBLIC system_reset();
void system_reset() {
  // Close out the main thread.
  g_NeedsShutdown = true;
}

void system_reboot() {
  WARNING("System shutting down...");
  Process* currentProcess = Processor::information().getCurrentThread()->getParent();
  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  const size_t shutdownProcessCount = Scheduler::instance().getNumProcesses();
  for (size_t i = shutdownProcessCount; i > 0; --i) {
    Scheduler::ProcessLease process;
    if (!Scheduler::instance().acquireProcess(process, i - 1)) {
      continue;
    }
    Subsystem* subsystem = process->getSubsystem();
    if (process.get() == currentProcess || process.get() == kernelProcess) {
      continue;
    }

    if (subsystem) {
      Process::ThreadLease target;
      if (process->acquireThread(target, static_cast<size_t>(0))) {
        subsystem->kill(Subsystem::Terminated, target.get());
      }
    } else {
      FATAL(
          "Shutdown found a non-kernel Process without a teardown "
          "Subsystem");
    }
  }

  while (true) {
    Scheduler::ProcessLease processToReap;
    const size_t processCount = Scheduler::instance().getNumProcesses();
    for (size_t i = 0; i < processCount; ++i) {
      Scheduler::ProcessLease candidate;
      if (Scheduler::instance().acquireProcess(candidate, i) && candidate.get() != currentProcess &&
          candidate.get() != kernelProcess) {
        processToReap = pedigree_std::move(candidate);
        break;
      }
    }

    if (!processToReap) {
      break;
    }
    if (!processToReap->waitUntilTerminationReapable()) {
      FATAL(
          "reboot attempted to reap the currently terminating "
          "process");
    }

    Process* processIdentity = processToReap.get();
    Process::ReaperClaim reaper = processToReap->tryClaimReaper();
    if (reaper) {
      reaper.publish();
    }
    processToReap.reset();
    Scheduler::instance().waitUntilProcessRemoved(processIdentity);
  }

  Subsystem* currentSubsystem = currentProcess->getSubsystem();
  if (!currentSubsystem) {
    FATAL("System reboot requires a userspace shutdown coordinator");
  }

  // The reaper owns the Process before its final Thread leaves the stack.
  // Keeping the kernel Process registered preserves the final parent/adopter
  // topology until off-stack completion is published.
  Process::ReaperClaim shutdownReaper = currentProcess->tryClaimReaper();
  if (!shutdownReaper) {
    FATAL("Shutdown coordinator Process already has a reaper");
  }
  shutdownReaper.publish();
  system_reset();
  Processor::information().getScheduler().requestCurrentThreadExitToIdle();
  currentSubsystem->exit(0);
  FATAL("Shutdown coordinator returned from process exit");
}

const char* SlamRecovery::getMemoryPressureDescription() {
  return "SLAM recovery; freeing unused slabs.";
}

bool SlamRecovery::compact() {
  return SlamAllocator::instance().recovery(5) != 0;
}

/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/Module.h"
#if !HOSTED || !PEDIGREE_HOSTED_SMOKE_TESTS || !THREADS
#error "The swap VM fixture requires hosted smoke tests with threads"
#endif
extern bool runSwapRegressions();
extern void system_reset();
namespace {
int worker(void*) {
  char name[] = "swap-vm-tests";
  const auto deadline = Time::getTicks() + 10 * Time::Multiplier::Second;
  while (!KernelElf::instance().moduleIsLoaded(name)) {
    if (Time::getTicks() >= deadline) {
      ERROR("SWAP-VM-TEST: FAIL fixture did not become active");
      NOTICE("SWAP-VM-TEST: END FAIL");
      system_reset();
      return 1;
    }
    Scheduler::instance().yield();
  }
  const bool passed = runSwapRegressions();
  if (!passed)
    NOTICE("SWAP-VM-TEST: END FAIL");
  system_reset();
  return passed ? 0 : 1;
}
bool entry() {
  auto* thread = new Thread(Scheduler::instance().getKernelProcess(), worker, nullptr);
  if (!thread)
    return false;
  thread->setName("Swap VM contracts");
  thread->detach();
  return true;
}
void exit() {}
}  // namespace
MODULE_INFO_NON_UNLOADABLE("swap-vm-tests", &entry, &exit, "vfs");

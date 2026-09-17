/* Copyright (c) 2026, Pedigree Developers. */

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/DeferredTimeAccounting.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/SyscallManager.h"

#include <cstdlib>

#include <benchmark/benchmark.h>

// The shared kernel also contains the ordinary hosted bootstrap. These are
// intentionally inert for this benchmark; the benchmark executable owns main
// and never enters the module loader.
extern "C" {
__attribute__((visibility("default"))) uintptr_t start_module_ctors[1] = {};
__attribute__((visibility("default"))) uintptr_t end_module_ctors[1] = {};
__attribute__((visibility("default"))) uintptr_t start_module_dtors[1] = {};
__attribute__((visibility("default"))) uintptr_t end_module_dtors[1] = {};
}

namespace {

class NoopSyscallHandler final : public SyscallHandler {
 public:
  uintptr_t syscall(SyscallState&) override {
    return 0;
  }
};

SyscallManager* g_SyscallManager = nullptr;

void BM_HostedNoopSyscall(benchmark::State& state) {
  uintptr_t observed = 0;
  for (auto _ : state) {
    observed |= g_SyscallManager->syscall(TUI, 0);
  }
  benchmark::DoNotOptimize(observed);
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_HostedNoopSyscall)->UseRealTime();

void initialiseHostedKernel() {
  BootstrapStruct_t bootstrap;
  // Processor initialisation logs before the normal boot sequence prepares the
  // machine timer. Keep those timestamps valid without starting hosted IRQs.
  Machine::instance().getTimer()->synchronise();
  Processor::initialise1(bootstrap);
  Processor::initialise2(bootstrap);
  Processor::setInterrupts(true);

  Thread* thread = Processor::information().getCurrentThread();
  if (!thread) {
    std::abort();
  }
  thread->recordTime(CpuTimeMode::User);

  static NoopSyscallHandler handler;
  static SyscallManager::Registration registration;
  SyscallManager& manager = SyscallManager::instance();
  if (!manager.registerSyscallHandler(TUI, &handler, registration)) {
    std::abort();
  }
  g_SyscallManager = &manager;
}

}  // namespace

int main(int argc, char** argv) {
  // The shared library's normal Darwin initializers have already initialized
  // the kernel singletons before this executable reaches main().
  initialiseHostedKernel();

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}

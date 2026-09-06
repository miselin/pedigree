/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
Module::UnloadAdmission decision = Module::UnloadAdmission::Busy;
size_t admissions = 0;
size_t exits = 0;
bool sawTerminal = false;
Module::UnloadAdmission admit(bool terminal) {
  ++admissions;
  sawTerminal = terminal;
  return decision;
}
void exitModule() {
  ++exits;
}
bool attempt(Module& module, bool terminal) {
  if (KernelElf::claimModuleUnloadForTest(&module) != KernelElf::TestUnloadClaimed)
    return false;
  return KernelElf::completeGuardedModuleUnloadForTest(&module, terminal);
}
}  // namespace
bool runHostedModuleAdmissionRegressions() {
  Module module;
  module.name.assign("unload-admission-fixture");
  module.status = Module::Active;
  module.unloadAdmission = admit;
  module.exit = exitModule;
  decision = Module::UnloadAdmission::Busy;
  admissions = exits = 0;
  bool passed = !attempt(module, false) && module.isActive() && !module.unloadComplete &&
                module.unloadable && admissions == 1 && exits == 0 && !sawTerminal;
  decision = Module::UnloadAdmission::KeepMapped;
  passed &= !attempt(module, false) && module.isActive() && module.unloadable && exits == 0;
  decision = Module::UnloadAdmission::Ready;
  passed &= attempt(module, false) && module.isUnloaded() && exits == 1 && admissions == 3;

  Module provider;
  provider.name.assign("retained-storage-provider");
  provider.status = Module::Active;
  Module consumer;
  consumer.name.assign("retained-storage-consumer");
  consumer.status = Module::Active;
  consumer.unloadAdmission = admit;
  consumer.exit = exitModule;
  const char* dependencies[] = {"retained-storage-provider", nullptr};
  consumer.depends = dependencies;
  decision = Module::UnloadAdmission::KeepMapped;
  passed &= !attempt(consumer, true) && consumer.isActive() && !consumer.unloadable &&
            !consumer.unloadComplete && sawTerminal && exits == 1;
  Module* modules[] = {&provider, &consumer};
  Module* order[2]{};
  passed &= KernelElf::planModuleUnloadOrderForTest(modules, 2, order, 2) == 0 &&
            provider.isActive() && consumer.isActive();
  if (passed)
    NOTICE("HOSTED-STORAGE-PAGING: PASS module-admission-retention");
  else
    ERROR("HOSTED-STORAGE-PAGING: FAIL module-admission-retention");
  return passed;
}
#endif

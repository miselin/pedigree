/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/linker/ModuleImage.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/Module.h"

#if !HOSTED || !PEDIGREE_HOSTED_SMOKE_TESTS
#error "The runtime-module lifecycle fixture requires the hosted smoke test configuration"
#endif

extern void system_reset();
extern "C" {
extern const uint8_t runtime_provider_image[];
extern const size_t runtime_provider_image_size;
extern const uint8_t runtime_consumer_image[];
extern const size_t runtime_consumer_image_size;
extern const uint8_t runtime_undeclared_image[];
extern const size_t runtime_undeclared_image_size;
}

namespace {
using Load = KernelElf::RuntimeLoadResult;
using Unload = KernelElf::RuntimeUnloadResult;
size_t constructors = 0, entries = 0, exits = 0, destructors = 0;
bool entryFails = false, visibleDuringEntry = false, expectHidden = true;
ModuleImage inspection;

bool check(bool value, const char* detail) {
  if (!value)
    ERROR("HOSTED-RUNTIME-MODULE-TEST: FAIL " << detail);
  return value;
}

Load load(const uint8_t* image, size_t length, char suffix = 0) {
  auto& kernel = KernelElf::instance();
  KernelElf::RuntimeLoad transaction;
  const auto admitted = kernel.beginRuntimeModuleLoad(length, transaction);
  if (admitted != Load::Ready)
    return admitted;
  MemoryCopy(transaction.data(), image, length);
  if (suffix) {
    ModuleImage::Symbol name;
    uintptr_t address = 0;
    size_t offset = 0;
    if (inspection.preflight(transaction.data(), length) != ModuleImage::Result::Valid ||
        !inspection.findSymbol("g_pModuleName", name) ||
        !inspection.localPointer(name.value, address) ||
        !inspection.fileOffset(address, 17, offset))
      return Load::InvalidImage;
    // Rename only the fixed trailing digit in the benign provider fixture.
    transaction.data()[offset + StringLength("runtime-fixture-")] = suffix;
  }
  return kernel.loadModuleRuntime(transaction);
}

bool run() {
  auto& kernel = KernelElf::instance();
  const auto start = Time::getTicks();
  bool ready = false;
  for (size_t attempt = 0;
       attempt < 100000 && Time::getTicks() - start < 10 * Time::Multiplier::Second; ++attempt) {
    KernelElf::RuntimeLoad probe;
    const auto admitted = kernel.beginRuntimeModuleLoad(runtime_provider_image_size, probe);
    if (admitted == Load::Ready) {
      ready = true;
      break;
    }
    if (admitted != Load::Busy)
      return check(false, "arena admission");
    Scheduler::instance().yield();
  }
  if (!check(ready, "bounded wait for Active boot provider"))
    return false;
  NOTICE("HOSTED-RUNTIME-MODULE-TEST: PASS active-admission");

  if (!check(inspection.preflight(runtime_provider_image, runtime_provider_image_size) ==
                 ModuleImage::Result::Valid,
             "native fixture preflight") ||
      !check(load(runtime_provider_image, 16) == Load::InvalidImage, "truncated header") ||
      !check(load(runtime_consumer_image, runtime_consumer_image_size) == Load::MissingDependency,
             "missing provider"))
    return false;
  NOTICE("HOSTED-RUNTIME-MODULE-TEST: PASS preflight-missing-provider");

  {
    KernelElf::RuntimeLoad first, second;
    if (!check(kernel.beginRuntimeModuleLoad(runtime_provider_image_size, first) == Load::Ready &&
                   kernel.beginRuntimeModuleLoad(runtime_provider_image_size, second) == Load::Busy,
               "single transition admission"))
      return false;
  }
  KernelElf::failRuntimeProtectionForTest(2);
  if (!check(load(runtime_provider_image, runtime_provider_image_size) == Load::ProtectionFailed,
             "recoverable protection failure") ||
      !check(!constructors && !entries && !exits && !destructors, "protection failure ran code"))
    return false;
  NOTICE("HOSTED-RUNTIME-MODULE-TEST: PASS protection-rollback");

  entryFails = true;
  if (!check(load(runtime_provider_image, runtime_provider_image_size) == Load::EntryFailed,
             "entry failure") ||
      !check(constructors == 1 && entries == 1 && exits == 1 && destructors == 1,
             "entry failure lifecycle cleanup") ||
      !check(kernel.unloadModuleRuntime("runtime-fixture-0") == Unload::NotFound,
             "failed entry remained published"))
    return false;
  entryFails = false;
  if (!check(load(runtime_provider_image, runtime_provider_image_size) == Load::Loaded,
             "retry after entry failure") ||
      !check(!visibleDuringEntry, "incomplete exports became visible") ||
      !check(kernel.globalLookupSymbol("module_runtime_fixture_value") != 0, "Active export") ||
      !check(load(runtime_provider_image, runtime_provider_image_size) == Load::Duplicate,
             "duplicate live name"))
    return false;
  NOTICE("HOSTED-RUNTIME-MODULE-TEST: PASS entry-rollback-retry-publish");

  expectHidden = false;
  if (!check(
          load(runtime_undeclared_image, runtime_undeclared_image_size) == Load::MissingDependency,
          "undeclared import provider") ||
      !check(load(runtime_consumer_image, runtime_consumer_image_size) == Load::Loaded,
             "declared provider import") ||
      !check(kernel.unloadModuleRuntime("runtime-fixture-0") == Unload::DependedOn,
             "live dependency did not pin provider") ||
      !check(kernel.unloadModuleRuntime("runtime-consumer") == Unload::Unloaded,
             "consumer unload") ||
      !check(kernel.unloadModuleRuntime("runtime-fixture-0") == Unload::Unloaded,
             "provider unload") ||
      !check(kernel.unloadModuleRuntime("runtime-fixture-0") == Unload::NotFound,
             "repeated unload") ||
      !check(!kernel.globalLookupSymbol("module_runtime_fixture_value"), "retired export visible"))
    return false;
  NOTICE("HOSTED-RUNTIME-MODULE-TEST: PASS dependency-unload");

  for (char suffix = '0'; suffix <= '3'; ++suffix) {
    if (!check(load(runtime_provider_image, runtime_provider_image_size, suffix) == Load::Loaded,
               "fill all four slots"))
      return false;
  }
  if (!check(load(runtime_provider_image, runtime_provider_image_size, '4') == Load::NoMemory,
             "bounded arena exhaustion"))
    return false;
  for (char suffix = '0'; suffix <= '3'; ++suffix) {
    char name[] = "runtime-fixture-0";
    name[sizeof(name) - 2] = suffix;
    if (!check(kernel.unloadModuleRuntime(name) == Unload::Unloaded, "slot reclaim"))
      return false;
  }
  expectHidden = true;
  if (!check(load(runtime_provider_image, runtime_provider_image_size) == Load::Loaded,
             "reload after slot exhaustion") ||
      !check(kernel.unloadModuleRuntime("runtime-fixture-0") == Unload::Unloaded, "final unload") ||
      !check(constructors == entries && entries == exits && exits == destructors,
             "exactly-once lifecycle totals"))
    return false;
  NOTICE("HOSTED-RUNTIME-MODULE-TEST: PASS exhaustion-reload");
  return true;
}

int worker(void*) {
  const bool passed = run();
  NOTICE("HOSTED-RUNTIME-MODULE-TEST: END " << (passed ? "PASS" : "FAIL"));
  system_reset();
  return passed ? 0 : 1;
}

bool entry() {
  if (!KernelElf::instance().prepareRuntimeModules()) {
    ERROR("HOSTED-RUNTIME-MODULE-TEST: FAIL boot preparation");
    NOTICE("HOSTED-RUNTIME-MODULE-TEST: END FAIL");
    system_reset();
    return false;
  }
  Thread* thread = new Thread(Scheduler::instance().getKernelProcess(), worker, nullptr);
  thread->setName("Runtime module lifecycle tests");
  thread->detach();
  return true;
}
void exit() {}
}  // namespace

extern "C" EXPORTED_PUBLIC void module_runtime_test_event(unsigned int event) {
  switch (event) {
    case 1:
      ++constructors;
      break;
    case 2:
      ++entries;
      if (expectHidden && KernelElf::instance().globalLookupSymbol("module_runtime_fixture_value"))
        visibleDuringEntry = true;
      break;
    case 3:
      ++exits;
      break;
    case 4:
      ++destructors;
      break;
  }
}
extern "C" EXPORTED_PUBLIC bool module_runtime_test_entry() {
  return !entryFails;
}
MODULE_INFO_NON_UNLOADABLE("module-runtime-tests", &entry, &exit);

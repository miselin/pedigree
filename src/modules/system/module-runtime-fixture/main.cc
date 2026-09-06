/* Copyright (c) 2026, Pedigree Developers. */
#include "modules/Module.h"

extern "C" void module_runtime_test_event(unsigned int event);
extern "C" bool module_runtime_test_entry();

#if defined(PEDIGREE_RUNTIME_CONSUMER)
extern "C" unsigned int module_runtime_fixture_value();
#else
extern "C" EXPORTED_PUBLIC unsigned int module_runtime_fixture_value() {
  return 42;
}
#endif

namespace {
void initialise() {
  module_runtime_test_event(1);
}
bool entry() {
  module_runtime_test_event(2);
#if defined(PEDIGREE_RUNTIME_CONSUMER)
  if (module_runtime_fixture_value() != 42)
    return false;
#endif
  return module_runtime_test_entry();
}
void exit() {
  module_runtime_test_event(3);
}
void finalise() {
  module_runtime_test_event(4);
}
void (*const constructor)() SECTION(".ctors") USED = initialise;
void (*const destructor)() SECTION(".dtors") USED = finalise;
}  // namespace

#if defined(PEDIGREE_RUNTIME_UNDECLARED)
MODULE_INFO("runtime-undeclared", &entry, &exit, "module-runtime-tests");
#elif defined(PEDIGREE_RUNTIME_CONSUMER)
MODULE_INFO("runtime-consumer", &entry, &exit, "module-runtime-tests", "runtime-fixture-0");
#else
MODULE_INFO("runtime-fixture-0", &entry, &exit, "module-runtime-tests");
#endif

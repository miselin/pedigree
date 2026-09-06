/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"

#include "modules/Module.h"

namespace {
bool entry() {
  NOTICE("MODULE-UNLOAD-FIXTURE: active");
  return true;
}

void exit() {
  NOTICE("MODULE-UNLOAD-FIXTURE: exit");
}

void finalise() {
  NOTICE("MODULE-UNLOAD-FIXTURE: destructor");
}

// Keep the callback in the loader's explicit destructor table.
void (*const destructor)() SECTION(".dtors") USED = finalise;
}  // namespace

MODULE_INFO("module-unload-fixture", &entry, &exit);

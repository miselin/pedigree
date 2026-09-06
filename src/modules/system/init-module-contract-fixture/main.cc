/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"

#include "modules/Module.h"

namespace {
unsigned int phase;
void initialise() {
  phase = 1;
}
bool entry() {
  if (phase != 1)
    return false;
  phase = 2;
  NOTICE("INIT-MODULE-FIXTURE: active");
  return true;
}
void exit() {
  phase = 3;
  NOTICE("INIT-MODULE-FIXTURE: exit");
}
void finalise() {
  if (phase == 3)
    NOTICE("INIT-MODULE-FIXTURE: destructor");
  else
    ERROR("INIT-MODULE-FIXTURE: FAIL lifecycle order");
  phase = 4;
}
void (*const constructor)() SECTION(".ctors") USED = initialise;
void (*const destructor)() SECTION(".dtors") USED = finalise;
}  // namespace
MODULE_INFO("init-module-contract", &entry, &exit);

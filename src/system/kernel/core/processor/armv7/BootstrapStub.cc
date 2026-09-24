#include "pedigree/kernel/BootstrapInfo.h"

extern "C" void armv7HandleException(void*) {
  for (;;) {
    asm volatile("wfi");
  }
}

extern "C" void _main(BootstrapStruct_t&) {
  for (;;) {
    asm volatile("wfi");
  }
}

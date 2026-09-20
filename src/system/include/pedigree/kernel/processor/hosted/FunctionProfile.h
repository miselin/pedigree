/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_KERNEL_HOSTED_FUNCTION_PROFILE_H
#define PEDIGREE_KERNEL_HOSTED_FUNCTION_PROFILE_H

#include <config.h>
#include <stddef.h>

#if PEDIGREE_HOSTED_FUNCTION_PROFILE
#include "pedigree/kernel/compiler.h"

enum class HostedProfileInvalidation {
  Schedule = 1,
  InterruptEnable = 2,
  ContextTransfer = 3,
};

EXPORTED_PUBLIC bool hostedFunctionProfileInitialise() __attribute__((no_instrument_function));
EXPORTED_PUBLIC size_t hostedFunctionProfileCount(size_t count)
    __attribute__((no_instrument_function));
EXPORTED_PUBLIC void hostedFunctionProfileBegin(const char* phase, size_t repetition, size_t count)
    __attribute__((no_instrument_function));
EXPORTED_PUBLIC bool hostedFunctionProfileEnd() __attribute__((no_instrument_function));
EXPORTED_PUBLIC void hostedFunctionProfileInvalidate(HostedProfileInvalidation reason)
    __attribute__((no_instrument_function));
#else
inline void hostedFunctionProfileBegin(const char*, size_t, size_t) {}
inline bool hostedFunctionProfileEnd() {
  return true;
}
#endif

#endif

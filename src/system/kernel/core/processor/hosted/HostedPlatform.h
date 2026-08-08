/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESSOR_HOSTED_PLATFORM_H
#define PEDIGREE_KERNEL_PROCESSOR_HOSTED_PLATFORM_H

#include <cstdint>

namespace HostedPlatform
{
template <class Context>
inline uintptr_t instructionPointer(const Context *context)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return context->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return context->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__x86_64__)
    return context->uc_mcontext.gregs[REG_RIP];
#else
#error Unsupported hosted ucontext platform
#endif
}

template <class Context>
inline uintptr_t stackPointer(const Context *context)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return context->uc_mcontext->__ss.__sp;
#elif defined(__APPLE__) && defined(__x86_64__)
    return context->uc_mcontext->__ss.__rsp;
#elif defined(__linux__) && defined(__x86_64__)
    return context->uc_mcontext.gregs[REG_RSP];
#else
#error Unsupported hosted ucontext platform
#endif
}

template <class Context>
inline uintptr_t basePointer(const Context *context)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return context->uc_mcontext->__ss.__fp;
#elif defined(__APPLE__) && defined(__x86_64__)
    return context->uc_mcontext->__ss.__rbp;
#elif defined(__linux__) && defined(__x86_64__)
    return context->uc_mcontext.gregs[REG_RBP];
#else
#error Unsupported hosted ucontext platform
#endif
}
}  // namespace HostedPlatform

#endif

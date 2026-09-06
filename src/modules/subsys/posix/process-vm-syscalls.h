/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_PROCESS_VM_SYSCALLS_H
#define POSIX_PROCESS_VM_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

struct iovec;
ssize_t posix_process_vm_readv(int pid, const iovec* local, size_t localCount, const iovec* remote,
                               size_t remoteCount, unsigned long flags);
ssize_t posix_process_vm_writev(int pid, const iovec* local, size_t localCount, const iovec* remote,
                                size_t remoteCount, unsigned long flags);

#if PEDIGREE_PROCESS_MEMORY_TESTS
class Thread;
using ProcessVmAfterFragmentHook = void (*)(size_t copied, void* context);
// Install/reset on the expected caller itself, and reset before it exits.
void setProcessVmAfterFragmentHookForTest(Thread* expectedCaller, ProcessVmAfterFragmentHook hook,
                                          void* context);
#endif

#endif

/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SANDBOX_STATE_H
#define POSIX_SANDBOX_STATE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class LandlockDomain;
class Thread;

EXPORTED_PUBLIC bool posix_no_new_privs();
EXPORTED_PUBLIC int posix_set_no_new_privs();
EXPORTED_PUBLIC int posix_seccomp_mode();
EXPORTED_PUBLIC int posix_seccomp(unsigned int operation, unsigned int flags, const void* args);
EXPORTED_PUBLIC void posix_sandbox_inherit(Thread& child, Thread& parent);
EXPORTED_PUBLIC SharedPointer<LandlockDomain> posix_sandbox_domain();
EXPORTED_PUBLIC bool posix_sandbox_restrict(const SharedPointer<LandlockDomain>& domain);

#endif

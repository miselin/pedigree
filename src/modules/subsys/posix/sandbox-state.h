/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SANDBOX_STATE_H
#define POSIX_SANDBOX_STATE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class LandlockDomain;
class IpcNamespace;
class PosixNetworkNamespace;
class PosixTaskCredentials;
class Thread;

EXPORTED_PUBLIC bool posix_no_new_privs();
EXPORTED_PUBLIC int posix_set_no_new_privs();
EXPORTED_PUBLIC int posix_seccomp_mode();
EXPORTED_PUBLIC int posix_seccomp(unsigned int operation, unsigned int flags, const void* args);
EXPORTED_PUBLIC void posix_sandbox_inherit(Thread& child, Thread& parent);
EXPORTED_PUBLIC bool posix_sandbox_prepare_namespaces(
    Thread& source, const SharedPointer<PosixTaskCredentials>& credentials,
    const SharedPointer<IpcNamespace>& ipc, const SharedPointer<PosixNetworkNamespace>& network,
    Thread::SecurityStateRef& prepared);
EXPORTED_PUBLIC SharedPointer<PosixTaskCredentials> posix_sandbox_credentials(Thread& thread);
EXPORTED_PUBLIC bool posix_sandbox_set_credentials(
    Thread& thread, const SharedPointer<PosixTaskCredentials>& credentials);
EXPORTED_PUBLIC SharedPointer<LandlockDomain> posix_sandbox_domain();
EXPORTED_PUBLIC bool posix_sandbox_restrict(const SharedPointer<LandlockDomain>& domain);

#endif

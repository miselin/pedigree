/* Copyright (c) 2026, Pedigree Developers. */
#include "ipc-namespace.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

namespace {
uint64_t nextIdentity = 0;
}

IpcNamespace::IpcNamespace(const UserNamespaceRef& owner)
    : m_Identity(__atomic_add_fetch(&nextIdentity, 1, __ATOMIC_RELAXED)), m_Owner(owner) {}

IpcNamespace::~IpcNamespace() {
  posix_sem_namespace_exit(m_Identity);
  posix_shm_namespace_exit(m_Identity);
  posix_msg_namespace_exit(m_Identity);
  posix_mqueue_namespace_exit(m_Identity);
}

SharedPointer<IpcNamespace> posix_ipc_namespace() {
  return posix_ipc_namespace(*Processor::information().getCurrentThread());
}

uint64_t posix_ipc_namespace_id() {
  auto space = posix_ipc_namespace();
  return space ? space->identity() : 0;
}

UserNamespaceRef posix_ipc_owner() {
  auto space = posix_ipc_namespace();
  return space ? space->owner() : UserNamespaceRef();
}

/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_IPC_NAMESPACE_H
#define POSIX_IPC_NAMESPACE_H

#include "user-namespace.h"

class Thread;

class EXPORTED_PUBLIC IpcNamespace {
 public:
  explicit IpcNamespace(const UserNamespaceRef& owner);
  ~IpcNamespace();
  uint64_t identity() const {
    return m_Identity;
  }
  const UserNamespaceRef& owner() const {
    return m_Owner;
  }

 private:
  const uint64_t m_Identity;
  const UserNamespaceRef m_Owner;
};

SharedPointer<IpcNamespace> posix_ipc_namespace(Thread& task);
SharedPointer<IpcNamespace> posix_ipc_namespace();
bool posix_set_ipc_namespace(Thread& task, const SharedPointer<IpcNamespace>& space);
uint64_t posix_ipc_namespace_id();
UserNamespaceRef posix_ipc_owner();
void posix_sem_namespace_exit(uint64_t identity);
void posix_shm_namespace_exit(uint64_t identity);
void posix_msg_namespace_exit(uint64_t identity);
void posix_mqueue_namespace_exit(uint64_t identity);

#endif

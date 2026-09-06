/* Copyright (c) 2026, Pedigree Developers. */
#include "advisory-lock-state.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/assert.h"

using namespace PosixAdvisory;

namespace {
uint64_t nextOwnerId = 0;
}

AdvisoryOwner::AdvisoryOwner(Kind kind)
    : m_Id(__atomic_add_fetch(&nextOwnerId, uint64_t(1), __ATOMIC_RELAXED)), m_Kind(kind) {
  assert(m_Id);
}

class PosixAdvisoryLocks {
 public:
  int apply(AdvisoryOwner& owner, Grant request, bool wait, PosixAdvisoryAdmission admission,
            void* context) {
    TerminationDeferral lifetime;
    if (!open(owner, request)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    Error::PosixError result = Error::NoError;
    size_t slot = MaximumWaiters;
    {
      LockGuard<Mutex> guard(m_Lock);
      bool converted = false;
      for (;;) {
        if (__atomic_load_n(&owner.m_State, __ATOMIC_ACQUIRE) == AdvisoryOwner::Closed) {
          result = Error::BadFileDescriptor;
          break;
        }
        if (admission && !admission(context)) {
          // Close already owns its release event. This request has installed
          // nothing, so removing grants here could erase a later valid lock.
          result = Error::BadFileDescriptor;
          break;
        }
        if (!converted && request.name == Namespace::Flock && request.type != Type::Unlock) {
          for (size_t i = 0; i < m_Table.count(); ++i) {
            const Grant& existing = m_Table.at(i);
            if (existing.owner == request.owner && existing.inode == request.inode &&
                existing.name == Namespace::Flock && existing.type != request.type) {
              Grant unlock = request;
              unlock.type = Type::Unlock;
              const PosixAdvisory::Result released = m_Table.apply(unlock);
              assert(released == PosixAdvisory::Result::Success);
              m_Changed.broadcast();
              break;
            }
          }
          converted = true;
        }
        const PosixAdvisory::Result applied = m_Table.apply(request);
        if (applied == PosixAdvisory::Result::Success) {
          m_Changed.broadcast();
          break;
        }
        if (applied == PosixAdvisory::Result::Full) {
          result = Error::NoLocksAvailable;
          break;
        }
        if (!wait) {
          result = Error::NoMoreProcesses;
          break;
        }
        if (m_Table.wouldDeadlock(request, m_Waiters, MaximumWaiters)) {
          result = Error::Deadlock;
          break;
        }
        if (slot == MaximumWaiters) {
          for (size_t i = 0; i < MaximumWaiters; ++i) {
            if (!m_Waiters[i]) {
              slot = i;
              m_Waiters[i] = &request;
              break;
            }
          }
          if (slot == MaximumWaiters) {
            result = Error::NoLocksAvailable;
            break;
          }
        }
        ConditionVariable::Error error = ConditionVariable::NoError;
        if (!m_Changed.wait(m_Lock, error)) {
          assert(ConditionVariable::mutexAcquired(error));
          result = Error::Interrupted;
          break;
        }
      }
      // Closing an owner can retire its slot before this stack resumes.
      if (slot != MaximumWaiters && m_Waiters[slot] == &request)
        m_Waiters[slot] = nullptr;
    }
    if (result != Error::NoError) {
      syscallError(result);
      return -1;
    }
    return 0;
  }

  int query(AdvisoryOwner& owner, Grant request, Grant& conflict, bool& found) {
    TerminationDeferral lifetime;
    if (!open(owner, request)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    LockGuard<Mutex> guard(m_Lock);
    if (__atomic_load_n(&owner.m_State, __ATOMIC_ACQUIRE) == AdvisoryOwner::Closed) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    found = m_Table.query(request, conflict);
    return 0;
  }

  void descriptorClosed(AdvisoryOwner& owner, uintptr_t inode) {
    if (!inode || __atomic_load_n(&owner.m_State, __ATOMIC_ACQUIRE) != AdvisoryOwner::Open)
      return;
    TerminationDeferral lifetime;
    LockGuard<Mutex> guard(m_Lock);
    m_Table.removeOwner(owner.m_Id, inode);
    // A closed original fd must also wake a request that had no grant yet.
    m_Changed.broadcast();
  }

  void ownerClosed(AdvisoryOwner& owner) {
    TerminationDeferral lifetime;
    const uint8_t previous =
        __atomic_exchange_n(&owner.m_State, AdvisoryOwner::Closed, __ATOMIC_ACQ_REL);
    if (previous != AdvisoryOwner::Open)
      return;
    LockGuard<Mutex> guard(m_Lock);
    m_Table.removeOwner(owner.m_Id);
    for (auto& waiter : m_Waiters) {
      if (waiter && waiter->owner == owner.m_Id)
        waiter = nullptr;
    }
    m_Changed.broadcast();
  }

 private:
  bool open(AdvisoryOwner& owner, Grant& request) {
    uint8_t expected = AdvisoryOwner::Unused;
    __atomic_compare_exchange_n(&owner.m_State, &expected, AdvisoryOwner::Open, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (expected == AdvisoryOwner::Closed)
      return false;
    request.owner = owner.m_Id;
    request.kind = owner.m_Kind;
    if (owner.m_Kind == AdvisoryOwner::Kind::OpenDescription)
      request.pid = -1;
    return true;
  }

  Mutex m_Lock;
  ConditionVariable m_Changed;
  Grant m_First[MaximumGrants];
  Grant m_Second[MaximumGrants];
  Table m_Table{m_First, m_Second, MaximumGrants};
  const Grant* m_Waiters[MaximumWaiters] = {};
};

namespace {
PosixAdvisoryLocks locks;
}

void posix_advisory_descriptor_closed(AdvisoryOwner& owner, uintptr_t inodeIdentity) {
  locks.descriptorClosed(owner, inodeIdentity);
}

void posix_advisory_owner_closed(AdvisoryOwner& owner) {
  locks.ownerClosed(owner);
}

int posix_advisory_apply(AdvisoryOwner& owner, Grant request, bool wait,
                         PosixAdvisoryAdmission admission, void* context) {
  return locks.apply(owner, request, wait, admission, context);
}

int posix_advisory_query(AdvisoryOwner& owner, Grant request, Grant& conflict, bool& found) {
  return locks.query(owner, request, conflict, found);
}

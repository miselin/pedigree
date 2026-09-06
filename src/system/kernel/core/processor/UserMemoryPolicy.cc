/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/processor/UserMemoryPolicy.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

UserMemoryOperation::UserMemoryOperation(VirtualAddressSpace& space)
    : m_EventDeferral(),
      m_TerminationDeferral(),
      m_Policy(space.userMemoryPolicy()),
      m_Privileged(false) {
  if (m_Policy) {
    m_Policy->enterOperation();
    m_Privileged = m_Policy->callerHasMemoryLockPrivilege();
  }
}

UserMemoryOperation::~UserMemoryOperation() {
  if (m_Policy)
    m_Policy->leaveOperation();
}

/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_ADVISORY_LOCK_STATE_H
#define POSIX_ADVISORY_LOCK_STATE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

#include "advisory-lock-table.h"

class EXPORTED_PUBLIC AdvisoryOwner {
 public:
  using Kind = PosixAdvisory::OwnerKind;
  explicit AdvisoryOwner(Kind kind);
  ~AdvisoryOwner() = default;
  AdvisoryOwner(const AdvisoryOwner&) = delete;
  AdvisoryOwner& operator=(const AdvisoryOwner&) = delete;

 private:
  friend class PosixAdvisoryLocks;
  enum State : uint8_t { Unused, Open, Closed };
  const uint64_t m_Id;
  const Kind m_Kind;
  uint8_t m_State = Unused;
};

void posix_advisory_descriptor_closed(AdvisoryOwner& owner, uintptr_t inodeIdentity);
void posix_advisory_owner_closed(AdvisoryOwner& owner);

// Admission is only the short fd/OFD identity check; it cannot take an OFD lock.
using PosixAdvisoryAdmission = bool (*)(void* context);
int posix_advisory_apply(AdvisoryOwner& owner, PosixAdvisory::Grant request, bool wait,
                         PosixAdvisoryAdmission admission = nullptr, void* context = nullptr);
int posix_advisory_query(AdvisoryOwner& owner, PosixAdvisory::Grant request,
                         PosixAdvisory::Grant& conflict, bool& found);

#endif

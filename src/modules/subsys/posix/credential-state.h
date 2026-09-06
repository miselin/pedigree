#ifndef POSIX_CREDENTIAL_STATE_H
#define POSIX_CREDENTIAL_STATE_H
#include "pedigree/kernel/process/FilesystemCredentials.h"

namespace PosixCredentials {
struct Snapshot {
  uint32_t ruid = 0, euid = 0, suid = 0;
  uint32_t rgid = 0, egid = 0, sgid = 0;
  uint32_t groups[FilesystemCredentials::MaximumGroups] = {};
  size_t groupCount = 0;
  uint64_t generation = 0;
  bool dumpable = true;
};
enum class Change { SetUid, SetGid, SetReUid, SetReGid, SetResUid, SetResGid };
enum class Status { Success, Invalid, Denied };

inline Status prepare(const Snapshot& old, Change change, uint32_t first, uint32_t second,
                      uint32_t third, uint32_t oldFs, Snapshot& next, uint32_t& nextFs) {
  const bool group =
      change == Change::SetGid || change == Change::SetReGid || change == Change::SetResGid;
  const bool single = change == Change::SetUid || change == Change::SetGid;
  const bool pair = change == Change::SetReUid || change == Change::SetReGid;
  const bool privileged = old.euid == 0;
  const uint32_t real = group ? old.rgid : old.ruid;
  const uint32_t effective = group ? old.egid : old.euid;
  const uint32_t saved = group ? old.sgid : old.suid;
  next = old;
  nextFs = oldFs;
  uint32_t r = real, e = effective, s = saved;
  const auto allowed = [&](uint32_t id) {
    return id == UINT32_MAX || id == real || id == effective || id == saved || privileged;
  };
  if (single) {
    if (first == UINT32_MAX)
      return Status::Invalid;
    if (privileged)
      r = s = first;
    else if (first != real && first != saved)
      return Status::Denied;
    e = first;
  } else if (pair) {
    if (first != UINT32_MAX && first != real && first != effective && !privileged)
      return Status::Denied;
    if (!allowed(second))
      return Status::Denied;
    if (first != UINT32_MAX)
      r = first;
    if (second != UINT32_MAX)
      e = second;
    if (first != UINT32_MAX || (second != UINT32_MAX && second != real))
      s = e;
  } else {
    if ((first == UINT32_MAX || first == real) &&
        (second == UINT32_MAX || (second == effective && second == oldFs)) &&
        (third == UINT32_MAX || third == saved))
      return Status::Success;
    if (!allowed(first) || !allowed(second) || !allowed(third))
      return Status::Denied;
    if (first != UINT32_MAX)
      r = first;
    if (second != UINT32_MAX)
      e = second;
    if (third != UINT32_MAX)
      s = third;
  }
  nextFs = e;
  if (group) {
    next.rgid = r;
    next.egid = e;
    next.sgid = s;
  } else {
    next.ruid = r;
    next.euid = e;
    next.suid = s;
  }
  if (e != effective || nextFs != oldFs)
    next.dumpable = false;
  if (r != real || e != effective || s != saved || nextFs != oldFs)
    ++next.generation;
  return Status::Success;
}
}  // namespace PosixCredentials
#endif

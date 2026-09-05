#ifndef POSIX_IPC_COMMON_H
#define POSIX_IPC_COMMON_H

#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Vector.h"

namespace PosixIpc {
struct Permission {
  int32_t key;
  uint32_t uid, gid, cuid, cgid;
  uint32_t mode, sequence, padding;
  uint64_t unused[2];
};
static_assert(sizeof(Permission) == 48, "Linux amd64 ipc_perm layout");

inline Process* process() {
  return Processor::information().getCurrentThread()->getParent();
}

inline void initialize(Permission& permission, int key, unsigned mode, unsigned sequence) {
  Process* current = process();
  permission = {};
  permission.key = key;
  permission.uid = permission.cuid = current->getEffectiveUserId();
  permission.gid = permission.cgid = current->getEffectiveGroupId();
  permission.mode = mode & 0777;
  permission.sequence = sequence;
}

inline bool owner(const Permission& permission) {
  const int64_t uid = process()->getEffectiveUserId();
  return uid == 0 || uid == permission.uid || uid == permission.cuid;
}

inline bool allowed(const Permission& permission, unsigned requestedMode) {
  Process* current = process();
  const int64_t uid = current->getEffectiveUserId();
  if (!requestedMode || uid == 0) {
    return true;
  }
  unsigned shift = 0;
  if (uid == permission.uid || uid == permission.cuid) {
    shift = 6;
  } else {
    const int64_t gid = current->getEffectiveGroupId();
    bool group = gid == permission.gid || gid == permission.cgid;
    if (!group) {
      Vector<int64_t> groups;
      current->getSupplementalGroupIds(groups);
      for (size_t i = 0; i < groups.count(); ++i) {
        if (groups[i] == permission.gid || groups[i] == permission.cgid) {
          group = true;
          break;
        }
      }
    }
    if (group) {
      shift = 3;
    }
  }
  return (((permission.mode >> shift) & requestedMode) == requestedMode);
}
}  // namespace PosixIpc
#endif

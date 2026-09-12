/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_LINUX_CLONE_ABI_H
#define PEDIGREE_POSIX_LINUX_CLONE_ABI_H

#include <stddef.h>
#include <stdint.h>

struct LinuxCloneArgs {
  uint64_t flags;
  uint64_t pidfd;
  uint64_t child_tid;
  uint64_t parent_tid;
  uint64_t exit_signal;
  uint64_t stack;
  uint64_t stack_size;
  uint64_t tls;
  uint64_t set_tid;
  uint64_t set_tid_size;
  uint64_t cgroup;
};

namespace LinuxCloneAbi {
constexpr size_t Version0Size = 64;
constexpr size_t Version1Size = 80;
constexpr size_t Version2Size = 88;
constexpr size_t MaximumSize = 4096;
constexpr uint64_t ClearSighand = 1ULL << 32;
}  // namespace LinuxCloneAbi

static_assert(sizeof(LinuxCloneArgs) == LinuxCloneAbi::Version2Size,
              "Linux amd64 clone_args must remain 88 bytes");
static_assert(offsetof(LinuxCloneArgs, set_tid) == LinuxCloneAbi::Version0Size &&
                  offsetof(LinuxCloneArgs, cgroup) == LinuxCloneAbi::Version1Size,
              "Linux clone_args version boundaries changed");
static_assert(offsetof(LinuxCloneArgs, child_tid) == 16 &&
                  offsetof(LinuxCloneArgs, parent_tid) == 24 &&
                  offsetof(LinuxCloneArgs, exit_signal) == 32 &&
                  offsetof(LinuxCloneArgs, stack) == 40 &&
                  offsetof(LinuxCloneArgs, stack_size) == 48 && offsetof(LinuxCloneArgs, tls) == 56,
              "Linux clone_args field offsets changed");

#endif

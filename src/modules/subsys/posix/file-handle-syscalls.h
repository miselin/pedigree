/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_FILE_HANDLE_SYSCALLS_H
#define POSIX_FILE_HANDLE_SYSCALLS_H

#include "pedigree/kernel/process/Process.h"

#include "PosixSubsystem.h"
#include "modules/system/vfs/VFS.h"

class PosixHandleTarget {
 public:
  // Mount admission must outlive destruction of every retained File owner.
  VFS::MountOperation mount;
  DescriptorLease descriptor;
  Process::FileContextLease cwd;
  Directory::ChildLease pathLease;
  File* file = nullptr;

  bool resolve(int dirfd, const char* path, bool follow, bool allowEmpty,
               bool nullAsDescriptor = false);
};

int posix_handle_error(FileHandleStatus status);
bool posix_effective_root();
int posix_name_to_handle_at(int dirfd, const char* path, void* handle, int* mountId, int flags);
int posix_open_by_handle_at(int mountfd, const void* handle, int flags);

#endif

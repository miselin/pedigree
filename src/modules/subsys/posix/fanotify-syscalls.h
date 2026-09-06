/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_FANOTIFY_SYSCALLS_H
#define POSIX_FANOTIFY_SYSCALLS_H

#include "pedigree/kernel/process/Readiness.h"

#include "descriptor-read.h"
#include "modules/system/vfs/FileHandle.h"
#include "modules/system/vfs/VFS.h"

class FanotifyState;
class FanotifyQueue;

class FanotifyInstance {
 public:
  static SharedPointer<FanotifyInstance> create();
  ~FanotifyInstance();
  int changeMark(File&, const VFS::MountOperation&, const FileHandle&, const FileSystemId&,
                 uint64_t mask, bool remove);
  int flushMarks();
  void lastDescriptorClosed();
  ssize_t readToUser(void*, size_t count, bool canBlock);
  ssize_t readWithCopy(size_t count, bool canBlock, PosixDescriptorReadCopy, void*);
  ReadyMask queryReady();
  ReadinessSource* readinessSource();
  int queuedMetadataBytes();

 private:
  FanotifyInstance();
  void reapMarks();
  FanotifyState* m_State = nullptr;
  bool m_Quota = false;
};

int posix_fanotify_init(unsigned flags, unsigned eventFlags);
int posix_fanotify_mark(int fd, unsigned flags, uint64_t mask, int dirfd, const char* path);
#endif

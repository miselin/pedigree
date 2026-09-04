/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef INOTIFY_SYSCALLS_H
#define INOTIFY_SYSCALLS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/processor/types.h"

class File;
class InotifyState;

namespace LinuxInotify {
constexpr uint32_t Access = 0x00000001;
constexpr uint32_t Modify = 0x00000002;
constexpr uint32_t Attributes = 0x00000004;
constexpr uint32_t CloseWrite = 0x00000008;
constexpr uint32_t CloseNoWrite = 0x00000010;
constexpr uint32_t Open = 0x00000020;
constexpr uint32_t MovedFrom = 0x00000040;
constexpr uint32_t MovedTo = 0x00000080;
constexpr uint32_t Create = 0x00000100;
constexpr uint32_t Delete = 0x00000200;
constexpr uint32_t DeleteSelf = 0x00000400;
constexpr uint32_t MoveSelf = 0x00000800;
constexpr uint32_t AllEvents = 0x00000FFF;
constexpr uint32_t Unmount = 0x00002000;
constexpr uint32_t QueueOverflow = 0x00004000;
constexpr uint32_t Ignored = 0x00008000;
constexpr uint32_t OnlyDirectory = 0x01000000;
constexpr uint32_t DontFollow = 0x02000000;
constexpr uint32_t ExcludeUnlinked = 0x04000000;
constexpr uint32_t MaskCreate = 0x10000000;
constexpr uint32_t MaskAdd = 0x20000000;
constexpr uint32_t IsDirectory = 0x40000000;
constexpr uint32_t OneShot = 0x80000000;
constexpr uint32_t AllBits = AllEvents | Unmount | QueueOverflow | Ignored | OnlyDirectory |
                             DontFollow | ExcludeUnlinked | MaskCreate | MaskAdd | IsDirectory |
                             OneShot;

constexpr int NonBlock = 0x00000800;
constexpr int CloseOnExec = 0x00080000;
}  // namespace LinuxInotify

struct LinuxInotifyEvent {
  int32_t wd;
  uint32_t mask;
  uint32_t cookie;
  uint32_t len;
};
static_assert(sizeof(LinuxInotifyEvent) == 16,
              "Linux x86-64 inotify_event header must be 16 bytes");

/** One Linux inotify object, shared by aliases of an open file description. */
class EXPORTED_PUBLIC InotifyInstance final : public ReadinessSource {
 public:
  InotifyInstance();
  ~InotifyInstance() override;

  int addWatch(File* target, uint32_t mask);
  int removeWatch(int wd);
  int readEvents(uint8_t* buffer, size_t length, bool canBlock);
  int readEventsToUser(uint8_t* buffer, size_t length, bool canBlock);

  ReadyMask queryReady();
  ReadinessGenerations readinessGenerations() override;

  /** Internal queue-to-source readiness bridge. */
  void eventsQueued();
  void lastDescriptorClosed();

 private:
  InotifyInstance(const InotifyInstance&) = delete;
  InotifyInstance& operator=(const InotifyInstance&) = delete;

  void reapInactiveWatches();

  InotifyState* m_State;
};

int posix_inotify_init();
int posix_inotify_init1(int flags);
int posix_inotify_add_watch(int fd, const char* pathname, uint32_t mask);
int posix_inotify_rm_watch(int fd, int wd);

#endif

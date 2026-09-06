/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_FANOTIFY_QUEUE_H
#define POSIX_FANOTIFY_QUEUE_H

#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "modules/system/vfs/FileHandle.h"

struct FanotifyRecord {
  uint32_t mountId = 0;
  FileHandle handle;
  FileSystemId fsid;
  uint64_t mask = 0;
  uint32_t producer = 0;

  size_t encodedSize() const;
  void encode(void* buffer) const;
  bool sameTarget(const FanotifyRecord&) const;
};

/** Preallocated event storage owns identity snapshots, without live File references. */
class FanotifyQueue final : public ReadinessSource {
 public:
  static constexpr size_t MaximumEvents = 256;
  static constexpr size_t MaximumRecordSize = 24 + 12 + 8 + 128;
  enum class Take { Ready, Empty, Closed, TooSmall, Interrupted };

  FanotifyQueue();
  ~FanotifyQueue() override;
  bool valid() const;
  void enqueue(const FanotifyRecord&);
  Take take(FanotifyRecord&, size_t capacity, bool canBlock);
  void close();
  ReadyMask queryReady();
  ReadinessGenerations readinessGenerations() override;
  int queuedMetadataBytes();

 private:
  Mutex m_Lock;
  ConditionVariable m_Readers;
  UniqueArray<FanotifyRecord> m_Records;
  size_t m_Head = 0;
  size_t m_Count = 0;
  bool m_OverflowQueued = false;
  bool m_Closed = false;
  ReadinessGenerations m_Generations;
};
#endif

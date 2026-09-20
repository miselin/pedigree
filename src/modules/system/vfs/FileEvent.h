/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef PEDIGREE_VFS_FILEEVENT_H
#define PEDIGREE_VFS_FILEEVENT_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/StringView.h"

using FileEventMask = uint32_t;

namespace FileEvents {
enum Event : FileEventMask {
  None = 0,
  Access = 1U << 0,
  Modify = 1U << 1,
  Attributes = 1U << 2,
  CloseWrite = 1U << 3,
  CloseNoWrite = 1U << 4,
  Open = 1U << 5,
  Created = 1U << 6,
  Removed = 1U << 7,
  DeletedSelf = 1U << 8,
  SourceRetired = 1U << 9,
};
}  // namespace FileEvents

struct FileEvent {
  FileEvent(FileEventMask eventMask, const StringView& eventName, bool eventTargetIsDirectory,
            uint32_t producer = 0)
      : mask(eventMask),
        name(eventName),
        targetIsDirectory(eventTargetIsDirectory),
        producerPid(producer) {}

  FileEventMask mask;
  /** Valid only for the duration of FileEventObserver::fileEvent(). */
  StringView name;
  bool targetIsDirectory;
  uint32_t producerPid;
};

class FileEventState;
class FileEventTarget;

class EXPORTED_PUBLIC FileEventObserver {
 public:
  virtual ~FileEventObserver() = default;

  /**
   * Called synchronously at the VFS mutation linearization point. Observers
   * must only snapshot or enqueue the payload: they must not re-enter VFS
   * mutation or reset their own subscription from this callback.
   */
  virtual void fileEvent(const FileEvent& event) = 0;
};

/**
 * Owns and drains one exact registration with a FileEventSource. reset() must
 * not be called by the subscription's own FileEventObserver::fileEvent().
 */
class EXPORTED_PUBLIC FileEventSubscription {
 public:
  FileEventSubscription();
  FileEventSubscription(FileEventSubscription&& other) noexcept;
  ~FileEventSubscription();

  FileEventSubscription& operator=(FileEventSubscription&& other) noexcept;

  explicit operator bool() const;
  void reset();

 private:
  friend class FileEventSource;

  FileEventSubscription(const FileEventSubscription&) = delete;
  FileEventSubscription& operator=(const FileEventSubscription&) = delete;

  SharedPointer<FileEventState> m_State;
  SharedPointer<FileEventTarget> m_Target;
  SharedPointer<FileEventObserver> m_Observer;
};

/** A lifetime-safe, payload-carrying VFS mutation event source. */
class EXPORTED_PUBLIC FileEventSource {
 public:
  FileEventSource();
  virtual ~FileEventSource();

  MUST_USE_RESULT bool subscribeFileEvents(FileEventMask interest,
                                           const SharedPointer<FileEventObserver>& observer,
                                           FileEventSubscription& subscription);

 protected:
  /** Conservative hints; delivery still checks admission under the source lock. */
  static bool anyFileEventObservers();
  bool hasFileEventObservers(FileEventMask mask) const;
  /** Delivers synchronously; callers may still hold the mutation's VFS lock. */
  void notifyFileEvent(const FileEvent& event);
  /** Atomically closes subscription admission and delivers a final event. */
  void notifyFinalFileEvent(const FileEvent& event);
  /** Closes admission and emits a terminal event without draining older callbacks. */
  void beginFinalFileEvent(const FileEvent& event);
  void drainFileEvents();
  void closeFileEvents();

 private:
  FileEventSource(const FileEventSource&) = delete;
  FileEventSource& operator=(const FileEventSource&) = delete;

  SharedPointer<FileEventState> m_FileEventState;
};

/** Shared backing source, independent of any one namespace wrapper. */
class EXPORTED_PUBLIC InodeEventSource : public FileEventSource {
 public:
  void publish(const FileEvent& event);
  void beginRetirement();
  /** Called after beginRetirement, outside backend and namespace locks. */
  void finishRetirement();
};

#endif

/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"

#include "Ext2File.h"
#include "ext2.h"

FileHandleStatus Ext2File::subscribeInodeEvents(FileEventMask mask,
                                                const SharedPointer<FileEventObserver>& observer,
                                                FileEventSubscription& subscription) {
  subscription.reset();
  if (!observer || !mask) {
    return FileHandleStatus::Invalid;
  }
  LockGuard<Mutex> metadata(m_State->writebackLock);
  if (m_State->orphan || !LITTLE_TO_HOST16(m_pInode->i_links_count)) {
    return FileHandleStatus::Stale;
  }
  if (!m_State->inodeEvents.subscribeFileEvents(mask | FileEvents::SourceRetired, observer,
                                                subscription)) {
    return FileHandleStatus::NoMemory;
  }
  return FileHandleStatus::Success;
}

void Ext2File::publishInodeEvent(const FileEvent& event) {
  m_State->inodeEvents.publish(event);
}

void Ext2File::finishInodeRetirement() {
  m_State->inodeEvents.finishRetirement();
}

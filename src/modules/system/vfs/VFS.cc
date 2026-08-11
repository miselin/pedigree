/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "VFS.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Directory.h"
#include "File.h"

#ifndef VFS_STANDALONE
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "modules/Module.h"
#endif

class Disk;

/// \todo Figure out a way to clean up files after deletion. Directory::remove()
///       is not the right place to do this. There needs to be a way to add a
///       File to some sort of queue that cleans it up once it hits refcount
///       zero or something like that.

VFS VFS::m_Instance;

VFS& VFS::instance() {
  return m_Instance;
}

VFS::VFS()
    : m_pRootFilesystem(nullptr),
      m_Mounts(),
      m_ProbeCallbacks(),
      m_MountCallbacks()
#if THREADS
      ,
      m_CallbackLock(),
      m_NextCallbackSequence(1),
      m_pActiveCallbacks(nullptr),
      m_CallbacksClosing(false)
#endif
{
}

VFS::~VFS() {
#if THREADS
  m_CallbackLock.acquire();
  m_CallbacksClosing = true;
  if (m_pActiveCallbacks) {
    m_CallbackLock.release();
    FATAL("VFS destroyed with an active callback invocation");
  }
  for (auto item : m_ProbeCallbacks) {
    if (item->state.inFlight || item->state.removers) {
      m_CallbackLock.release();
      FATAL("VFS destroyed before probe callback retirement completed");
    }
  }
  for (auto item : m_MountCallbacks) {
    if (item->state.inFlight || item->state.removers) {
      m_CallbackLock.release();
      FATAL("VFS destroyed before mount callback retirement completed");
    }
  }
  m_CallbackLock.release();
#endif

  // Wipe out probe callbacks we know about.
  for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
    delete *it;
  }
  for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
    delete *it;
  }

  // Unmount each registered filesystem exactly once.
  for (auto it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
    delete it.value();
    delete it.key();
  }
}

bool VFS::mount(Disk* pDisk, String& stableName, Filesystem** pMountedFs) {
#if THREADS
  TerminationDeferral dispatchDeferral;
  Thread* current = Processor::information().getCurrentThread();
  void* owner =
      current ? static_cast<void*>(current) : static_cast<void*>(&Processor::information());
  size_t boundary = 0;
  m_CallbackLock.acquire();
  boundary = m_NextCallbackSequence;
  m_CallbackLock.release();

  size_t afterSequence = 0;
  while (true) {
    ActiveInvocation invocation = {nullptr, owner, nullptr};
    ProbeCallbackItem* item = acquireProbeCallback(afterSequence, boundary, invocation);
    if (!item) {
      break;
    }

    Filesystem* pFs = item->callback(pDisk);
    if (pFs) {
      m_CallbackLock.acquire();
      // This is the publication commit point. Retirement which closes the
      // entry first rejects the result while the provider remains pinned;
      // commit which wins keeps the pin through every use below.
      const bool committed = item->state.enabled && !m_CallbacksClosing;
      m_CallbackLock.release();
      if (!committed) {
        // The provider must still be pinned while its rejected result is
        // destroyed; removal can only return after finishCallback below.
        delete pFs;
        finishCallback(&item->state, invocation);
        continue;
      }

      if (stableName.length() == 0) {
        stableName = pFs->getVolumeLabel();
      }
      stableName = registerFilesystem(pFs, stableName);
      dispatchMountCallbacks(owner);

      if (pMountedFs) {
        *pMountedFs = pFs;
      }

      NOTICE("mounted filesystem '" << stableName << "'");

      finishCallback(&item->state, invocation);
      return true;
    }

    finishCallback(&item->state, invocation);
  }
#else
  for (List<ProbeCallbackItem*>::Iterator it = m_ProbeCallbacks.begin();
       it != m_ProbeCallbacks.end(); it++) {
    Filesystem* pFs = (*it)->callback(pDisk);
    if (pFs) {
      if (stableName.length() == 0) {
        stableName = pFs->getVolumeLabel();
      }
      stableName = registerFilesystem(pFs, stableName);

      for (List<MountCallbackItem*>::Iterator it2 = m_MountCallbacks.begin();
           it2 != m_MountCallbacks.end(); it2++) {
        (*it2)->callback();
      }

      if (pMountedFs) {
        *pMountedFs = pFs;
      }

      NOTICE("mounted filesystem '" << stableName << "'");

      return true;
    }
  }
#endif
  return false;
}

String VFS::registerFilesystem(Filesystem* pFs, const String& preferredStableName) {
  if (!pFs) {
    return String();
  }

  MountInfo* existing = m_Mounts.lookup(pFs);
  if (existing) {
    return existing->stableName;
  }

  String stableName = getUniqueStableName(preferredStableName);
  NormalStaticString path;
  path += "/media/";
  path += stableName;
  MountInfo* info = new MountInfo(stableName, String(path));
  m_Mounts.insert(pFs, info);

  if (m_pRootFilesystem) {
    attachFilesystem(pFs, info->path);
  }

  return stableName;
}

void VFS::unregisterFilesystem(Filesystem* pFs, bool canDelete) {
  if (!pFs)
    return;

  MountInfo* info = m_Mounts.lookup(pFs);
  if (info && m_pRootFilesystem && pFs != m_pRootFilesystem) {
    File* point = find(info->path);
    if (point && point->isDirectory()) {
      Directory::fromFile(point)->setReparsePoint(nullptr);
    }
  }

  if (pFs == m_pRootFilesystem) {
    m_pRootFilesystem = nullptr;
  }

  delete info;
  m_Mounts.remove(pFs);

  if (canDelete) {
    delete pFs;
  }
}

bool VFS::setRootFilesystem(Filesystem* pFs) {
  if (pFs && !m_Mounts.lookup(pFs)) {
    registerFilesystem(pFs, pFs->getVolumeLabel());
  }

  m_pRootFilesystem = pFs;
  attachRegisteredFilesystems();
  return !pFs || m_pRootFilesystem == pFs;
}

bool VFS::getMountPath(Filesystem* pFs, String& path) const {
  MountInfo* info = m_Mounts.lookup(pFs);
  if (!info) {
    return false;
  }

  path = info->path;
  return true;
}

Filesystem* VFS::getFilesystemAt(const String& path) const {
  for (MountTable::Iterator it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
    if (it.value()->path == path) {
      return it.key();
    }
  }

  return nullptr;
}

File* VFS::find(const String& path, File* pStartNode) {
  // NOTICE("find: " << path);

  File* pResult = 0;

  pStartNode = resolveStartNode(path, pStartNode);
  if (pStartNode) {
    pResult = pStartNode->getFilesystem()->find(path.view(), pStartNode);
  }

  // NOTICE("find: " << path << " -> " << pResult);
  return pResult;
}

void VFS::addProbeCallback(Filesystem::ProbeCallback callback) {
  if (!callback) {
    FATAL("VFS cannot register a null probe callback");
  }

  ProbeCallbackItem* item = new ProbeCallbackItem(callback);
#if THREADS
  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS probe callback registered during teardown");
  }
  for (auto registered : m_ProbeCallbacks) {
    if (registered->callback == callback) {
      if (!registered->state.draining) {
        registered->state.enabled = true;
      }
      m_CallbackLock.release();
      delete item;
      return;
    }
  }
  if (m_NextCallbackSequence == static_cast<size_t>(-1)) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS callback sequence exhausted");
  }
  item->state.sequence = m_NextCallbackSequence++;
  item->state.debugAddress = reinterpret_cast<uintptr_t>(callback);
  m_ProbeCallbacks.pushBack(item);
  m_CallbackLock.release();
#else
  for (auto registered : m_ProbeCallbacks) {
    if (registered->callback == callback) {
      delete item;
      return;
    }
  }
  m_ProbeCallbacks.pushBack(item);
#endif
}

bool VFS::removeProbeCallback(Filesystem::ProbeCallback callback) {
#if THREADS
  if (!callback) {
    return false;
  }

  TerminationDeferral removalDeferral;
  Thread* current = Processor::information().getCurrentThread();
  const bool canYield =
      current && Processor::executionContext() == ExecutionContext::WaitableThread;
  void* owner =
      current ? static_cast<void*>(current) : static_cast<void*>(&Processor::information());
  ProbeCallbackItem* item = nullptr;
  bool callbackContext = false;

  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    return false;
  }
  for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
    if ((*it)->callback != callback) {
      continue;
    }

    item = *it;
    item->state.enabled = false;
    callbackContext = isCallbackInvocation(owner);
    if (!callbackContext && canYield) {
      item->state.draining = true;
      ++item->state.removers;
    }
    break;
  }
  m_CallbackLock.release();

  if (!item) {
    return false;
  }
  if (callbackContext || !canYield) {
    return false;
  }

  drainProbeCallback(item);
  return true;
#else
  for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
    ProbeCallbackItem* item = *it;
    if (item->callback == callback) {
      m_ProbeCallbacks.erase(it);
      delete item;
      return true;
    }
  }
  return false;
#endif
}

void VFS::addMountCallback(MountCallback callback) {
  if (!callback) {
    FATAL("VFS cannot register a null mount callback");
  }

  MountCallbackItem* item = new MountCallbackItem(callback);
#if THREADS
  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS mount callback registered during teardown");
  }
  for (auto registered : m_MountCallbacks) {
    if (registered->callback == callback) {
      if (!registered->state.draining) {
        registered->state.enabled = true;
      }
      m_CallbackLock.release();
      delete item;
      return;
    }
  }
  if (m_NextCallbackSequence == static_cast<size_t>(-1)) {
    m_CallbackLock.release();
    delete item;
    FATAL("VFS callback sequence exhausted");
  }
  item->state.sequence = m_NextCallbackSequence++;
  item->state.debugAddress = reinterpret_cast<uintptr_t>(callback);
  m_MountCallbacks.pushBack(item);
  m_CallbackLock.release();
#else
  for (auto registered : m_MountCallbacks) {
    if (registered->callback == callback) {
      delete item;
      return;
    }
  }
  m_MountCallbacks.pushBack(item);
#endif
}

bool VFS::removeMountCallback(MountCallback callback) {
#if THREADS
  if (!callback) {
    return false;
  }

  TerminationDeferral removalDeferral;
  Thread* current = Processor::information().getCurrentThread();
  const bool canYield =
      current && Processor::executionContext() == ExecutionContext::WaitableThread;
  void* owner =
      current ? static_cast<void*>(current) : static_cast<void*>(&Processor::information());
  MountCallbackItem* item = nullptr;
  bool callbackContext = false;

  m_CallbackLock.acquire();
  if (m_CallbacksClosing) {
    m_CallbackLock.release();
    return false;
  }
  for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
    if ((*it)->callback != callback) {
      continue;
    }

    item = *it;
    item->state.enabled = false;
    callbackContext = isCallbackInvocation(owner);
    if (!callbackContext && canYield) {
      item->state.draining = true;
      ++item->state.removers;
    }
    break;
  }
  m_CallbackLock.release();

  if (!item) {
    return false;
  }
  if (callbackContext || !canYield) {
    return false;
  }

  drainMountCallback(item);
  return true;
#else
  for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
    MountCallbackItem* item = *it;
    if (item->callback == callback) {
      m_MountCallbacks.erase(it);
      delete item;
      return true;
    }
  }
  return false;
#endif
}

#if THREADS
VFS::ProbeCallbackItem* VFS::acquireProbeCallback(size_t& afterSequence, size_t boundary,
                                                  ActiveInvocation& invocation) {
  ProbeCallbackItem* item = nullptr;
  m_CallbackLock.acquire();
  if (!m_CallbacksClosing) {
    for (auto candidate : m_ProbeCallbacks) {
      if (candidate->state.sequence <= afterSequence) {
        continue;
      }
      if (candidate->state.sequence >= boundary) {
        break;
      }

      afterSequence = candidate->state.sequence;
      if (!candidate->state.enabled) {
        continue;
      }

      item = candidate;
      invocation.state = &item->state;
      ++item->state.inFlight;
      invocation.next = m_pActiveCallbacks;
      m_pActiveCallbacks = &invocation;
      break;
    }
  }
  m_CallbackLock.release();
  return item;
}

VFS::MountCallbackItem* VFS::acquireMountCallback(size_t& afterSequence, size_t boundary,
                                                  ActiveInvocation& invocation) {
  MountCallbackItem* item = nullptr;
  m_CallbackLock.acquire();
  if (!m_CallbacksClosing) {
    for (auto candidate : m_MountCallbacks) {
      if (candidate->state.sequence <= afterSequence) {
        continue;
      }
      if (candidate->state.sequence >= boundary) {
        break;
      }

      afterSequence = candidate->state.sequence;
      if (!candidate->state.enabled) {
        continue;
      }

      item = candidate;
      invocation.state = &item->state;
      ++item->state.inFlight;
      invocation.next = m_pActiveCallbacks;
      m_pActiveCallbacks = &invocation;
      break;
    }
  }
  m_CallbackLock.release();
  return item;
}

void VFS::finishCallback(CallbackState* state, ActiveInvocation& invocation) {
  auto completionGuard = state->drainWaiters.acquire();
  bool wakeDrainers = false;

  m_CallbackLock.acquire();
  ActiveInvocation** link = &m_pActiveCallbacks;
  while (*link && *link != &invocation) {
    link = &((*link)->next);
  }
  if (!*link) {
    m_CallbackLock.release();
    FATAL("VFS lost an active callback invocation");
  }
  *link = invocation.next;

  if (!state->inFlight) {
    m_CallbackLock.release();
    FATAL("VFS callback pin underflow");
  }
  --state->inFlight;
  wakeDrainers = !state->inFlight && state->draining;
  m_CallbackLock.release();

  if (wakeDrainers) {
    completionGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(state));
  }
}

void VFS::drainProbeCallback(ProbeCallbackItem* item) {
  while (true) {
    bool complete = false;
    {
      auto waitGuard = item->state.drainWaiters.acquire();
      m_CallbackLock.acquire();
      if (!item->state.inFlight) {
        complete = true;
        m_CallbackLock.release();
      } else {
        m_CallbackLock.release();
        const WaitQueue::WakeReason reason = waitGuard.waitForCompletion(
            WaitQueue::Channel(&item->state), Thread::CallbackDrain, item->state.debugAddress);
        (void)reason;
      }
    }
    if (complete) {
      break;
    }
  }

  bool deleteItem = false;
  m_CallbackLock.acquire();
  if (!item->state.removers) {
    m_CallbackLock.release();
    FATAL("VFS probe callback remover underflow");
  }
  --item->state.removers;
  if (!item->state.removers) {
    for (auto it = m_ProbeCallbacks.begin(); it != m_ProbeCallbacks.end(); ++it) {
      if (*it == item) {
        m_ProbeCallbacks.erase(it);
        deleteItem = true;
        break;
      }
    }
  }
  m_CallbackLock.release();

  if (deleteItem) {
    delete item;
  }
}

void VFS::drainMountCallback(MountCallbackItem* item) {
  while (true) {
    bool complete = false;
    {
      auto waitGuard = item->state.drainWaiters.acquire();
      m_CallbackLock.acquire();
      if (!item->state.inFlight) {
        complete = true;
        m_CallbackLock.release();
      } else {
        m_CallbackLock.release();
        const WaitQueue::WakeReason reason = waitGuard.waitForCompletion(
            WaitQueue::Channel(&item->state), Thread::CallbackDrain, item->state.debugAddress);
        (void)reason;
      }
    }
    if (complete) {
      break;
    }
  }

  bool deleteItem = false;
  m_CallbackLock.acquire();
  if (!item->state.removers) {
    m_CallbackLock.release();
    FATAL("VFS mount callback remover underflow");
  }
  --item->state.removers;
  if (!item->state.removers) {
    for (auto it = m_MountCallbacks.begin(); it != m_MountCallbacks.end(); ++it) {
      if (*it == item) {
        m_MountCallbacks.erase(it);
        deleteItem = true;
        break;
      }
    }
  }
  m_CallbackLock.release();

  if (deleteItem) {
    delete item;
  }
}

void VFS::dispatchMountCallbacks(void* owner) {
  size_t boundary = 0;
  m_CallbackLock.acquire();
  boundary = m_NextCallbackSequence;
  m_CallbackLock.release();

  size_t afterSequence = 0;
  while (true) {
    ActiveInvocation invocation = {nullptr, owner, nullptr};
    MountCallbackItem* item = acquireMountCallback(afterSequence, boundary, invocation);
    if (!item) {
      break;
    }

    item->callback();
    finishCallback(&item->state, invocation);
  }
}

bool VFS::isCallbackInvocation(void* owner) const {
  for (ActiveInvocation* invocation = m_pActiveCallbacks; invocation;
       invocation = invocation->next) {
    if (invocation->owner == owner) {
      return true;
    }
  }
  return false;
}
#endif

bool VFS::createFile(const String& path, uint32_t mask, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->createFile(path, mask, pStartNode);
}

bool VFS::createDirectory(const String& path, uint32_t mask, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  if (!pStartNode) {
    NOTICE("no start node found");
    return false;
  }

  return pStartNode->getFilesystem()->createDirectory(path, mask, pStartNode);
}

bool VFS::createSymlink(const String& path, const String& value, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->createSymlink(path, value, pStartNode);
}

bool VFS::createLink(const String& path, File* target, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->createLink(path, target, pStartNode);
}

bool VFS::remove(const String& path, File* pStartNode) {
  pStartNode = resolveStartNode(path, pStartNode);
  return pStartNode && pStartNode->getFilesystem()->remove(path, pStartNode);
}

bool VFS::checkAccess(File* pFile, bool bRead, bool bWrite, bool bExecute) {
#ifdef VFS_STANDALONE
  // We don't check permissions on standalone builds of the VFS.
  return true;
#else
  if (!pFile) {
    // The error for a null file is not EPERM or EACCESS.
    return true;
  }

  Process* pProcess = Processor::information().getCurrentThread()->getParent();

  int64_t fuid = pFile->getUid();
  int64_t fgid = pFile->getGid();

  int64_t processUid = pProcess->getEffectiveUserId();
  if (processUid < 0) {
    processUid = pProcess->getUserId();
  }

  int64_t processGid = pProcess->getEffectiveGroupId();
  if (processGid < 0) {
    processGid = pProcess->getGroupId();
  }

  uint32_t check = 0;
  uint32_t permissions = pFile->getPermissions();
  uint32_t needed = (bRead ? FILE_UR : 0) | (bWrite ? FILE_UW : 0) | (bExecute ? FILE_UX : 0);

  if (processUid == 0) {
    if (!bExecute || (permissions & (FILE_UX | FILE_GX | FILE_OX))) {
      return true;
    }
  } else if (fuid == processUid) {
    check = (permissions >> FILE_UBITS) & 0x7;
  } else {
    bool inFileGroup = fgid == processGid;

    if (!inFileGroup) {
      Vector<int64_t> supplementalGroups;
      pProcess->getSupplementalGroupIds(supplementalGroups);

      for (auto it : supplementalGroups) {
        if (it == fgid) {
          inFileGroup = true;
          break;
        }
      }
    }

    check = (permissions >> (inFileGroup ? FILE_GBITS : FILE_OBITS)) & 0x7;
  }

  if ((check & needed) != needed) {
    NOTICE("VFS::checkAccess: needed " << Oct << needed << ", check was " << check);
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }

  return true;
#endif
}

void VFS::trackFile(File* pFile) {
  size_t n = m_TrackedFiles.lookup(pFile);
  ++n;
  m_TrackedFiles.insert(pFile, n);
}

bool VFS::untrackFile(File* pFile, bool destroy) {
  size_t n = m_TrackedFiles.lookup(pFile);
  if ((n == 0) || ((n - 1) == 0)) {
    m_TrackedFiles.remove(pFile);
    if (destroy) {
      delete pFile;
    }
    return true;
  } else {
    m_TrackedFiles.insert(pFile, n - 1);
  }

  return false;
}

String VFS::getUniqueStableName(const String& preferredName) const {
  NormalStaticString safeName;
  for (size_t i = 0; i < preferredName.length(); ++i) {
    char c = preferredName[i];
    bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
    safeName.append(allowed ? c : '-');
  }

  String base(safeName, safeName.length());
  if (!base.length() || base == "." || base == "..") {
    base.assign("filesystem");
  }

  size_t suffix = 1;
  while (true) {
    NormalStaticString candidateBuffer;
    candidateBuffer += base;
    if (suffix > 1) {
      candidateBuffer += "-";
      candidateBuffer.append(suffix);
    }
    String candidate(candidateBuffer, candidateBuffer.length());

    bool exists = false;
    for (MountTable::Iterator it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
      if (it.value()->stableName == candidate) {
        exists = true;
        break;
      }
    }

    if (!exists) {
      return candidate;
    }

    ++suffix;
  }
}

File* VFS::resolveStartNode(const String& path, File* pStartNode) {
  if (!path.length() || path[0] != '/') {
    return pStartNode;
  }

  return m_pRootFilesystem ? m_pRootFilesystem->getRoot() : nullptr;
}

bool VFS::attachFilesystem(Filesystem* pFs, const String& path) {
  if (!m_pRootFilesystem || !pFs || pFs == m_pRootFilesystem) {
    return pFs == m_pRootFilesystem;
  }

  if (!find(String("/media"))) {
    createDirectory(String("/media"), 0755);
  }
  if (!find(path)) {
    createDirectory(path, 0755);
  }

  File* point = find(path);
  if (!point || !point->isDirectory()) {
    ERROR("VFS: cannot attach filesystem at " << path);
    return false;
  }

  Directory::fromFile(point)->setReparsePoint(Directory::fromFile(pFs->getRoot()));
  NOTICE("VFS: attached filesystem at " << path);
  return true;
}

void VFS::attachRegisteredFilesystems() {
  if (!m_pRootFilesystem) {
    return;
  }

  for (MountTable::Iterator it = m_Mounts.begin(); it != m_Mounts.end(); ++it) {
    MountInfo* info = it.value();
    if (it.key() == m_pRootFilesystem) {
      info->path.assign("/");
      continue;
    }

    NormalStaticString path;
    path += "/media/";
    path += info->stableName;
    info->path.assign(path, path.length());
    attachFilesystem(it.key(), info->path);
  }
}

#ifndef VFS_STANDALONE
static bool initVFS() {
  return true;
}

static void destroyVFS() {}

MODULE_INFO("vfs", &initVFS, &destroyVFS, "users");
#endif

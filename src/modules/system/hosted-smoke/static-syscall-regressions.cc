/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/Module.h"
#include "modules/subsys/pedigree-c/pedigreecSyscallNumbers.h"
#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/subsys/posix/epoll-syscalls.h"
#include "modules/subsys/posix/eventfd-syscalls.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/subsys/posix/poll-syscalls.h"
#include "modules/subsys/posix/select-syscalls.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Pipe.h"
#include "modules/system/vfs/VFS.h"
#undef PEDIGREE_INIT_SIGRET
#undef PEDIGREE_SIGRET
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/utility.h"

#include <fcntl.h>
#include <limits.h>
#include <sched.h>

#include "modules/subsys/posix/syscalls/posixSyscallNumbers.h"

extern void system_reset();
extern "C" bool posixDuplicateInitRollbackPreservesProcessForTest(Process* processIdentity);
extern "C" void posixSetCloneBeforeStartHookForTest(void (*hook)(Thread*, size_t, void*),
                                                    void* context);
extern "C" unsigned int posixSelectProjectionForTest(short revents, bool checkRead, bool checkWrite,
                                                     bool checkExceptional);
extern "C" int posixSelectTimeoutMillisecondsForTest(timeval timeout);
extern bool runHostedEventFdRegressions(Process* process);
extern bool runHostedScalarIoRegressions(Process* process);
extern bool runHostedUsercopyRegressions(Process* process);

namespace {
constexpr size_t HostedAttempts = 10000;
constexpr int PollCloseReuseTimeoutMilliseconds = 5000;
constexpr uint64_t EpollInitialData = 0x1111222233334444ULL;
constexpr uint64_t EpollLevelData = 0x3333444455556666ULL;
constexpr uint64_t EpollOneShotData = 0x5555666677778888ULL;
constexpr uint64_t EpollRearmedData = 0x9999AAAABBBBCCCCULL;
constexpr uint64_t EpollAliasData = 0xDDDDEEEEFFFF0001ULL;
constexpr uint64_t EpollEventFdData = 0x123456789ABCDEF0ULL;
constexpr uint64_t EpollReorderedData = 0x0DDBA11C0FFEE123ULL;
constexpr uint64_t EpollFifoData = 0xF1F0C105ED6E0001ULL;
size_t g_RuntimePinnedLifecycleCalls = 0;

struct TerminalBlockedHandlerContext {
  TerminalBlockedHandlerContext()
      : blocker(0, true),
        thread(nullptr),
        hookEntered(0),
        exitStaged(0),
        releasedByTermination(0),
        unexpectedRelease(0),
        syscallReturned(0) {}

  Semaphore blocker;
  Thread* thread;
  Atomic<size_t> hookEntered;
  Atomic<size_t> exitStaged;
  Atomic<size_t> releasedByTermination;
  Atomic<size_t> unexpectedRelease;
  Atomic<size_t> syscallReturned;
};

TerminalBlockedHandlerContext* g_TerminalBlockedHandlerContext = nullptr;

int terminalCreatedFixtureEntry(void*) {
  FATAL("HOSTED-SYSCALL-TEST: FAIL posix-terminal-drain-created-entry-ran");
  return 0;
}

void terminalBlockedHandlerPin(Service_t service, SyscallHandler*) {
  TerminalBlockedHandlerContext* context = g_TerminalBlockedHandlerContext;
  Thread* current = Processor::information().getCurrentThread();
  if (!context || service != posix || current != context->thread) {
    return;
  }

  context->hookEntered += 1;
  if (SyscallManager::instance().requestProcessExit(73)) {
    context->exitStaged += 1;
  }

  const bool acquired = context->blocker.acquire();
  if (!acquired && current->getUnwindState() == Thread::TerminateThread) {
    context->releasedByTermination += 1;
  } else {
    context->unexpectedRelease += 1;
  }
}

int terminalBlockedHandlerEntry(void* parameter) {
  TerminalBlockedHandlerContext* context =
      reinterpret_cast<TerminalBlockedHandlerContext*>(parameter);
  SyscallManager::instance().syscall(posix, POSIX_GETPID);
  context->syscallReturned += 1;
  return 1;
}

void runtimePinnedLifecycleProbe() {
  ++g_RuntimePinnedLifecycleCalls;
}

class DescriptorRetirementProbe : public FileDescriptor {
 public:
  explicit DescriptorRetirementProbe(Atomic<size_t>& destructions)
      : FileDescriptor(), m_Destructions(destructions) {}

  ~DescriptorRetirementProbe() override {
    m_Destructions += 1;
  }

 private:
  Atomic<size_t>& m_Destructions;
};

class EstablishedAliasFileProbe : public File {
 public:
  explicit EstablishedAliasFileProbe(Atomic<size_t>& destructions)
      : File(), m_Destructions(destructions) {}

  ~EstablishedAliasFileProbe() override {
    m_Destructions += 1;
  }

 private:
  Atomic<size_t>& m_Destructions;
};

void repairAliasFileProbe(EstablishedAliasFileProbe* file, Atomic<size_t>& destructions) {
  if (!destructions) {
    VFS::instance().untrackFile(file);
  }
  if (!destructions) {
    delete file;
  }
}

class RetainedLookupDirectory;

class RetainedLookupFilesystem final : public Filesystem {
 public:
  using RemoveHook = bool (*)(File*, File*, void*);

  RetainedLookupFilesystem()
      : m_Root(nullptr),
        m_Label("retained-lookup-test"),
        m_RemoveHook(nullptr),
        m_RemoveHookContext(nullptr) {}

  void setRoot(File* root) {
    m_Root = root;
  }

  void setRemoveHook(RemoveHook hook, void* context) {
    m_RemoveHook = hook;
    m_RemoveHookContext = context;
  }

  bool initialise(Disk*) override {
    return true;
  }

  File* getRoot() const override {
    return m_Root;
  }

  const String& getVolumeLabel() const override {
    return m_Label;
  }

 protected:
  bool createFile(File*, const String&, uint32_t) override {
    return false;
  }

  bool createDirectory(File*, const String&, uint32_t) override {
    return false;
  }

  bool createSymlink(File*, const String&, const String&) override {
    return false;
  }

  bool removeNode(File* parent, const String&, File* file) override {
    return m_RemoveHook ? m_RemoveHook(parent, file, m_RemoveHookContext) : false;
  }

 private:
  File* m_Root;
  String m_Label;
  RemoveHook m_RemoveHook;
  void* m_RemoveHookContext;
};

class RetainedLookupFile final : public File {
 public:
  RetainedLookupFile(const String& name, Filesystem* filesystem, File* parent,
                     Atomic<size_t>& destructions, RetainedLookupDirectory* lockProbe = nullptr,
                     Atomic<size_t>* lockAvailable = nullptr)
      : File(name, 0, 0, 0, 0, filesystem, 0, parent),
        m_Destructions(destructions),
        m_LockProbe(lockProbe),
        m_LockAvailable(lockAvailable) {}

  ~RetainedLookupFile() override;

 private:
  Atomic<size_t>& m_Destructions;
  RetainedLookupDirectory* m_LockProbe;
  Atomic<size_t>* m_LockAvailable;
};

class RetainedLookupDirectory final : public Directory {
 public:
  RetainedLookupDirectory(const String& name, Filesystem* filesystem,
                          Atomic<size_t>* destructions = nullptr)
      : Directory(name, 0, 0, 0, 0, filesystem, 0, nullptr),
        m_LazyTarget(nullptr),
        m_Conversions(nullptr),
        m_FirstConversionEntered(nullptr),
        m_FirstConversionRelease(nullptr),
        m_SecondConversionEntered(nullptr),
        m_SecondConversionRelease(nullptr),
        m_Destructions(destructions) {}

  ~RetainedLookupDirectory() override {
    if (m_Destructions) {
      *m_Destructions += 1;
    }
  }

  void publish(const String& name, File* file) {
    addDirectoryEntry(name, file);
  }

  bool removePublished(const String& name, File* expected) {
    return removeDirectoryEntry(HashedStringView(name), expected);
  }

  const void* namespaceLockAddress() {
    return static_cast<const void*>(&namespaceMutationLock());
  }

  bool publishEphemeral(File* file) {
    return addEphemeralFile(file) == AddStatus::Added;
  }

  void publishLazy(const String& name, File* file, Atomic<size_t>& conversions,
                   Atomic<size_t>& firstEntered, Semaphore& firstRelease,
                   Atomic<size_t>& secondEntered, Semaphore& secondRelease) {
    m_LazyTarget = file;
    m_Conversions = &conversions;
    m_FirstConversionEntered = &firstEntered;
    m_FirstConversionRelease = &firstRelease;
    m_SecondConversionEntered = &secondEntered;
    m_SecondConversionRelease = &secondRelease;

    DirectoryEntryMetadata metadata;
    metadata.pDirectory = this;
    metadata.filename = name;
    addDirectoryEntry(name, pedigree_std::move(metadata));
  }

  void publishFailedLazy(const String& name, Atomic<size_t>& conversions) {
    m_LazyTarget = nullptr;
    m_Conversions = &conversions;
    m_FirstConversionEntered = nullptr;
    m_FirstConversionRelease = nullptr;
    m_SecondConversionEntered = nullptr;
    m_SecondConversionRelease = nullptr;

    DirectoryEntryMetadata metadata;
    metadata.pDirectory = this;
    metadata.filename = name;
    addDirectoryEntry(name, pedigree_std::move(metadata));
  }

 protected:
  File* convertToFile(const DirectoryEntryMetadata&) override {
    const size_t conversion = (*m_Conversions += 1);
    Atomic<size_t>* entered = nullptr;
    Semaphore* release = nullptr;
    if (conversion == static_cast<size_t>(1)) {
      entered = m_FirstConversionEntered;
      release = m_FirstConversionRelease;
    } else if (conversion == static_cast<size_t>(2)) {
      entered = m_SecondConversionEntered;
      release = m_SecondConversionRelease;
    }
    if (entered && release) {
      *entered += 1;
      const bool released = release->acquireForCompletion();
      (void)released;
    }
    return m_LazyTarget;
  }

 private:
  File* m_LazyTarget;
  Atomic<size_t>* m_Conversions;
  Atomic<size_t>* m_FirstConversionEntered;
  Semaphore* m_FirstConversionRelease;
  Atomic<size_t>* m_SecondConversionEntered;
  Semaphore* m_SecondConversionRelease;
  Atomic<size_t>* m_Destructions;
};

RetainedLookupFile::~RetainedLookupFile() {
  if (m_LockProbe && m_LockAvailable && m_LockProbe->tryCacheLockForHostedTest()) {
    *m_LockAvailable += 1;
  }
  m_Destructions += 1;
}

struct RetainedLookupHookContext {
  RetainedLookupHookContext(Directory* directory, File* file, bool pauseBefore, bool pauseAfter)
      : directory(directory),
        file(file),
        pauseBefore(pauseBefore),
        pauseAfter(pauseAfter),
        beforeRelease(0, false),
        afterRelease(0, false),
        beforeClaimed(0),
        beforeEntered(0),
        beforeReturned(0),
        beforeNullFile(0),
        afterClaimed(0),
        afterEntered(0),
        afterReturned(0) {}

  Directory* directory;
  File* file;
  bool pauseBefore;
  bool pauseAfter;
  Semaphore beforeRelease;
  Semaphore afterRelease;
  Atomic<size_t> beforeClaimed;
  Atomic<size_t> beforeEntered;
  Atomic<size_t> beforeReturned;
  Atomic<size_t> beforeNullFile;
  Atomic<size_t> afterClaimed;
  Atomic<size_t> afterEntered;
  Atomic<size_t> afterReturned;
};

struct RetainedLookupWorkerContext {
  RetainedLookupWorkerContext(Directory* directory, const String& name)
      : directory(directory), name(name), child(nullptr), result(false), returned(0) {}

  Directory* directory;
  String name;
  File* child;
  bool result;
  Atomic<size_t> returned;
};

struct DirectoryRemoveWorkerContext {
  DirectoryRemoveWorkerContext(Directory* directory, const String& name)
      : directory(directory), name(name), returned(0) {}

  Directory* directory;
  String name;
  Atomic<size_t> returned;
};

struct RetainedLookupReplacementWorkerContext {
  RetainedLookupReplacementWorkerContext(Directory* directory, const String& oldName,
                                         const String& replacementName, File* oldFile,
                                         File* replacementFile)
      : directory(directory),
        oldName(oldName),
        replacementName(replacementName),
        oldFile(oldFile),
        replacementFile(replacementFile),
        oldRetained(false),
        missingPreserved(false),
        replaced(false),
        returned(0) {}

  Directory* directory;
  String oldName;
  String replacementName;
  File* oldFile;
  File* replacementFile;
  bool oldRetained;
  bool missingPreserved;
  bool replaced;
  Atomic<size_t> returned;
};

Atomic<RetainedLookupHookContext*> g_RetainedLookupHookContext(nullptr);

void pauseRetainedLookup(Directory* directory, File* file, Directory::RetainedLookupPhase phase) {
  RetainedLookupHookContext* context = g_RetainedLookupHookContext;
  if (!context || context->directory != directory) {
    return;
  }

  const bool before = phase == Directory::RetainedLookupPhase::BeforeLookup;
  if ((before && file) || (!before && context->file != file)) {
    return;
  }
  const bool pause = before ? context->pauseBefore : context->pauseAfter;
  Atomic<size_t>& claimed = before ? context->beforeClaimed : context->afterClaimed;
  if (!pause || !claimed.compareAndSwap(0, 1)) {
    return;
  }

  Atomic<size_t>& entered = before ? context->beforeEntered : context->afterEntered;
  Atomic<size_t>& returned = before ? context->beforeReturned : context->afterReturned;
  Semaphore& release = before ? context->beforeRelease : context->afterRelease;
  if (before) {
    context->beforeNullFile += 1;
  }
  entered += 1;
  if (release.acquireForCompletion()) {
    returned += 1;
  }
}

int retainedLookupWorker(void* parameter) {
  RetainedLookupWorkerContext* context = reinterpret_cast<RetainedLookupWorkerContext*>(parameter);
  Directory::ChildLease child;
  context->result = context->directory->lookupRetained(HashedStringView(context->name), child);
  context->child = child.get();
  child.reset();
  context->returned += 1;
  return context->result ? 0 : 1;
}

int directoryRemoveWorker(void* parameter) {
  DirectoryRemoveWorkerContext* context =
      reinterpret_cast<DirectoryRemoveWorkerContext*>(parameter);
  context->directory->remove(HashedStringView(context->name));
  context->returned += 1;
  return 0;
}

int retainedLookupReplacementWorker(void* parameter) {
  RetainedLookupReplacementWorkerContext* context =
      reinterpret_cast<RetainedLookupReplacementWorkerContext*>(parameter);
  Directory::ChildLease child;
  context->oldRetained =
      context->directory->lookupRetained(HashedStringView(context->oldName), child) &&
      child.get() == context->oldFile;
  if (context->oldRetained) {
    context->directory->remove(HashedStringView(context->oldName));
    context->missingPreserved =
        !context->directory->lookupRetained(HashedStringView("retained-missing"), child) &&
        child.get() == context->oldFile;
  }
  if (context->missingPreserved) {
    context->replaced =
        context->directory->lookupRetained(HashedStringView(context->replacementName), child) &&
        child.get() == context->replacementFile;
  }
  child.reset();
  context->returned += 1;
  return context->oldRetained && context->missingPreserved && context->replaced ? 0 : 1;
}

bool waitForDirectoryLock(Thread* thread, const Directory& directory, Atomic<size_t>& returned) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (returned) {
      return false;
    }
    Thread::WaitDebugInfo info = {};
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        info.channelOwner == directory.cacheLockAddressForHostedTest() &&
        thread->getStatus() == Thread::Sleeping) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitForRetainedLookupPause(Thread* thread, Atomic<size_t>& entered, Semaphore& release,
                                Atomic<size_t>& returned) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (returned) {
      return false;
    }
    Thread::WaitDebugInfo info = {};
    if (entered == static_cast<size_t>(1) && thread->getWaitDebugInfo(info) && info.queue &&
        info.queued && info.channelOwner == &release && thread->getStatus() == Thread::Sleeping) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

size_t drainTrackedFileOwners(File* file, Atomic<size_t>& destructions) {
  for (size_t release = 1; release <= 8 && !destructions; ++release) {
    if (VFS::instance().untrackFile(file, false)) {
      delete file;
      return release;
    }
  }
  return 0;
}

bool directoryRetainedLookupRemoval(Process* kernelProcess) {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-removal-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> destructions(0);
  const String alias("retained-removal-alias");
  RetainedLookupFile* child = new RetainedLookupFile(String("retained-removal-child"), &filesystem,
                                                     &directory, destructions);
  directory.publish(alias, child);
  const bool firstEmergencyRetained = VFS::instance().retainTrackedFile(child);
  const bool secondEmergencyRetained =
      firstEmergencyRetained && VFS::instance().retainTrackedFile(child);

  RetainedLookupHookContext hook(&directory, child, true, true);
  RetainedLookupWorkerContext lookupContext(&directory, alias);
  DirectoryRemoveWorkerContext removeContext(&directory, alias);
  Thread* lookup =
      new Thread(kernelProcess, retainedLookupWorker, &lookupContext, nullptr, false, true, true);
  lookup->setName("hosted retained directory lookup");
  Thread* remover = nullptr;

  g_RetainedLookupHookContext = &hook;
  Directory::setRetainedLookupHookForHostedTest(pauseRetainedLookup);
  const bool lookupStarted = secondEmergencyRetained && lookup->start();
  const bool beforePaused =
      lookupStarted && waitForRetainedLookupPause(lookup, hook.beforeEntered, hook.beforeRelease,
                                                  lookupContext.returned);

  bool removeStarted = false;
  bool removeQueuedBefore = false;
  if (beforePaused) {
    remover = new Thread(kernelProcess, directoryRemoveWorker, &removeContext, nullptr, false, true,
                         true);
    remover->setName("hosted retained directory remover");
    removeStarted = remover->start();
    if (removeStarted) {
      removeQueuedBefore = waitForDirectoryLock(remover, directory, removeContext.returned);
    }
  }

  const bool removalStayedPendingBefore = removeQueuedBefore && !removeContext.returned;
  hook.beforeRelease.release();

  const bool afterPaused =
      lookupStarted && waitForRetainedLookupPause(lookup, hook.afterEntered, hook.afterRelease,
                                                  lookupContext.returned);
  const bool removeQueuedAfter = afterPaused && removeStarted &&
                                 waitForDirectoryLock(remover, directory, removeContext.returned);
  const bool removalStayedPendingAfter = removeQueuedAfter && !removeContext.returned;

  // Every blocking gate gets a rescue token before completion-safe joins.
  hook.beforeRelease.release();
  hook.afterRelease.release();
  const bool lookupJoined = lookupStarted && lookup->joinForCompletion();
  const bool removeJoined = removeStarted && remover->joinForCompletion();
  if (!lookupStarted) {
    delete lookup;
  }
  if (remover && !removeStarted) {
    delete remover;
  }
  Directory::setRetainedLookupHookForHostedTest(nullptr);
  g_RetainedLookupHookContext = nullptr;

  const bool lookupSucceeded = lookupContext.result && lookupContext.child == child;
  directory.remove(HashedStringView(alias));
  const size_t cleanupReleases = drainTrackedFileOwners(child, destructions);

  return firstEmergencyRetained && secondEmergencyRetained && lookupStarted && beforePaused &&
         removeStarted && removalStayedPendingBefore && afterPaused && removalStayedPendingAfter &&
         lookupJoined && removeJoined && hook.beforeReturned == static_cast<size_t>(1) &&
         hook.beforeNullFile == static_cast<size_t>(1) &&
         hook.afterReturned == static_cast<size_t>(1) && lookupSucceeded &&
         removeContext.returned == static_cast<size_t>(1) && cleanupReleases == 2 &&
         destructions == static_cast<size_t>(1);
}

bool directoryRetainedLazyLookup(Process* kernelProcess) {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-lazy-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> destructions(0);
  Atomic<size_t> conversions(0);
  Atomic<size_t> firstConversionEntered(0);
  Semaphore firstConversionRelease(0, false);
  Atomic<size_t> secondConversionEntered(0);
  Semaphore secondConversionRelease(0, false);
  const String alias("retained-lazy-alias");
  RetainedLookupFile* child =
      new RetainedLookupFile(String("retained-lazy-child"), &filesystem, &directory, destructions);
  directory.publishLazy(alias, child, conversions, firstConversionEntered, firstConversionRelease,
                        secondConversionEntered, secondConversionRelease);

  RetainedLookupWorkerContext firstContext(&directory, alias);
  RetainedLookupWorkerContext secondContext(&directory, alias);
  Thread* first =
      new Thread(kernelProcess, retainedLookupWorker, &firstContext, nullptr, false, true, true);
  first->setName("hosted first lazy retained lookup");
  Thread* second = nullptr;
  const bool firstStarted = first->start();

  bool firstPaused = false;
  for (size_t attempt = 0; attempt < HostedAttempts && firstStarted; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (firstConversionEntered == static_cast<size_t>(1) && first->getWaitDebugInfo(info) &&
        info.queue && info.queued && info.channelOwner == &firstConversionRelease &&
        first->getStatus() == Thread::Sleeping) {
      firstPaused = true;
      break;
    }
    Scheduler::instance().yield();
  }

  bool secondStarted = false;
  bool secondQueued = false;
  bool secondConverted = false;
  if (firstStarted && firstPaused) {
    second =
        new Thread(kernelProcess, retainedLookupWorker, &secondContext, nullptr, false, true, true);
    second->setName("hosted second lazy retained lookup");
    secondStarted = second->start();
    if (secondStarted) {
      for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
        Thread::WaitDebugInfo info = {};
        if (secondConversionEntered == static_cast<size_t>(1) && second->getWaitDebugInfo(info) &&
            info.queue && info.queued && info.channelOwner == &secondConversionRelease &&
            second->getStatus() == Thread::Sleeping) {
          secondConverted = true;
          break;
        }
        if (second->getWaitDebugInfo(info) && info.queue && info.queued &&
            info.channelOwner == directory.cacheLockAddressForHostedTest() &&
            second->getStatus() == Thread::Sleeping) {
          secondQueued = true;
          break;
        }
        if (secondContext.returned) {
          break;
        }
        Scheduler::instance().yield();
      }
    }
  }

  const bool singleConversionWhilePaused = secondQueued && !secondConverted &&
                                           !secondContext.returned &&
                                           conversions == static_cast<size_t>(1);

  bool secondFinishedBeforeFirstRelease = false;
  if (secondConverted) {
    secondConversionRelease.release();
    for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
      if (secondContext.returned) {
        secondFinishedBeforeFirstRelease = true;
        break;
      }
      Scheduler::instance().yield();
    }
  }

  // In an unlocked mutant, conversion two completes before conversion one is
  // allowed to write its LazyEvaluate result.
  firstConversionRelease.release();
  firstConversionRelease.release();
  secondConversionRelease.release();
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }
  if (second && !secondStarted) {
    delete second;
  }

  const bool lookupsSucceeded = firstContext.result && secondContext.result &&
                                firstContext.child == child && secondContext.child == child;
  directory.remove(HashedStringView(alias));
  const size_t cleanupReleases = drainTrackedFileOwners(child, destructions);
  if (!destructions) {
    delete child;
  }

  return firstStarted && firstPaused && secondStarted && singleConversionWhilePaused &&
         !secondFinishedBeforeFirstRelease && firstJoined && secondJoined && lookupsSucceeded &&
         conversions == static_cast<size_t>(1) && cleanupReleases == 0 &&
         destructions == static_cast<size_t>(1);
}

bool directoryRetainedLookupDisjoint(Process* kernelProcess) {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory firstDirectory(String("retained-disjoint-first"), &filesystem);
  RetainedLookupDirectory secondDirectory(String("retained-disjoint-second"), &filesystem);
  filesystem.setRoot(&firstDirectory);
  Atomic<size_t> destructions(0);
  const String firstAlias("retained-disjoint-first-alias");
  const String secondAlias("retained-disjoint-second-alias");
  RetainedLookupFile* firstChild = new RetainedLookupFile(
      String("retained-disjoint-first-child"), &filesystem, &firstDirectory, destructions);
  RetainedLookupFile* secondChild = new RetainedLookupFile(
      String("retained-disjoint-second-child"), &filesystem, &secondDirectory, destructions);
  firstDirectory.publish(firstAlias, firstChild);
  secondDirectory.publish(secondAlias, secondChild);

  RetainedLookupHookContext hook(&firstDirectory, firstChild, false, true);
  RetainedLookupWorkerContext firstContext(&firstDirectory, firstAlias);
  RetainedLookupWorkerContext secondContext(&secondDirectory, secondAlias);
  Thread* first =
      new Thread(kernelProcess, retainedLookupWorker, &firstContext, nullptr, false, true, true);
  first->setName("hosted blocked retained lookup");
  Thread* second = nullptr;

  g_RetainedLookupHookContext = &hook;
  Directory::setRetainedLookupHookForHostedTest(pauseRetainedLookup);
  const bool firstStarted = first->start();

  const bool firstPaused =
      firstStarted && waitForRetainedLookupPause(first, hook.afterEntered, hook.afterRelease,
                                                 firstContext.returned);

  bool secondStarted = false;
  bool secondFinishedWhilePaused = false;
  if (firstStarted && firstPaused) {
    second =
        new Thread(kernelProcess, retainedLookupWorker, &secondContext, nullptr, false, true, true);
    second->setName("hosted disjoint retained lookup");
    secondStarted = second->start();
    for (size_t attempt = 0; attempt < HostedAttempts && secondStarted; ++attempt) {
      if (secondContext.returned) {
        secondFinishedWhilePaused = true;
        break;
      }
      Scheduler::instance().yield();
    }
  }

  hook.beforeRelease.release();
  hook.afterRelease.release();
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }
  if (second && !secondStarted) {
    delete second;
  }
  Directory::setRetainedLookupHookForHostedTest(nullptr);
  g_RetainedLookupHookContext = nullptr;

  const bool lookupsSucceeded = firstContext.result && secondContext.result &&
                                firstContext.child == firstChild &&
                                secondContext.child == secondChild;
  firstDirectory.remove(HashedStringView(firstAlias));
  secondDirectory.remove(HashedStringView(secondAlias));

  return firstStarted && firstPaused && secondStarted && secondFinishedWhilePaused && firstJoined &&
         secondJoined && hook.beforeEntered == static_cast<size_t>(0) &&
         hook.afterReturned == static_cast<size_t>(1) && lookupsSucceeded &&
         destructions == static_cast<size_t>(2);
}

bool directoryRetainedLookupDeletion() {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-delete-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> destructions(0);
  Atomic<size_t> lockAvailable(0);
  const String alias("retained-delete-alias");
  RetainedLookupFile* child =
      new RetainedLookupFile(String("retained-delete-child"), &filesystem, &directory, destructions,
                             &directory, &lockAvailable);
  directory.publish(alias, child);
  directory.remove(HashedStringView(alias));
  return destructions == static_cast<size_t>(1) && lockAvailable == static_cast<size_t>(1);
}

bool directoryRetainedLookupReplacement(Process* kernelProcess) {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-replacement-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> oldDestructions(0);
  Atomic<size_t> oldLockAvailable(0);
  Atomic<size_t> replacementDestructions(0);
  const String oldAlias("retained-replacement-old-alias");
  const String replacementAlias("retained-replacement-new-alias");
  RetainedLookupFile* oldFile =
      new RetainedLookupFile(String("retained-replacement-old"), &filesystem, &directory,
                             oldDestructions, &directory, &oldLockAvailable);
  RetainedLookupFile* replacement = new RetainedLookupFile(
      String("retained-replacement-new"), &filesystem, &directory, replacementDestructions);
  directory.publish(oldAlias, oldFile);
  directory.publish(replacementAlias, replacement);
  const bool firstReplacementEmergency = VFS::instance().retainTrackedFile(replacement);
  const bool secondReplacementEmergency =
      firstReplacementEmergency && VFS::instance().retainTrackedFile(replacement);

  RetainedLookupHookContext hook(&directory, replacement, false, true);
  RetainedLookupReplacementWorkerContext workerContext(&directory, oldAlias, replacementAlias,
                                                       oldFile, replacement);
  Thread* worker = new Thread(kernelProcess, retainedLookupReplacementWorker, &workerContext,
                              nullptr, false, true, true);
  worker->setName("hosted retained lookup replacement");

  g_RetainedLookupHookContext = &hook;
  Directory::setRetainedLookupHookForHostedTest(pauseRetainedLookup);
  const bool workerStarted = secondReplacementEmergency && worker->start();
  const bool afterPaused =
      workerStarted && waitForRetainedLookupPause(worker, hook.afterEntered, hook.afterRelease,
                                                  workerContext.returned);
  const bool oldStayedAliveThroughRetain = afterPaused && !oldDestructions;

  hook.afterRelease.release();
  const bool workerJoined = workerStarted && worker->joinForCompletion();
  if (!workerStarted) {
    delete worker;
  }
  Directory::setRetainedLookupHookForHostedTest(nullptr);
  g_RetainedLookupHookContext = nullptr;

  directory.remove(HashedStringView(oldAlias));
  directory.remove(HashedStringView(replacementAlias));
  if (!oldDestructions) {
    drainTrackedFileOwners(oldFile, oldDestructions);
  }
  const size_t replacementCleanupReleases =
      drainTrackedFileOwners(replacement, replacementDestructions);

  return firstReplacementEmergency && secondReplacementEmergency && workerStarted && afterPaused &&
         oldStayedAliveThroughRetain && workerJoined && workerContext.oldRetained &&
         workerContext.missingPreserved && workerContext.replaced &&
         workerContext.returned == static_cast<size_t>(1) &&
         hook.afterReturned == static_cast<size_t>(1) &&
         oldDestructions == static_cast<size_t>(1) && oldLockAvailable == static_cast<size_t>(1) &&
         replacementCleanupReleases == 2 && replacementDestructions == static_cast<size_t>(1);
}

bool directoryRetainedFailedLazyLookup() {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-failed-lazy-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> conversions(0);
  const String alias("retained-failed-lazy-alias");

  // This seed makes any attempt to track a null conversion result observable.
  VFS::instance().trackFile(nullptr);
  directory.publishFailedLazy(alias, conversions);

  Directory::ChildLease first;
  Directory::ChildLease second;
  const bool firstFailed = !directory.lookupRetained(HashedStringView(alias), first);
  const bool secondFailed = !directory.lookupRetained(HashedStringView(alias), second);
  directory.remove(HashedStringView(alias));

  const bool seedWasFinal = VFS::instance().untrackFile(nullptr, false);
  bool mutantExtrasDrained = seedWasFinal;
  for (size_t release = 0; release < 4 && !mutantExtrasDrained; ++release) {
    mutantExtrasDrained = VFS::instance().untrackFile(nullptr, false);
  }

  return firstFailed && secondFailed && !first && !second &&
         conversions == static_cast<size_t>(2) && seedWasFinal && mutantExtrasDrained;
}

struct DirectoryMutationSerializationContext {
  DirectoryMutationSerializationContext(RetainedLookupDirectory* directory, const String& alias,
                                        RetainedLookupFile* oldChild,
                                        Atomic<size_t>& oldDestructions,
                                        RetainedLookupFile* replacement)
      : directory(directory),
        alias(alias),
        oldChild(oldChild),
        oldDestructions(oldDestructions),
        replacement(replacement),
        publisher(nullptr),
        publishStart(0),
        publishAttempted(0),
        publishReturned(0),
        callbacks(0),
        firstSawOld(false),
        oldStayedAliveAfterAliasRemoval(false),
        publisherBlocked(false) {}

  RetainedLookupDirectory* directory;
  String alias;
  RetainedLookupFile* oldChild;
  Atomic<size_t>& oldDestructions;
  RetainedLookupFile* replacement;
  Thread* publisher;
  Semaphore publishStart;
  Atomic<size_t> publishAttempted;
  Atomic<size_t> publishReturned;
  size_t callbacks;
  bool firstSawOld;
  bool oldStayedAliveAfterAliasRemoval;
  bool publisherBlocked;
};

int publishDuringDirectoryRemoval(void* opaque) {
  DirectoryMutationSerializationContext* context =
      reinterpret_cast<DirectoryMutationSerializationContext*>(opaque);
  if (!context->publishStart.acquireForCompletion()) {
    return 1;
  }
  context->publishAttempted += 1;
  context->directory->publish(context->alias, context->replacement);
  context->publishReturned += 1;
  return 0;
}

bool serializeDirectoryMutationRemove(File* parent, File* file, void* opaque) {
  DirectoryMutationSerializationContext* context =
      reinterpret_cast<DirectoryMutationSerializationContext*>(opaque);
  ++context->callbacks;
  context->firstSawOld =
      context->callbacks == 1 && parent == context->directory && file == context->oldChild;
  if (!context->firstSawOld ||
      !context->directory->removePublished(context->alias, context->oldChild)) {
    return false;
  }

  context->oldStayedAliveAfterAliasRemoval = context->oldDestructions == static_cast<size_t>(0);
  context->publishStart.release();
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context->publishAttempted == static_cast<size_t>(1) &&
        context->publisher->getWaitDebugInfo(info) && info.queue && info.queued &&
        info.channelOwner == context->directory->namespaceLockAddress() &&
        context->publisher->getStatus() == Thread::Sleeping) {
      context->publisherBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }
  return context->oldStayedAliveAfterAliasRemoval && context->publisherBlocked;
}

bool directoryMutationSerializesSameKeyReplacement(Process* kernelProcess) {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-mutation-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> oldDestructions(0);
  Atomic<size_t> replacementDestructions(0);
  const String alias("retained-mutation-alias");
  RetainedLookupFile* oldChild = new RetainedLookupFile(String("retained-mutation-old"),
                                                        &filesystem, &directory, oldDestructions);
  RetainedLookupFile* replacement = new RetainedLookupFile(
      String("retained-mutation-replacement"), &filesystem, &directory, replacementDestructions);
  directory.publish(alias, oldChild);

  DirectoryMutationSerializationContext context(&directory, alias, oldChild, oldDestructions,
                                                replacement);
  Thread* publisher = new Thread(kernelProcess, publishDuringDirectoryRemoval, &context, nullptr,
                                 false, true, true);
  publisher->setName("hosted directory mutation publisher");
  context.publisher = publisher;
  const bool publisherStarted = publisher->start();

  filesystem.setRemoveHook(serializeDirectoryMutationRemove, &context);
  const bool removed = publisherStarted && filesystem.remove(&directory, oldChild);
  filesystem.setRemoveHook(nullptr, nullptr);
  if (!removed) {
    context.publishStart.release();
  }
  const bool publisherJoined = publisherStarted && publisher->joinForCompletion();
  if (!publisherStarted) {
    delete publisher;
  }

  Directory::ChildLease retainedReplacement;
  const bool replacementVisible =
      directory.lookupRetained(HashedStringView(alias), retainedReplacement) &&
      retainedReplacement.get() == replacement;

  directory.remove(HashedStringView(alias));
  retainedReplacement.reset();
  if (!oldDestructions) {
    drainTrackedFileOwners(oldChild, oldDestructions);
  }
  if (!replacementDestructions) {
    delete replacement;
  }

  return publisherStarted && removed && publisherJoined && context.callbacks == 1 &&
         context.firstSawOld && context.oldStayedAliveAfterAliasRemoval &&
         context.publisherBlocked && context.publishAttempted == static_cast<size_t>(1) &&
         context.publishReturned == static_cast<size_t>(1) && replacementVisible &&
         oldDestructions == static_cast<size_t>(1) &&
         replacementDestructions == static_cast<size_t>(1);
}

bool directoryRetainedDuplicateEphemeral() {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-ephemeral-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> originalDestructions(0);
  Atomic<size_t> duplicateDestructions(0);
  const String name("retained-ephemeral-child");
  RetainedLookupFile* original =
      new RetainedLookupFile(name, &filesystem, &directory, originalDestructions);
  RetainedLookupFile* duplicate =
      new RetainedLookupFile(name, &filesystem, &directory, duplicateDestructions);
  directory.publish(name, original);

  const bool duplicateAdded = directory.publishEphemeral(duplicate);
  Directory::ChildLease visible;
  const bool originalVisible =
      directory.lookupRetained(HashedStringView(name), visible) && visible.get() == original;
  const bool duplicateWasTracked = VFS::instance().untrackFile(duplicate, false);

  directory.remove(HashedStringView(name));
  visible.reset();
  const size_t originalCleanupReleases = drainTrackedFileOwners(original, originalDestructions);
  if (!duplicateDestructions) {
    delete duplicate;
  }

  return !duplicateAdded && originalVisible && !duplicateWasTracked &&
         originalCleanupReleases == 0 && originalDestructions == static_cast<size_t>(1) &&
         duplicateDestructions == static_cast<size_t>(1);
}

bool directoryRetainedLookupAtomicity(Process* kernelProcess) {
  const bool removal = directoryRetainedLookupRemoval(kernelProcess);
  const bool lazy = directoryRetainedLazyLookup(kernelProcess);
  const bool failedLazy = directoryRetainedFailedLazyLookup();
  if (!removal || !lazy || !failedLazy) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL directory-retained-lookup-atomicity: "
        "lookup did not linearise child retention with removal and lazy evaluation");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS directory-retained-lookup-atomicity");
  return true;
}

bool directoryRetainedLookupLifecycle(Process* kernelProcess) {
  const bool disjoint = directoryRetainedLookupDisjoint(kernelProcess);
  const bool deletion = directoryRetainedLookupDeletion();
  const bool replacement = directoryRetainedLookupReplacement(kernelProcess);
  const bool mutationSerialization = directoryMutationSerializesSameKeyReplacement(kernelProcess);
  const bool duplicateEphemeral = directoryRetainedDuplicateEphemeral();
  if (!disjoint || !deletion || !replacement || !mutationSerialization || !duplicateEphemeral) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL directory-retained-lookup-lifecycle: "
        "directory locks were global or child destruction ran while locked");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS directory-retained-lookup-lifecycle");
  return true;
}

struct TrackedFileRetainHookContext {
  explicit TrackedFileRetainHookContext(File* target)
      : target(target), release(0, false), claimed(0), entered(0), returned(0) {}

  File* target;
  Semaphore release;
  Atomic<size_t> claimed;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

struct TrackedFileRetainWorkerContext {
  explicit TrackedFileRetainWorkerContext(File* file) : file(file), retained(0), returned(0) {}

  File* file;
  Atomic<size_t> retained;
  Atomic<size_t> returned;
};

Atomic<TrackedFileRetainHookContext*> g_TrackedFileRetainHookContext(nullptr);

void pauseFirstTrackedFileRetain(File* file) {
  TrackedFileRetainHookContext* context = g_TrackedFileRetainHookContext;
  if (!context || context->target != file || !context->claimed.compareAndSwap(0, 1)) {
    return;
  }

  context->entered += 1;
  if (context->release.acquireForCompletion()) {
    context->returned += 1;
  }
}

int retainTrackedFileWorker(void* parameter) {
  TrackedFileRetainWorkerContext* context =
      reinterpret_cast<TrackedFileRetainWorkerContext*>(parameter);
  context->retained = VFS::instance().retainTrackedFile(context->file) ? 1 : 0;
  context->returned += 1;
  return context->retained ? 0 : 1;
}

bool establishedAliasRetainSerialization(Process* kernelProcess) {
  Atomic<size_t> destructions(0);
  EstablishedAliasFileProbe* file = new EstablishedAliasFileProbe(destructions);
  VFS::instance().trackFile(file);

  TrackedFileRetainHookContext hook(file);
  TrackedFileRetainWorkerContext workerAContext(file);
  TrackedFileRetainWorkerContext workerBContext(file);
  Thread* workerA = new Thread(kernelProcess, retainTrackedFileWorker, &workerAContext, nullptr,
                               false, true, true);
  Thread* workerB = nullptr;
  workerA->setName("hosted VFS retain serializer A");

  g_TrackedFileRetainHookContext = &hook;
  VFS::setRetainTrackedFileHookForHostedTest(pauseFirstTrackedFileRetain);
  const bool startedA = workerA->start();

  bool workerABlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && startedA; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (hook.entered == static_cast<size_t>(1) && workerA->getWaitDebugInfo(info) && info.queue &&
        info.queued && info.channelOwner == &hook.release &&
        workerA->getStatus() == Thread::Sleeping) {
      workerABlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  bool startedB = false;
  if (startedA) {
    workerB = new Thread(kernelProcess, retainTrackedFileWorker, &workerBContext, nullptr, false,
                         true, true);
    workerB->setName("hosted VFS retain serializer B");
    startedB = workerB->start();
  }
  bool workerBQueued = false;
  for (size_t attempt = 0; attempt < HostedAttempts && startedB; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (workerB->getWaitDebugInfo(info) && info.queue && info.queued &&
        info.channelOwner == VFS::instance().trackedFilesLockAddressForHostedTest() &&
        workerB->getStatus() == Thread::Sleeping) {
      workerBQueued = true;
      break;
    }
    if (workerBContext.returned) {
      break;
    }
    Scheduler::instance().yield();
  }

  bool passed = startedA && workerABlocked && startedB && workerBQueued && !workerBContext.returned;

  hook.release.release();
  const bool joinedA = startedA ? workerA->joinForCompletion() : false;
  const bool joinedB = startedB ? workerB->joinForCompletion() : false;
  if (!startedA) {
    delete workerA;
  }
  if (workerB && !startedB) {
    delete workerB;
  }
  VFS::setRetainTrackedFileHookForHostedTest(nullptr);
  g_TrackedFileRetainHookContext = nullptr;

  passed = passed && joinedA && joinedB && hook.returned == static_cast<size_t>(1) &&
           workerAContext.retained == static_cast<size_t>(1) &&
           workerBContext.retained == static_cast<size_t>(1);

  const bool firstWasFinal = VFS::instance().untrackFile(file, false);
  bool secondWasFinal = false;
  if (!firstWasFinal) {
    secondWasFinal = VFS::instance().untrackFile(file, false);
  }

  bool finalDestroyed = false;
  if (!firstWasFinal && !secondWasFinal) {
    finalDestroyed = VFS::instance().untrackFile(file);
  } else {
    delete file;
  }

  passed = passed && !firstWasFinal && !secondWasFinal && finalDestroyed &&
           destructions == static_cast<size_t>(1);
  if (!destructions) {
    repairAliasFileProbe(file, destructions);
  }

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL vfs-established-alias-serialization: "
        "concurrent established-owner retains were not serialized by the tracker lock");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS vfs-established-alias-serialization");
  return true;
}

enum DescriptorAliasConstruction {
  DescriptorDirect,
  DescriptorCopy,
  DescriptorPointerCopy,
};

bool descriptorEstablishedAliasLifetime(DescriptorAliasConstruction construction) {
  Atomic<size_t> destructions(0);
  EstablishedAliasFileProbe* file = new EstablishedAliasFileProbe(destructions);
  FileDescriptor* source = nullptr;

  if (construction != DescriptorDirect) {
    source = new FileDescriptor(file);
  }

  VFS::instance().trackFile(file);
  VFS::instance().trackFile(file);

  FileDescriptor* alias = nullptr;
  if (construction == DescriptorDirect) {
    alias = new FileDescriptor(file);
  } else if (construction == DescriptorCopy) {
    alias = new FileDescriptor(*source);
  } else {
    alias = new FileDescriptor(source);
  }

  delete source;
  VFS::instance().untrackFile(file);
  const bool emergencyWasFinal = VFS::instance().untrackFile(file, false);
  bool passed = !emergencyWasFinal && !destructions;

  delete alias;
  passed = passed && destructions == static_cast<size_t>(1);
  repairAliasFileProbe(file, destructions);
  return passed;
}

bool establishedFileAliasLifetime() {
  const bool directPassed = descriptorEstablishedAliasLifetime(DescriptorDirect);
  const bool copyPassed = descriptorEstablishedAliasLifetime(DescriptorCopy);
  const bool pointerCopyPassed = descriptorEstablishedAliasLifetime(DescriptorPointerCopy);
  bool passed = directPassed && copyPassed && pointerCopyPassed;

  Atomic<size_t> destructions(0);
  EstablishedAliasFileProbe* untracked = new EstablishedAliasFileProbe(destructions);
  FileDescriptor* descriptor = new FileDescriptor(untracked);
  const bool descriptorPublishedFile = VFS::instance().untrackFile(untracked, false);
  delete descriptor;
  passed = passed && !descriptorPublishedFile && !destructions;
  repairAliasFileProbe(untracked, destructions);

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL file-established-alias-lifetime: "
        "tracked descriptors did not retain exactly one VFS owner, or an untracked descriptor "
        "published a new owner");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS file-established-alias-lifetime");
  return true;
}

bool processFilesystemContextLifetime(Process* kernelProcess) {
  Atomic<size_t> cwdDestructions(0);
  Atomic<size_t> rootDestructions(0);
  Atomic<size_t> borrowedCwdDestructions(0);
  Atomic<size_t> borrowedRootDestructions(0);
  RetainedLookupFilesystem borrowedCwdFilesystem;
  RetainedLookupFilesystem borrowedRootFilesystem;
  EstablishedAliasFileProbe* cwd = new EstablishedAliasFileProbe(cwdDestructions);
  EstablishedAliasFileProbe* root = new EstablishedAliasFileProbe(rootDestructions);
  EstablishedAliasFileProbe* borrowedCwd = new EstablishedAliasFileProbe(borrowedCwdDestructions);
  EstablishedAliasFileProbe* borrowedRoot = new EstablishedAliasFileProbe(borrowedRootDestructions);
  borrowedCwd->setFilesystem(&borrowedCwdFilesystem);
  borrowedRoot->setFilesystem(&borrowedRootFilesystem);
  borrowedCwdFilesystem.setRoot(borrowedCwd);
  borrowedRootFilesystem.setRoot(borrowedRoot);
  VFS::instance().trackFile(cwd);
  VFS::instance().trackFile(root);

  Process* parent = new Process(kernelProcess);
  parent->setCwd(cwd);
  parent->setRootFile(root);
  Process::FileContextLease cwdSnapshot;
  Process::FileContextLease rootSnapshot;
  File* retainedCwd = parent->acquireCwd(cwdSnapshot);
  File* retainedRoot = parent->acquireRootFile(rootSnapshot);
  Process* child = new Process(parent);

  const bool cwdNamespaceWasFinal = VFS::instance().untrackFile(cwd, false);
  const bool rootNamespaceWasFinal = VFS::instance().untrackFile(root, false);
  parent->setCwd(borrowedCwd);
  parent->setRootFile(borrowedRoot);
  const bool borrowedCwdWasPublished = VFS::instance().untrackFile(borrowedCwd, false);
  const bool borrowedRootWasPublished = VFS::instance().untrackFile(borrowedRoot, false);

  delete child;
  const bool snapshotsHeldAcrossReplacement =
      retainedCwd == cwd && retainedRoot == root && !cwdDestructions && !rootDestructions;
  cwdSnapshot.reset();
  rootSnapshot.reset();
  const bool inheritedReferencesReleased =
      cwdDestructions == static_cast<size_t>(1) && rootDestructions == static_cast<size_t>(1);
  delete parent;
  const bool borrowedReferencesSurvived = !borrowedCwdDestructions && !borrowedRootDestructions;
  delete borrowedCwd;
  delete borrowedRoot;

  const bool passed = !cwdNamespaceWasFinal && !rootNamespaceWasFinal && !borrowedCwdWasPublished &&
                      !borrowedRootWasPublished && snapshotsHeldAcrossReplacement &&
                      inheritedReferencesReleased && borrowedReferencesSurvived &&
                      borrowedCwdDestructions == static_cast<size_t>(1) &&
                      borrowedRootDestructions == static_cast<size_t>(1);
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL process-filesystem-context-lifetime: "
        "fork, replacement, or destruction mismanaged a cwd/root VFS owner");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS process-filesystem-context-lifetime");
  return true;
}

enum MappingAliasConstruction {
  MappingDirect,
  MappingClone,
  MappingSplit,
};

bool mappingEstablishedAliasLifetime(MappingAliasConstruction construction) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  Atomic<size_t> destructions(0);
  EstablishedAliasFileProbe* file = new EstablishedAliasFileProbe(destructions);
  MemoryMappedObject* source = nullptr;

  if (construction != MappingDirect) {
    source = new MemoryMappedFile(0x100000, pageSize * 2, 0, file, false, MemoryMappedObject::Read);
  }

  VFS::instance().trackFile(file);
  VFS::instance().trackFile(file);

  MemoryMappedObject* alias = nullptr;
  if (construction == MappingDirect) {
    alias = new MemoryMappedFile(0x100000, pageSize, 0, file, false, MemoryMappedObject::Read);
  } else if (construction == MappingClone) {
    alias = source->clone();
  } else {
    alias = source->split(0x100000 + pageSize);
  }

  delete source;
  VFS::instance().untrackFile(file);
  const bool emergencyWasFinal = VFS::instance().untrackFile(file, false);
  bool passed = !emergencyWasFinal && !destructions;

  delete alias;
  passed = passed && destructions == static_cast<size_t>(1);
  repairAliasFileProbe(file, destructions);
  return passed;
}

bool establishedMappingAliasLifetime() {
  const bool directPassed = mappingEstablishedAliasLifetime(MappingDirect);
  const bool clonePassed = mappingEstablishedAliasLifetime(MappingClone);
  const bool splitPassed = mappingEstablishedAliasLifetime(MappingSplit);
  bool passed = directPassed && clonePassed && splitPassed;

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  Atomic<size_t> destructions(0);
  EstablishedAliasFileProbe* untracked = new EstablishedAliasFileProbe(destructions);
  MemoryMappedObject* mapping =
      new MemoryMappedFile(0x100000, pageSize, 0, untracked, false, MemoryMappedObject::Read);
  const bool mappingPublishedFile = VFS::instance().untrackFile(untracked, false);
  delete mapping;
  passed = passed && !mappingPublishedFile && !destructions;
  repairAliasFileProbe(untracked, destructions);

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL mmap-established-alias-lifetime: "
        "tracked mappings did not retain a VFS owner, or an untracked mapping published a new "
        "owner");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS mmap-established-alias-lifetime");
  return true;
}

bool munmapUsesTargetPageGeometry(Thread* thread) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();

  thread->setErrno(0);
  const int misalignedResult = posix_munmap(reinterpret_cast<void*>(pageSize / 2), pageSize);
  const bool rejectedMisaligned =
      misalignedResult == -1 && thread->getErrno() == Error::InvalidArgument;

  thread->setErrno(0);
  const int alignedResult = posix_munmap(reinterpret_cast<void*>(pageSize), pageSize);
  const bool acceptedAligned = alignedResult == 0 && !thread->getErrno();
  thread->setErrno(0);

  if (!rejectedMisaligned || !acceptedAligned) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL munmap-target-page-geometry: "
        "munmap did not validate addresses against the target page size");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS munmap-target-page-geometry");
  return true;
}

bool mappingManagerSplitLifetime(Process* process, bool exactSuffix) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = pageSize * 3;
  uintptr_t address = 0;
  if (!process->getSpaceAllocator().allocate(mappingLength, address)) {
    return false;
  }

  Atomic<size_t> destructions(0);
  EstablishedAliasFileProbe* file = new EstablishedAliasFileProbe(destructions);
  VFS::instance().trackFile(file);
  VFS::instance().trackFile(file);

  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapFile(
      file, mappedAddress, mappingLength, MemoryMappedObject::Read);
  bool passed = mapping && mappedAddress == address;
  if (mapping) {
    const size_t removedMiddleOrSuffix = MemoryMapManager::instance().remove(
        address + pageSize, exactSuffix ? pageSize * 2 : pageSize);
    const size_t removedPrefix = MemoryMapManager::instance().remove(address, pageSize);
    size_t removedTail = 1;
    if (!exactSuffix) {
      removedTail = MemoryMapManager::instance().remove(address + pageSize * 2, pageSize);
    }
    passed = passed && removedMiddleOrSuffix == 1 && removedPrefix == 1 && removedTail == 1;
  }

  MemoryMapManager::instance().remove(address, mappingLength);
  process->getSpaceAllocator().free(address, mappingLength);
  const bool namespaceWasFinal = VFS::instance().untrackFile(file);
  const bool emergencyWasFinal = VFS::instance().untrackFile(file);
  passed =
      passed && !namespaceWasFinal && emergencyWasFinal && destructions == static_cast<size_t>(1);
  repairAliasFileProbe(file, destructions);
  return passed;
}

bool mappingManagerSplitLifetime(Process* process) {
  const bool exactSuffixPassed = mappingManagerSplitLifetime(process, true);
  const bool middlePassed = mappingManagerSplitLifetime(process, false);
  const bool passed = exactSuffixPassed && middlePassed;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL mmap-split-alias-lifetime: "
        "an exact-suffix or middle removal leaked a file-backed mapping owner");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS mmap-split-alias-lifetime");
  return true;
}

bool posixPathLookupLifetime(Process* kernelProcess) {
  Filesystem* priorRoot = VFS::instance().getRootFilesystem();
  if (priorRoot) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-path-lookup-lifetime: "
        "the isolated pathname fixture requires an empty hosted root namespace");
    return false;
  }
  UnixFilesystem* testFilesystem = new UnixFilesystem;
  Filesystem* displacedRoot = VFS::instance().swapRootFilesystemForHostedTest(testFilesystem);
  const bool rootInstalled = displacedRoot == priorRoot;
  File* root = testFilesystem->getRoot();
  const String path("/hosted-established-alias-path-cache");
  VFS::instance().remove(path, root);
  const bool created = VFS::instance().createFile(path, 0600, root);

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  subsystem->setAbi(PosixSubsystem::LinuxAbi);

  File* original = created ? subsystem->findFile(path, root) : nullptr;
  if (original) {
    VFS::instance().trackFile(original);
  }

  const bool originalRemoved = original && VFS::instance().remove(path, root);
  const bool recreated = originalRemoved && VFS::instance().createFile(path, 0600, root);

  File* replacement = recreated ? VFS::instance().find(path, root) : nullptr;
  File* resolved = recreated ? subsystem->findFile(path, root) : nullptr;
  const bool resolvedReplacement =
      replacement && replacement != original && resolved == replacement;

  File* remaining = VFS::instance().find(path, root);
  const bool pathCleaned = !remaining || VFS::instance().remove(path, root);
  delete process;
  if (original) {
    VFS::instance().untrackFile(original);
  }

  Filesystem* removedRoot = VFS::instance().swapRootFilesystemForHostedTest(priorRoot);
  const bool rootRestored = removedRoot == testFilesystem;
  delete testFilesystem;

  const bool passed = rootInstalled && created && original && originalRemoved && recreated &&
                      resolvedReplacement && pathCleaned && rootRestored;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-path-lookup-lifetime: "
        "a removed pathname resolved to its retired cached File object");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-path-lookup-lifetime");
  return true;
}

class PollGenerationProbe : public NetworkSyscalls {
 public:
  PollGenerationProbe(Atomic<size_t>& queries, Atomic<size_t>& notifications,
                      Atomic<size_t>& destructions)
      : NetworkSyscalls(AF_UNSPEC, SOCK_STREAM, 0),
        m_Queries(queries),
        m_Notifications(notifications),
        m_Destructions(destructions),
        m_Ready(0) {}

  ~PollGenerationProbe() override {
    m_Destructions += 1;
  }

  int connect(const struct sockaddr_storage*, socklen_t) override {
    return -1;
  }

  ssize_t sendto_msg(const struct msghdr*) override {
    return -1;
  }

  ssize_t recvfrom_msg(struct msghdr*) override {
    return -1;
  }

  int listen(int) override {
    return -1;
  }

  int bind(const struct sockaddr_storage*, socklen_t) override {
    return -1;
  }

  int accept(struct sockaddr_storage*, socklen_t*, int) override {
    return -1;
  }

  int getpeername(struct sockaddr_storage*, socklen_t*) override {
    return -1;
  }

  int getsockname(struct sockaddr_storage*, socklen_t*) override {
    return -1;
  }

  int setsockopt(int, int, const void*, socklen_t) override {
    return -1;
  }

  int getsockopt(int, int, void*, socklen_t*) override {
    return -1;
  }

  ReadyMask queryReady(bool reading, bool writing) override {
    m_Queries += 1;
    ReadyMask ready = ReadyNone;
    if (reading && m_Ready) {
      ready |= ReadyRead;
    }
    if (writing) {
      ready |= ReadyWrite;
    }
    return ready;
  }

  void makeReadable() {
    m_Ready = 1;
    m_Notifications += 1;
    notifyReadiness(ReadyRead);
  }

 private:
  Atomic<size_t>& m_Queries;
  Atomic<size_t>& m_Notifications;
  Atomic<size_t>& m_Destructions;
  Atomic<size_t> m_Ready;
};

struct DescriptorCloseContext {
  DescriptorCloseContext(PosixSubsystem* subsystem, size_t fd)
      : subsystem(subsystem),
        fd(fd),
        release(0, false),
        entered(0),
        acquired(0),
        usedAfterClose(0),
        returned(0) {}

  PosixSubsystem* subsystem;
  size_t fd;
  Semaphore release;
  Atomic<size_t> entered;
  Atomic<size_t> acquired;
  Atomic<size_t> usedAfterClose;
  Atomic<size_t> returned;
};

int holdDescriptorAcrossBlock(void* parameter) {
  DescriptorCloseContext* context = reinterpret_cast<DescriptorCloseContext*>(parameter);
  DescriptorLease descriptor;
  context->acquired = context->subsystem->acquireFileDescriptor(context->fd, descriptor) ? 1 : 0;
  context->entered += 1;

  if (!context->release.acquireForCompletion()) {
    context->returned += 1;
    return 1;
  }

  if (descriptor && descriptor->fd == context->fd) {
    context->usedAfterClose += 1;
  }
  context->returned += 1;
  return 0;
}

bool descriptorClosePinning(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 37;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Atomic<size_t> destructions(0);
  DescriptorRetirementProbe* probe = new DescriptorRetirementProbe(destructions);
  probe->fd = DescriptorNumber;
  subsystem->addFileDescriptor(DescriptorNumber, probe);

  DescriptorCloseContext context(subsystem, DescriptorNumber);
  Thread* worker =
      new Thread(kernelProcess, holdDescriptorAcrossBlock, &context, nullptr, false, true, true);
  worker->setName("hosted descriptor pin holder");

  bool passed = worker->start();
  bool blocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && passed; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context.entered && worker->getWaitDebugInfo(info) && info.queue && info.queued &&
        info.channelOwner == &context.release && worker->getStatus() == Thread::Sleeping) {
      blocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  passed = passed && blocked && context.acquired == 1;
  DescriptorLease closing;
  const bool closeAcquired = subsystem->acquireFileDescriptor(DescriptorNumber, closing);
  const bool closed = closeAcquired && subsystem->closeFileDescriptor(DescriptorNumber, closing);
  closing.reset();
  passed = passed && closed;

  DescriptorLease unpublished;
  passed = passed && !subsystem->acquireFileDescriptor(DescriptorNumber, unpublished) &&
           destructions == 0;

  context.release.release();
  passed = worker->join() && passed;
  passed = passed && context.returned == 1 && context.usedAfterClose == 1 && destructions == 1;

  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-close-pinning: "
        "close did not unpublish immediately while retaining the active "
        "operation");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-close-pinning");
  return true;
}

bool descriptorCloseGeneration(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 38;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Atomic<size_t> oldDestructions(0);
  Atomic<size_t> replacementDestructions(0);
  DescriptorRetirementProbe* oldDescriptor = new DescriptorRetirementProbe(oldDestructions);
  oldDescriptor->fd = DescriptorNumber;
  oldDescriptor->setOffset(1);
  subsystem->addFileDescriptor(DescriptorNumber, oldDescriptor);

  DescriptorLease oldLease;
  bool passed = subsystem->acquireFileDescriptor(DescriptorNumber, oldLease);

  DescriptorRetirementProbe* replacement = new DescriptorRetirementProbe(replacementDestructions);
  replacement->fd = DescriptorNumber;
  replacement->setOffset(2);
  subsystem->addFileDescriptor(DescriptorNumber, replacement);

  // An in-flight close of the old generation must not remove a descriptor
  // which has since reused the same numeric fd.
  passed = passed && !subsystem->closeFileDescriptor(DescriptorNumber, oldLease) &&
           oldDestructions == 0 && replacementDestructions == 0;

  DescriptorLease replacementLease;
  passed = passed && subsystem->acquireFileDescriptor(DescriptorNumber, replacementLease) &&
           replacementLease->getOffset() == 2;

  oldLease.reset();
  passed = passed && oldDestructions == 1 && replacementDestructions == 0;

  const bool replacementClosed = subsystem->closeFileDescriptor(DescriptorNumber, replacementLease);
  DescriptorLease unpublished;
  passed = passed && replacementClosed &&
           !subsystem->acquireFileDescriptor(DescriptorNumber, unpublished) &&
           replacementDestructions == 0;
  replacementLease.reset();
  passed = passed && replacementDestructions == 1;

  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-close-generation: "
        "an old close removed a reused descriptor generation");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-close-generation");
  return true;
}

struct DescriptorPositionContext {
  explicit DescriptorPositionContext(FileDescriptor* descriptor)
      : descriptor(descriptor), entered(0), acquired(0), observed(0) {}

  FileDescriptor* descriptor;
  Atomic<size_t> entered;
  Atomic<size_t> acquired;
  Atomic<size_t> observed;
};

class DescriptorPositionFile final : public File {
 public:
  explicit DescriptorPositionFile(bool seekable)
      : File(String("position-policy"), 0, 0, 0, 1, nullptr, 0, nullptr),
        m_Seekable(seekable),
        m_ReadOffset(~static_cast<uint64_t>(0)),
        m_WriteOffset(~static_cast<uint64_t>(0)),
        m_ReadCanBlock(true),
        m_WriteCanBlock(true) {}

  bool isSeekable() const override {
    return m_Seekable;
  }

  uint64_t readOffset() const {
    return m_ReadOffset;
  }

  uint64_t writeOffset() const {
    return m_WriteOffset;
  }

  bool readCanBlock() const {
    return m_ReadCanBlock;
  }

  bool writeCanBlock() const {
    return m_WriteCanBlock;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t, bool canBlock) override {
    m_ReadOffset = location;
    m_ReadCanBlock = canBlock;
    return size;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t, bool canBlock) override {
    m_WriteOffset = location;
    m_WriteCanBlock = canBlock;
    return size;
  }

 private:
  bool m_Seekable;
  uint64_t m_ReadOffset;
  uint64_t m_WriteOffset;
  bool m_ReadCanBlock;
  bool m_WriteCanBlock;
};

class DescriptorAppendFile final : public File {
 public:
  DescriptorAppendFile()
      : File(String("append-policy"), 0, 0, 0, 1, nullptr, 10, nullptr),
        m_WriteCount(0),
        m_WriteOffsets{0, 0, 0} {}

  uint64_t writeOffset(size_t index) const {
    return m_WriteOffsets[index];
  }

  size_t writeCount() const {
    return m_WriteCount;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t, bool) override {
    if (m_WriteCount < 3) {
      m_WriteOffsets[m_WriteCount] = location;
    }
    ++m_WriteCount;
    if (location + size > getSize()) {
      setSize(location + size);
    }
    return size;
  }

 private:
  size_t m_WriteCount;
  uint64_t m_WriteOffsets[3];
};

class ConcurrentDescriptorAppendFile final : public File {
 public:
  ConcurrentDescriptorAppendFile()
      : File(String("concurrent-append-policy"), 0, 0, 0, 1, nullptr, 10, nullptr),
        m_FirstWriteEntered(0, false),
        m_ReleaseFirstWrite(0, false),
        m_WriteCount(0),
        m_WriteOffsets{0, 0} {}

  bool waitForFirstWrite() {
    return m_FirstWriteEntered.acquireForCompletion();
  }

  void releaseFirstWrite() {
    m_ReleaseFirstWrite.release();
  }

  size_t writeCount() const {
    return m_WriteCount;
  }

  uint64_t writeOffset(size_t index) const {
    return m_WriteOffsets[index];
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t, bool) override {
    const size_t slot = (m_WriteCount += 1) - 1;
    if (slot < 2) {
      m_WriteOffsets[slot] = location;
    }
    if (!slot) {
      m_FirstWriteEntered.release();
      if (!m_ReleaseFirstWrite.acquireForCompletion()) {
        return 0;
      }
    }
    if (location + size > getSize()) {
      setSize(location + size);
    }
    return size;
  }

 private:
  Semaphore m_FirstWriteEntered;
  Semaphore m_ReleaseFirstWrite;
  Atomic<size_t> m_WriteCount;
  uint64_t m_WriteOffsets[2];
};

struct ConcurrentDescriptorAppendContext {
  explicit ConcurrentDescriptorAppendContext(FileDescriptor* descriptor)
      : descriptor(descriptor), entered(0), returned(0), result(0) {}

  FileDescriptor* descriptor;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<uint64_t> result;
};

int appendThroughDescriptor(void* parameter) {
  ConcurrentDescriptorAppendContext* context =
      reinterpret_cast<ConcurrentDescriptorAppendContext*>(parameter);
  char byte = 0;
  context->entered += 1;
  context->result = context->descriptor->write(1, reinterpret_cast<uintptr_t>(&byte));
  context->returned += 1;
  return context->result == 1 ? 0 : 1;
}

bool concurrentDescriptorAppendSerialization(Process* kernelProcess, bool positionedSecond) {
  ConcurrentDescriptorAppendFile file;
  FileDescriptor first(&file, 0, 0, 0, O_WRONLY | O_APPEND);
  FileDescriptor second(&file, positionedSecond ? 20 : 0, 0, 0,
                        positionedSecond ? O_WRONLY : O_WRONLY | O_APPEND);
  ConcurrentDescriptorAppendContext firstContext(&first);
  ConcurrentDescriptorAppendContext secondContext(&second);

  Thread* firstWorker =
      new Thread(kernelProcess, appendThroughDescriptor, &firstContext, nullptr, false, true, true);
  Thread* secondWorker = new Thread(kernelProcess, appendThroughDescriptor, &secondContext, nullptr,
                                    false, true, true);
  firstWorker->setName("hosted first independent append");
  if (positionedSecond) {
    secondWorker->setName("hosted positioned append race");
  } else {
    secondWorker->setName("hosted second independent append");
  }

  const bool firstStarted = firstWorker->start();
  const bool firstEntered = firstStarted && file.waitForFirstWrite();
  const bool secondStarted = firstEntered && secondWorker->start();
  bool secondBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && secondStarted; ++attempt) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (secondContext.entered && !secondContext.returned && file.writeCount() == 1 &&
        secondWorker->getWaitDebugInfo(info) && info.queued &&
        secondWorker->getDebugState(debugAddress) == Thread::SemWait) {
      secondBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  file.releaseFirstWrite();
  const bool firstJoined = firstStarted && firstWorker->joinForCompletion();
  const bool secondJoined = secondStarted && secondWorker->joinForCompletion();
  if (!firstStarted) {
    delete firstWorker;
  }
  if (!secondStarted) {
    delete secondWorker;
  }

  const bool separateDescriptions =
      first.acquireOpenFileDescription().get() != second.acquireOpenFileDescription().get();
  const uint64_t expectedSecondOffset = positionedSecond ? 20 : 11;
  const uint64_t expectedSize = positionedSecond ? 21 : 12;
  const uint64_t expectedSecondPosition = positionedSecond ? 21 : 12;
  return firstStarted && firstEntered && secondStarted && secondBlocked && firstJoined &&
         secondJoined && firstContext.returned == 1 && secondContext.returned == 1 &&
         firstContext.result == 1 && secondContext.result == 1 && separateDescriptions &&
         file.writeCount() == 2 && file.writeOffset(0) == 10 &&
         file.writeOffset(1) == expectedSecondOffset && file.getSize() == expectedSize &&
         first.getOffset() == 11 && second.getOffset() == expectedSecondPosition;
}

bool independentDescriptorAppendSerialization(Process* kernelProcess) {
  return concurrentDescriptorAppendSerialization(kernelProcess, false) &&
         concurrentDescriptorAppendSerialization(kernelProcess, true);
}

bool descriptorOpenFileDescriptionState() {
  DescriptorPositionFile file(true);
  FileDescriptor source(&file, 0, 19, 0, O_RDWR | O_CLOEXEC);
  FileDescriptor alias(source);
  alias.setFlags(0);
  FileDescriptor::OpenFileDescriptionLease sourceDescription = source.acquireOpenFileDescription();
  FileDescriptor::OpenFileDescriptionLease aliasDescription = alias.acquireOpenFileDescription();

  source.addStatusFlag(O_NONBLOCK | O_CLOEXEC);
  bool passed = source.getFlags() == FD_CLOEXEC && alias.getFlags() == 0 &&
                source.getStatusFlags() == (O_RDWR | O_NONBLOCK) &&
                alias.getStatusFlags() == (O_RDWR | O_NONBLOCK) &&
                sourceDescription == aliasDescription && sourceDescription->getFile() == &file &&
                !sourceDescription->getNetworkImpl() &&
                sourceDescription->descriptorOwnerCount() == 2;

  {
    FileDescriptor third(alias);
    passed = passed && sourceDescription->descriptorOwnerCount() == 3;
  }
  passed = passed && sourceDescription->descriptorOwnerCount() == 2;

  alias.setStatusFlags(O_APPEND | O_CLOEXEC);
  passed = passed && source.getStatusFlags() == (O_RDWR | O_APPEND) &&
           alias.getStatusFlags() == (O_RDWR | O_APPEND) && source.getFlags() == FD_CLOEXEC &&
           alias.getFlags() == 0;

  source.removeStatusFlag(O_APPEND);
  passed = passed && source.getStatusFlags() == O_RDWR && alias.getStatusFlags() == O_RDWR;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-open-file-description-state: "
        "status flags were descriptor-local, access mode was lost, or CLOEXEC entered shared "
        "state");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-open-file-description-state");
  return true;
}

bool descriptorOpenFileDescriptionLifetime() {
  Atomic<size_t> fileDestructions(0);
  EstablishedAliasFileProbe* file = new EstablishedAliasFileProbe(fileDestructions);
  VFS::instance().trackFile(file);
  FileDescriptor* descriptor = new FileDescriptor(file, 0, 0, 0, O_RDWR);
  const bool baselineWasFinal = VFS::instance().untrackFile(file, false);
  FileDescriptor::OpenFileDescriptionLease fileDescription =
      descriptor->acquireOpenFileDescription();

  delete descriptor;
  bool passed = !baselineWasFinal && !fileDestructions &&
                fileDescription->descriptorOwnerCount() == 0 && fileDescription->getFile() == file;
  fileDescription.reset();
  passed = passed && fileDestructions == 1;
  repairAliasFileProbe(file, fileDestructions);

  Atomic<size_t> registrations(0);
  Atomic<size_t> unpolls(0);
  Atomic<size_t> networkDestructions(0);
  FileDescriptor* socketDescriptor = new FileDescriptor;
  SharedPointer<NetworkSyscalls> network(
      new PollGenerationProbe(registrations, unpolls, networkDestructions));
  socketDescriptor->setNetworkImpl(network);
  network.reset();
  FileDescriptor::OpenFileDescriptionLease socketDescription =
      socketDescriptor->acquireOpenFileDescription();

  delete socketDescriptor;
  passed = passed && !networkDestructions && socketDescription->descriptorOwnerCount() == 0 &&
           socketDescription->getNetworkImpl();
  socketDescription.reset();
  passed = passed && networkDestructions == 1;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-open-file-description-lifetime: "
        "an OFD lease did not retain its target independently of descriptor aliases");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-open-file-description-lifetime");
  return true;
}

bool descriptorAppendPolicy(Process* kernelProcess) {
  char byte = 0;
  DescriptorAppendFile file;
  FileDescriptor source(&file, 2, 0, 0, O_WRONLY | O_APPEND);
  FileDescriptor alias(source);

  const bool firstWrite = source.write(3, reinterpret_cast<uintptr_t>(&byte)) == 3;
  source.setOffset(1);
  const bool secondWrite = alias.write(2, reinterpret_cast<uintptr_t>(&byte)) == 2;

  alias.setStatusFlags(0);
  source.setOffset(5);
  const bool positionedWrite = source.write(1, reinterpret_cast<uintptr_t>(&byte)) == 1;

  const bool independentSerialization = independentDescriptorAppendSerialization(kernelProcess);
  const bool passed = firstWrite && secondWrite && positionedWrite && independentSerialization &&
                      file.writeCount() == 3 && file.writeOffset(0) == 10 &&
                      file.writeOffset(1) == 13 && file.writeOffset(2) == 5 &&
                      file.getSize() == 15 && source.getOffset() == 6 && alias.getOffset() == 6 &&
                      source.getStatusFlags() == O_WRONLY;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-append-policy: "
        "append did not serialize EOF selection across open descriptions or update the shared "
        "offset");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-append-policy");
  return true;
}

bool descriptorNonblockingPolicy() {
  char byte = 0;
  DescriptorPositionFile sequential(false);
  FileDescriptor source(&sequential, 0, 0, 0, O_RDWR | O_NONBLOCK);
  FileDescriptor alias(source);

  const bool nonblockingRead = source.read(1, reinterpret_cast<uintptr_t>(&byte)) == 1;
  const bool nonblockingWrite = source.write(1, reinterpret_cast<uintptr_t>(&byte)) == 1;
  bool passed = nonblockingRead && nonblockingWrite && !sequential.readCanBlock() &&
                !sequential.writeCanBlock();

  alias.removeStatusFlag(O_NONBLOCK);
  const bool blockingRead = source.read(1, reinterpret_cast<uintptr_t>(&byte)) == 1;
  const bool blockingWrite = source.write(1, reinterpret_cast<uintptr_t>(&byte)) == 1;
  passed = passed && blockingRead && blockingWrite && sequential.readCanBlock() &&
           sequential.writeCanBlock() && source.getStatusFlags() == O_RDWR &&
           alias.getStatusFlags() == O_RDWR;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-nonblocking-policy: "
        "O_NONBLOCK did not reach file I/O or did not propagate across aliases");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-nonblocking-policy");
  return true;
}

bool descriptorPositionPolicy() {
  char byte = 0;
  DescriptorPositionFile seekable(true);
  FileDescriptor positioned(&seekable, 40, 0, 0, O_RDWR);
  const bool positionedPassed = positioned.read(1, reinterpret_cast<uintptr_t>(&byte)) == 1 &&
                                positioned.write(1, reinterpret_cast<uintptr_t>(&byte)) == 1 &&
                                seekable.readOffset() == 40 && seekable.writeOffset() == 41 &&
                                positioned.getOffset() == 42;

  DescriptorPositionFile sequential(false);
  FileDescriptor unpositioned(&sequential, 40, 0, 0, O_RDWR);
  const bool unpositionedPassed = unpositioned.read(1, reinterpret_cast<uintptr_t>(&byte)) == 1 &&
                                  unpositioned.write(1, reinterpret_cast<uintptr_t>(&byte)) == 1 &&
                                  sequential.readOffset() == 0 && sequential.writeOffset() == 0 &&
                                  unpositioned.getOffset() == 40;

  if (!positionedPassed || !unpositionedPassed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-position-policy: "
        "non-seekable I/O consumed a file position");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-position-policy");
  return true;
}

int advanceDescriptorPosition(void* parameter) {
  DescriptorPositionContext* context = reinterpret_cast<DescriptorPositionContext*>(parameter);
  context->entered += 1;
  FileDescriptor::PositionGuard position = context->descriptor->lockPosition();
  context->observed = position.offset();
  position.advanceOffset(1);
  context->acquired += 1;
  return 0;
}

bool descriptorPositionAliasSerialization(Process* kernelProcess) {
  FileDescriptor source;
  source.setOffset(40);
  FileDescriptor alias(source);
  DescriptorPositionContext context(&alias);

  Thread* worker =
      new Thread(kernelProcess, advanceDescriptorPosition, &context, nullptr, false, true, true);
  worker->setName("hosted descriptor position alias");

  bool started = false;
  bool queued = false;
  {
    FileDescriptor::PositionGuard position = source.lockPosition();
    started = worker->start();
    for (size_t attempt = 0; attempt < HostedAttempts && started; ++attempt) {
      Thread::WaitDebugInfo info = {};
      uintptr_t debugAddress = 0;
      if (context.entered && !context.acquired && worker->getWaitDebugInfo(info) && info.queued &&
          worker->getDebugState(debugAddress) == Thread::SemWait) {
        queued = true;
        break;
      }
      Scheduler::instance().yield();
    }
    position.setOffset(41);
  }

  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool passed = started && queued && joined && context.acquired == 1 &&
                      context.observed == 41 && source.getOffset() == 42 && alias.getOffset() == 42;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-position-alias-serialization: "
        "duplicated descriptors did not serialize access to their shared offset");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-position-alias-serialization");
  return true;
}

class VectorWriteFile final : public File {
 public:
  VectorWriteFile()
      : File(String("vector-write-policy"), 0, 0, 0, 1, nullptr, 0, nullptr),
        m_FirstWriteEntered(0, false),
        m_ReleaseFirstWrite(0, false),
        m_WriteCount(0),
        m_WriteOffsets{0, 0, 0},
        m_WriteValues{0, 0, 0} {}

  bool waitForFirstWrite() {
    return m_FirstWriteEntered.acquireForCompletion();
  }

  void releaseFirstWrite() {
    m_ReleaseFirstWrite.release();
  }

  size_t writeCount() const {
    return m_WriteCount;
  }

  uint64_t writeOffset(size_t index) const {
    return m_WriteOffsets[index];
  }

  char writeValue(size_t index) const {
    return m_WriteValues[index];
  }

 protected:
  bool isBytewise() const override {
    return true;
  }

  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    const size_t slot = (m_WriteCount += 1) - 1;
    if (slot < 3) {
      m_WriteOffsets[slot] = location;
      m_WriteValues[slot] = size ? *reinterpret_cast<const char*>(buffer) : 0;
    }
    if (!slot) {
      m_FirstWriteEntered.release();
      if (!m_ReleaseFirstWrite.acquireForCompletion()) {
        return 0;
      }
    }
    if (location + size > getSize()) {
      setSize(location + size);
    }
    return size;
  }

 private:
  Semaphore m_FirstWriteEntered;
  Semaphore m_ReleaseFirstWrite;
  Atomic<size_t> m_WriteCount;
  uint64_t m_WriteOffsets[3];
  char m_WriteValues[3];
};

struct VectorWriteContext {
  explicit VectorWriteContext(size_t descriptor)
      : descriptor(descriptor), result(-2), error(0), returned(0) {}

  size_t descriptor;
  int result;
  int error;
  Atomic<size_t> returned;
};

int writeThroughVector(void* parameter) {
  VectorWriteContext* context = reinterpret_cast<VectorWriteContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  char bytes[2] = {'a', 'b'};
  struct iovec vectors[2] = {{&bytes[0], 1}, {&bytes[1], 1}};
  thread->setErrno(0);
  context->result = posix_writev(static_cast<int>(context->descriptor), vectors, 2);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

int writeThroughAlias(void* parameter) {
  VectorWriteContext* context = reinterpret_cast<VectorWriteContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  char byte = 'c';
  thread->setErrno(0);
  context->result = posix_write(static_cast<int>(context->descriptor), &byte, 1, false);
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool descriptorVectorIoSerialization(Process* kernelProcess) {
  constexpr size_t SourceDescriptor = 74;
  constexpr size_t AliasDescriptor = 75;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  VectorWriteFile original;
  DescriptorPositionFile replacement(true);
  FileDescriptor* source = new FileDescriptor(&original, 0, SourceDescriptor, 0, O_WRONLY);
  FileDescriptor* alias = new FileDescriptor(*source);
  alias->fd = AliasDescriptor;
  subsystem->addFileDescriptor(SourceDescriptor, source);
  subsystem->addFileDescriptor(AliasDescriptor, alias);

  VectorWriteContext vectorContext(SourceDescriptor);
  VectorWriteContext aliasContext(AliasDescriptor);
  Thread* vectorWorker =
      new Thread(process, writeThroughVector, &vectorContext, nullptr, false, true, true);
  Thread* aliasWorker =
      new Thread(process, writeThroughAlias, &aliasContext, nullptr, false, true, true);
  vectorWorker->setName("hosted vector write generation");
  aliasWorker->setName("hosted vector write alias");

  const bool vectorStarted = vectorWorker->start();
  const bool firstEntered = vectorStarted && original.waitForFirstWrite();
  const bool aliasStarted = firstEntered && aliasWorker->start();
  bool aliasBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && aliasStarted; ++attempt) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (!aliasContext.returned && original.writeCount() == 1 &&
        aliasWorker->getWaitDebugInfo(info) && info.queued &&
        aliasWorker->getDebugState(debugAddress) == Thread::SemWait) {
      aliasBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  DescriptorLease closingSource;
  const bool sourceAcquired = subsystem->acquireFileDescriptor(SourceDescriptor, closingSource);
  const bool sourceClosed =
      sourceAcquired && subsystem->closeFileDescriptor(SourceDescriptor, closingSource);
  closingSource.reset();
  FileDescriptor* replacementDescriptor =
      new FileDescriptor(&replacement, 0, SourceDescriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(SourceDescriptor, replacementDescriptor);

  original.releaseFirstWrite();
  const bool vectorJoined = vectorStarted && vectorWorker->joinForCompletion();
  const bool aliasJoined = aliasStarted && aliasWorker->joinForCompletion();
  if (!vectorStarted) {
    delete vectorWorker;
  }
  if (!aliasStarted) {
    delete aliasWorker;
  }

  bool passed = vectorStarted && firstEntered && aliasStarted && aliasBlocked && sourceClosed &&
                vectorJoined && aliasJoined && vectorContext.returned == 1 &&
                vectorContext.result == 2 && vectorContext.error == 0 &&
                aliasContext.returned == 1 && aliasContext.result == 1 && aliasContext.error == 0 &&
                original.writeCount() == 3 && original.writeOffset(0) == 0 &&
                original.writeOffset(1) == 1 && original.writeOffset(2) == 2 &&
                original.writeValue(0) == 'a' && original.writeValue(1) == 'b' &&
                original.writeValue(2) == 'c' && original.getSize() == 3 &&
                replacement.writeOffset() == ~static_cast<uint64_t>(0);

  DescriptorLease closingAlias;
  const bool aliasAcquired = subsystem->acquireFileDescriptor(AliasDescriptor, closingAlias);
  const bool aliasClosed =
      aliasAcquired && subsystem->closeFileDescriptor(AliasDescriptor, closingAlias);
  closingAlias.reset();
  DescriptorLease closingReplacement;
  const bool replacementAcquired =
      subsystem->acquireFileDescriptor(SourceDescriptor, closingReplacement);
  const bool replacementClosed =
      replacementAcquired && subsystem->closeFileDescriptor(SourceDescriptor, closingReplacement);
  closingReplacement.reset();
  passed = passed && aliasClosed && replacementClosed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-vector-io-serialization: "
        "writev switched descriptor generations or released the shared offset between vectors");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-vector-io-serialization");
  return true;
}

struct DescriptorDupContractContext {
  DescriptorDupContractContext(PosixSubsystem* subsystem, size_t sourceFd, size_t occupiedFd,
                               size_t minimum,
                               const FileDescriptor::OpenFileDescriptionLease& sourceDescription,
                               const FileDescriptor::OpenFileDescriptionLease& occupiedDescription)
      : subsystem(subsystem),
        sourceFd(sourceFd),
        occupiedFd(occupiedFd),
        minimum(minimum),
        sourceDescription(sourceDescription),
        occupiedDescription(occupiedDescription),
        duplicateResult(-2),
        duplicateError(0),
        occupiedIntact(false),
        duplicateAliasesSource(false),
        duplicateFlags(-1),
        duplicateClosed(false),
        ordinaryAllocation(static_cast<size_t>(-1)),
        sameInvalidResult(-2),
        sameInvalidError(0),
        returned(0) {}

  PosixSubsystem* subsystem;
  size_t sourceFd;
  size_t occupiedFd;
  size_t minimum;
  FileDescriptor::OpenFileDescriptionLease sourceDescription;
  FileDescriptor::OpenFileDescriptionLease occupiedDescription;
  int duplicateResult;
  int duplicateError;
  bool occupiedIntact;
  bool duplicateAliasesSource;
  int duplicateFlags;
  bool duplicateClosed;
  size_t ordinaryAllocation;
  int sameInvalidResult;
  int sameInvalidError;
  Atomic<size_t> returned;
};

int exerciseDescriptorDupContract(void* parameter) {
  DescriptorDupContractContext* context =
      reinterpret_cast<DescriptorDupContractContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();

  thread->setErrno(0);
  context->duplicateResult = posix_fcntl(static_cast<int>(context->sourceFd), F_DUPFD,
                                         reinterpret_cast<void*>(context->minimum));
  context->duplicateError = thread->getErrno();

  DescriptorLease occupied;
  if (context->subsystem->acquireFileDescriptor(context->occupiedFd, occupied)) {
    context->occupiedIntact =
        occupied->acquireOpenFileDescription().get() == context->occupiedDescription.get();
  }
  occupied.reset();

  DescriptorLease duplicate;
  if (context->duplicateResult >= 0 &&
      context->subsystem->acquireFileDescriptor(context->duplicateResult, duplicate)) {
    context->duplicateAliasesSource =
        duplicate->acquireOpenFileDescription().get() == context->sourceDescription.get();
    context->duplicateFlags = duplicate->getFlags();
    context->duplicateClosed = context->subsystem->closeFileDescriptor(
        static_cast<size_t>(context->duplicateResult), duplicate);
  }
  duplicate.reset();

  context->ordinaryAllocation = context->subsystem->getFd();

  constexpr int InvalidDescriptor = 80;
  thread->setErrno(0);
  context->sameInvalidResult = posix_dup2(InvalidDescriptor, InvalidDescriptor);
  context->sameInvalidError = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool descriptorDupContract(Process* kernelProcess) {
  constexpr size_t SourceDescriptor = 70;
  constexpr size_t MinimumDescriptor = 72;
  constexpr size_t ExpectedDescriptor = 73;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  File* sourceFile = new File;
  File* occupiedFile = new File;
  FileDescriptor* source =
      new FileDescriptor(sourceFile, 0, SourceDescriptor, FD_CLOEXEC, O_RDONLY);
  FileDescriptor* occupied = new FileDescriptor(occupiedFile, 0, MinimumDescriptor, 0, O_RDONLY);
  subsystem->addFileDescriptor(SourceDescriptor, source);
  subsystem->addFileDescriptor(MinimumDescriptor, occupied);

  DescriptorDupContractContext context(subsystem, SourceDescriptor, MinimumDescriptor,
                                       MinimumDescriptor, source->acquireOpenFileDescription(),
                                       occupied->acquireOpenFileDescription());
  Thread* worker =
      new Thread(process, exerciseDescriptorDupContract, &context, nullptr, false, true, true);
  worker->setName("hosted descriptor duplication contract");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed =
      started && joined && context.returned == 1 &&
      context.duplicateResult == static_cast<int>(ExpectedDescriptor) &&
      context.duplicateError == 0 && context.occupiedIntact && context.duplicateAliasesSource &&
      context.duplicateFlags == 0 && context.duplicateClosed && context.ordinaryAllocation == 0 &&
      context.sameInvalidResult == -1 && context.sameInvalidError == Error::BadFileDescriptor;

  delete process;
  context.sourceDescription.reset();
  context.occupiedDescription.reset();
  delete sourceFile;
  delete occupiedFile;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL descriptor-dup-contract: "
        "F_DUPFD replaced an occupied descriptor, hid a lower hole, or dup2 accepted an invalid "
        "same-fd source");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-dup-contract");
  return true;
}

struct PollCloseReuseContext {
  explicit PollCloseReuseContext(size_t fd)
      : descriptor{static_cast<int>(fd), POLLIN, 0}, result(-2), entered(0), returned(0) {}

  struct pollfd descriptor;
  Atomic<int> result;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

bool selectProjectionAndTimeoutContract() {
  constexpr unsigned int ReadResult = 1U;
  constexpr unsigned int WriteResult = 1U << 1;
  constexpr unsigned int ExceptionalResult = 1U << 2;
  constexpr unsigned int OneReady = 1U << 8;
  constexpr unsigned int TwoReady = 2U << 8;
  constexpr unsigned int ThreeReady = 3U << 8;

  const unsigned int hangup = posixSelectProjectionForTest(POLLHUP, true, true, true);
  const unsigned int error = posixSelectProjectionForTest(POLLERR, true, true, true);
  const unsigned int priority = posixSelectProjectionForTest(POLLPRI, true, true, true);
  const unsigned int all =
      posixSelectProjectionForTest(POLLIN | POLLOUT | POLLPRI, true, true, true);
  const unsigned int writeOnlyError = posixSelectProjectionForTest(POLLERR, false, true, false);

  const timeval zero = {0, 0};
  const timeval oneMicrosecond = {0, 1};
  const timeval oneMillisecond = {0, 1000};
  const timeval justOverOneMillisecond = {0, 1001};
  const timeval almostOneSecond = {0, 999999};
  const timeval exactMaximum = {
      INT_MAX / 1000,
      (INT_MAX % 1000) * 1000,
  };
  const timeval saturatingBoundary = {
      INT_MAX / 1000,
      ((INT_MAX % 1000) + 1) * 1000,
  };
  const timeval saturatingSeconds = {
      static_cast<time_t>(INT_MAX / 1000) + 1,
      0,
  };

  const bool projectionsPassed =
      hangup == (OneReady | ReadResult) && error == (TwoReady | ReadResult | WriteResult) &&
      priority == (OneReady | ExceptionalResult) &&
      all == (ThreeReady | ReadResult | WriteResult | ExceptionalResult) &&
      writeOnlyError == (OneReady | WriteResult);
  const bool timeoutPassed = posixSelectTimeoutMillisecondsForTest(zero) == 0 &&
                             posixSelectTimeoutMillisecondsForTest(oneMicrosecond) == 1 &&
                             posixSelectTimeoutMillisecondsForTest(oneMillisecond) == 1 &&
                             posixSelectTimeoutMillisecondsForTest(justOverOneMillisecond) == 2 &&
                             posixSelectTimeoutMillisecondsForTest(almostOneSecond) == 1000 &&
                             posixSelectTimeoutMillisecondsForTest(exactMaximum) == INT_MAX &&
                             posixSelectTimeoutMillisecondsForTest(saturatingBoundary) == INT_MAX &&
                             posixSelectTimeoutMillisecondsForTest(saturatingSeconds) == INT_MAX;

  if (!projectionsPassed || !timeoutPassed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL select-projection-timeout: "
        "readiness projection, return-bit counting, or timeout rounding was incorrect");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS select-projection-timeout");
  return true;
}

struct PipePollReadinessContext {
  PipePollReadinessContext(size_t readFd, size_t writeFd)
      : readFd(readFd),
        writeFd(writeFd),
        pollEntryGate(0, false),
        firstPhaseGate(0, false),
        eofGate(0, false),
        pollEntered(0),
        firstPhaseReturned(0),
        returned(0),
        emptyReadResult(-2),
        emptyReadError(0),
        firstPollResult(-2),
        firstPollEvents(0),
        firstReadResult(-2),
        fillWriteResult(-2),
        fullWriteResult(-2),
        fullWriteError(0),
        atomicSetupDrainResult(-2),
        atomicVectorResult(-2),
        atomicVectorError(0),
        drainResult(-2),
        overBoundaryWriteResult(-2),
        overBoundaryDrainResult(-2),
        eofPollResult(-2),
        eofPollEvents(0),
        eofReadResult(-2) {}

  size_t readFd;
  size_t writeFd;
  Semaphore pollEntryGate;
  Semaphore firstPhaseGate;
  Semaphore eofGate;
  Atomic<size_t> pollEntered;
  Atomic<size_t> firstPhaseReturned;
  Atomic<size_t> returned;
  int emptyReadResult;
  int emptyReadError;
  int firstPollResult;
  short firstPollEvents;
  int firstReadResult;
  int fillWriteResult;
  int fullWriteResult;
  int fullWriteError;
  int atomicSetupDrainResult;
  int atomicVectorResult;
  int atomicVectorError;
  int drainResult;
  int overBoundaryWriteResult;
  int overBoundaryDrainResult;
  int eofPollResult;
  short eofPollEvents;
  int eofReadResult;
};

struct EpollReadinessContext {
  EpollReadinessContext(PosixSubsystem* subsystem, const SharedPointer<EpollInstance>& instance,
                        size_t readFd, size_t writeFd, size_t aliasFd, size_t regularFd)
      : subsystem(subsystem),
        instance(instance),
        readDescription(),
        readFd(readFd),
        writeFd(writeFd),
        aliasFd(aliasFd),
        regularFd(regularFd),
        waitEntryGate(0, false),
        waitEntered(0),
        returned(0),
        addResult(-2),
        regularAddResult(-2),
        regularAddError(0),
        createdFd(-2),
        createdDescriptorAcquired(false),
        createdStatusFlags(-1),
        createdDescriptorFlags(-1),
        createdCloseResult(false),
        duplicateAddResult(-2),
        duplicateAddError(0),
        exclusiveModifyResult(-2),
        exclusiveModifyError(0),
        firstWaitResult(-2),
        firstWaitEvents(0),
        firstWaitData(0),
        repeatedWaitResult(-2),
        repeatedWaitEvents(0),
        repeatedWaitData(0),
        sameReadyWriteResult(-2),
        sameReadyWaitResult(-2),
        firstDrainResult(-2),
        drainedWaitResult(-2),
        transitionWriteResult(-2),
        transitionWaitResult(-2),
        transitionWaitEvents(0),
        transitionWaitData(0),
        transitionDrainResult(-2),
        levelModifyResult(-2),
        levelWriteResult(-2),
        levelWaitResult(-2),
        levelWaitEvents(0),
        levelWaitData(0),
        levelRepeatedWaitResult(-2),
        levelDrainResult(-2),
        oneShotModifyResult(-2),
        oneShotWriteResult(-2),
        oneShotWaitResult(-2),
        oneShotWaitEvents(0),
        oneShotWaitData(0),
        oneShotSuppressedResult(-2),
        rearmResult(-2),
        rearmedWaitResult(-2),
        rearmedWaitEvents(0),
        rearmedWaitData(0),
        oneShotDrainResult(-2),
        deleteResult(-2),
        postDeleteWriteResult(-2),
        postDeleteWaitResult(-2),
        postDeleteDrainResult(-2),
        eventFd(-2),
        eventFdAddResult(-2),
        eventFdFirstWriteResult(-2),
        eventFdFirstWaitResult(-2),
        eventFdFirstWaitEvents(0),
        eventFdFirstWaitData(0),
        eventFdSecondWriteResult(-2),
        eventFdSecondWaitResult(-2),
        eventFdSecondWaitEvents(0),
        eventFdSecondWaitData(0),
        eventFdReadResult(-2),
        eventFdReadValue(0),
        eventFdDeleteResult(-2),
        eventFdCloseResult(false),
        aliasAddResult(-2),
        originalCloseResult(false),
        ownersAfterOriginalClose(static_cast<size_t>(-1)),
        aliasWriteResult(-2),
        aliasWaitResult(-2),
        aliasWaitEvents(0),
        aliasWaitData(0),
        aliasDrainResult(-2),
        aliasCloseResult(false),
        ownersAfterAliasClose(static_cast<size_t>(-1)),
        prunedWaitResult(-2),
        descriptionRefsAfterPrune(static_cast<size_t>(-1)) {}

  PosixSubsystem* subsystem;
  SharedPointer<EpollInstance> instance;
  FileDescriptor::OpenFileDescriptionLease readDescription;
  size_t readFd;
  size_t writeFd;
  size_t aliasFd;
  size_t regularFd;
  Semaphore waitEntryGate;
  Atomic<size_t> waitEntered;
  Atomic<size_t> returned;
  int addResult;
  int regularAddResult;
  int regularAddError;
  int createdFd;
  bool createdDescriptorAcquired;
  int createdStatusFlags;
  int createdDescriptorFlags;
  bool createdCloseResult;
  int duplicateAddResult;
  int duplicateAddError;
  int exclusiveModifyResult;
  int exclusiveModifyError;
  int firstWaitResult;
  uint32_t firstWaitEvents;
  uint64_t firstWaitData;
  int repeatedWaitResult;
  uint32_t repeatedWaitEvents;
  uint64_t repeatedWaitData;
  int sameReadyWriteResult;
  int sameReadyWaitResult;
  int firstDrainResult;
  int drainedWaitResult;
  int transitionWriteResult;
  int transitionWaitResult;
  uint32_t transitionWaitEvents;
  uint64_t transitionWaitData;
  int transitionDrainResult;
  int levelModifyResult;
  int levelWriteResult;
  int levelWaitResult;
  uint32_t levelWaitEvents;
  uint64_t levelWaitData;
  int levelRepeatedWaitResult;
  int levelDrainResult;
  int oneShotModifyResult;
  int oneShotWriteResult;
  int oneShotWaitResult;
  uint32_t oneShotWaitEvents;
  uint64_t oneShotWaitData;
  int oneShotSuppressedResult;
  int rearmResult;
  int rearmedWaitResult;
  uint32_t rearmedWaitEvents;
  uint64_t rearmedWaitData;
  int oneShotDrainResult;
  int deleteResult;
  int postDeleteWriteResult;
  int postDeleteWaitResult;
  int postDeleteDrainResult;
  int eventFd;
  int eventFdAddResult;
  int eventFdFirstWriteResult;
  int eventFdFirstWaitResult;
  uint32_t eventFdFirstWaitEvents;
  uint64_t eventFdFirstWaitData;
  int eventFdSecondWriteResult;
  int eventFdSecondWaitResult;
  uint32_t eventFdSecondWaitEvents;
  uint64_t eventFdSecondWaitData;
  int eventFdReadResult;
  uint64_t eventFdReadValue;
  int eventFdDeleteResult;
  bool eventFdCloseResult;
  int aliasAddResult;
  bool originalCloseResult;
  size_t ownersAfterOriginalClose;
  int aliasWriteResult;
  int aliasWaitResult;
  uint32_t aliasWaitEvents;
  uint64_t aliasWaitData;
  int aliasDrainResult;
  bool aliasCloseResult;
  size_t ownersAfterAliasClose;
  int prunedWaitResult;
  size_t descriptionRefsAfterPrune;
};

class ReorderedReadinessFile final : public File {
 public:
  ReorderedReadinessFile()
      : File(), m_ReadinessLock(), m_Readable(true), m_Writable(true), m_Generations() {
    m_Generations.read = 1;
    m_Generations.write = 1;
  }

  ReadyMask queryReady(bool reading, bool writing) override {
    LockGuard<Mutex> guard(m_ReadinessLock);
    ReadyMask ready = ReadyNone;
    if (reading && m_Readable) {
      ready |= ReadyRead;
    }
    if (writing && m_Writable) {
      ready |= ReadyWrite;
    }
    return ready;
  }

  ReadinessGenerations readinessGenerations() override {
    LockGuard<Mutex> guard(m_ReadinessLock);
    return m_Generations;
  }

  bool supportsReadinessNotifications() const override {
    return true;
  }

  void setReady(bool readable, bool writable) {
    LockGuard<Mutex> guard(m_ReadinessLock);
    if (!m_Readable && readable) {
      ++m_Generations.read;
    }
    if (!m_Writable && writable) {
      ++m_Generations.write;
    }
    m_Readable = readable;
    m_Writable = writable;
  }

  void publishReadiness() {
    dataChanged();
  }

 private:
  Mutex m_ReadinessLock;
  bool m_Readable;
  bool m_Writable;
  ReadinessGenerations m_Generations;
};

struct ReorderedEpollContext {
  ReorderedEpollContext(const SharedPointer<EpollInstance>& instance, size_t fd,
                        ReorderedReadinessFile* file)
      : instance(instance),
        fd(fd),
        file(file),
        initialComplete(0, false),
        drainMutated(0, false),
        publishDrain(0, false),
        collectFinal(0, false),
        initialWaitResult(-2),
        initialWaitEvents(0),
        initialWaitData(0),
        finalWaitResult(-2),
        finalWaitEvents(0),
        finalWaitData(0),
        addResult(-2),
        deleteResult(-2),
        waiterReturned(0),
        drainReturned(0) {}

  SharedPointer<EpollInstance> instance;
  size_t fd;
  ReorderedReadinessFile* file;
  Semaphore initialComplete;
  Semaphore drainMutated;
  Semaphore publishDrain;
  Semaphore collectFinal;
  int initialWaitResult;
  uint32_t initialWaitEvents;
  uint64_t initialWaitData;
  int finalWaitResult;
  uint32_t finalWaitEvents;
  uint64_t finalWaitData;
  int addResult;
  int deleteResult;
  Atomic<size_t> waiterReturned;
  Atomic<size_t> drainReturned;
};

int waitAcrossReorderedReadiness(void* parameter) {
  ReorderedEpollContext* context = reinterpret_cast<ReorderedEpollContext*>(parameter);
  LinuxEpollEvent event = {LinuxEpoll::In | LinuxEpoll::Out | LinuxEpoll::EdgeTriggered,
                           EpollReorderedData};
  context->addResult =
      context->instance->control(LinuxEpoll::ControlAdd, static_cast<int>(context->fd), &event);

  LinuxEpollEvent result = {};
  context->initialWaitResult = context->instance->wait(&result, 1, 0);
  context->initialWaitEvents = result.events;
  context->initialWaitData = result.data;
  context->initialComplete.release();

  if (context->collectFinal.acquireForCompletion()) {
    result = {};
    context->finalWaitResult = context->instance->wait(&result, 1, 0);
    context->finalWaitEvents = result.events;
    context->finalWaitData = result.data;
    context->deleteResult = context->instance->control(LinuxEpoll::ControlDelete,
                                                       static_cast<int>(context->fd), nullptr);
  }

  context->waiterReturned += 1;
  return 0;
}

int publishDelayedDrain(void* parameter) {
  ReorderedEpollContext* context = reinterpret_cast<ReorderedEpollContext*>(parameter);
  context->file->setReady(false, false);
  context->drainMutated.release();
  if (context->publishDrain.acquireForCompletion()) {
    context->file->publishReadiness();
  }
  context->drainReturned += 1;
  return 0;
}

class ReorderedFifo final : public Pipe {
 public:
  ReorderedFifo() : Pipe(String("hosted-reordered-fifo"), 0, 0, 0, 0, nullptr, 0, nullptr, false) {}

  void reopenReaderWithoutPublishing() {
    LockGuard<Mutex> guard(m_Lock);
    m_Buffer.enableReads();
    ++m_nReaders;
    m_ReaderCondition.broadcast();
  }

  void publishReopen() {
    dataChanged();
  }
};

struct ReorderedFifoEpollContext {
  ReorderedFifoEpollContext(const SharedPointer<EpollInstance>& instance, size_t fd)
      : instance(instance),
        fd(fd),
        initialComplete(0, false),
        collectFinal(0, false),
        addResult(-2),
        initialWaitResult(-2),
        initialWaitEvents(0),
        initialWaitData(0),
        finalWaitResult(-2),
        finalWaitEvents(0),
        finalWaitData(0),
        deleteResult(-2),
        returned(0) {}

  SharedPointer<EpollInstance> instance;
  size_t fd;
  Semaphore initialComplete;
  Semaphore collectFinal;
  int addResult;
  int initialWaitResult;
  uint32_t initialWaitEvents;
  uint64_t initialWaitData;
  int finalWaitResult;
  uint32_t finalWaitEvents;
  uint64_t finalWaitData;
  int deleteResult;
  Atomic<size_t> returned;
};

int waitAcrossReorderedFifoReopen(void* parameter) {
  ReorderedFifoEpollContext* context = reinterpret_cast<ReorderedFifoEpollContext*>(parameter);
  LinuxEpollEvent interest = {LinuxEpoll::Out | LinuxEpoll::EdgeTriggered, EpollFifoData};
  context->addResult =
      context->instance->control(LinuxEpoll::ControlAdd, static_cast<int>(context->fd), &interest);

  LinuxEpollEvent event = {};
  context->initialWaitResult = context->instance->wait(&event, 1, 0);
  context->initialWaitEvents = event.events;
  context->initialWaitData = event.data;
  context->initialComplete.release();

  if (context->collectFinal.acquireForCompletion()) {
    event = {};
    context->finalWaitResult = context->instance->wait(&event, 1, 0);
    context->finalWaitEvents = event.events;
    context->finalWaitData = event.data;
    context->deleteResult = context->instance->control(LinuxEpoll::ControlDelete,
                                                       static_cast<int>(context->fd), nullptr);
  }

  context->returned += 1;
  return 0;
}

int pollPipeReadiness(void* parameter) {
  PipePollReadinessContext* context = reinterpret_cast<PipePollReadinessContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  char byte = 0;

  thread->setErrno(0);
  context->emptyReadResult = posix_read(context->readFd, &byte, 1);
  context->emptyReadError = thread->getErrno();

  struct pollfd readable = {static_cast<int>(context->readFd), POLLIN, 0};
  context->pollEntered += 1;
  context->pollEntryGate.release();
  context->firstPollResult = posix_poll_safe(&readable, 1, PollCloseReuseTimeoutMilliseconds);
  context->firstPollEvents = readable.revents;
  context->firstReadResult = posix_read(context->readFd, &byte, 1);

  char fill[PIPE_BUF_MAX] = {};
  context->fillWriteResult = posix_write(context->writeFd, fill, sizeof(fill), false);
  thread->setErrno(0);
  context->fullWriteResult = posix_write(context->writeFd, &byte, 1, false);
  context->fullWriteError = thread->getErrno();
  context->atomicSetupDrainResult = posix_read(context->readFd, &byte, 1);
  char vectorBytes[2] = {1, 2};
  struct iovec vectors[2] = {{&vectorBytes[0], 1}, {&vectorBytes[1], 1}};
  thread->setErrno(0);
  context->atomicVectorResult = posix_writev(context->writeFd, vectors, 2);
  context->atomicVectorError = thread->getErrno();
  context->drainResult = posix_read(context->readFd, fill, sizeof(fill));
  char overBoundary[PIPE_BUF_MAX + 1] = {};
  context->overBoundaryWriteResult =
      posix_write(context->writeFd, overBoundary, sizeof(overBoundary), false);
  context->overBoundaryDrainResult = posix_read(context->readFd, fill, sizeof(fill));
  context->firstPhaseReturned += 1;
  context->firstPhaseGate.release();

  if (!context->eofGate.acquireForCompletion()) {
    context->returned += 1;
    return 1;
  }

  struct pollfd eof = {static_cast<int>(context->readFd), POLLIN, 0};
  context->eofPollResult = posix_poll_safe(&eof, 1, PollCloseReuseTimeoutMilliseconds);
  context->eofPollEvents = eof.revents;
  context->eofReadResult = posix_read(context->readFd, &byte, 1);
  context->returned += 1;
  return 0;
}

bool pipePollReadiness(Process* kernelProcess) {
  constexpr size_t ReadDescriptor = 45;
  constexpr size_t WriteDescriptor = 46;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Pipe* pipe = new Pipe(String(""), 0, 0, 0, 0, nullptr, 0, nullptr, true);
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY | O_NONBLOCK);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY | O_NONBLOCK);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  PipePollReadinessContext context(ReadDescriptor, WriteDescriptor);
  Thread* worker = new Thread(process, pollPipeReadiness, &context, nullptr, false, true, true);
  worker->setName("hosted pipe poll readiness worker");
  const bool started = worker->start();

  const bool pollEntered = started && context.pollEntryGate.acquireForCompletion();
  bool pollBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && pollEntered; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (worker->getWaitDebugInfo(info) && info.queue && info.queued &&
        worker->getStatus() == Thread::Sleeping) {
      pollBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  char byte = 1;
  const bool initialWrite =
      pollBlocked && writer->write(1, reinterpret_cast<uintptr_t>(&byte)) == 1;
  const bool firstPhaseCompleted = started && context.firstPhaseGate.acquireForCompletion();

  DescriptorLease closingWriter;
  const bool writerAcquired = subsystem->acquireFileDescriptor(WriteDescriptor, closingWriter);
  const bool writerClosed =
      writerAcquired && subsystem->closeFileDescriptor(WriteDescriptor, closingWriter);
  closingWriter.reset();
  context.eofGate.release();

  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool nonblockingPassed =
      context.emptyReadResult == -1 && context.emptyReadError == Error::NoMoreProcesses &&
      context.fillWriteResult == PIPE_BUF_MAX && context.fullWriteResult == -1 &&
      context.fullWriteError == Error::NoMoreProcesses && context.atomicSetupDrainResult == 1 &&
      context.atomicVectorResult == -1 && context.atomicVectorError == Error::NoMoreProcesses &&
      context.drainResult == PIPE_BUF_MAX - 1 && context.overBoundaryWriteResult == PIPE_BUF_MAX &&
      context.overBoundaryDrainResult == PIPE_BUF_MAX && context.eofReadResult == 0;
  const bool readinessPassed =
      initialWrite && context.firstPollResult == 1 && (context.firstPollEvents & POLLIN) &&
      !(context.firstPollEvents & POLLHUP) && context.firstReadResult == 1 &&
      context.eofPollResult == 1 && (context.eofPollEvents & POLLHUP);
  bool passed = started && pollBlocked && firstPhaseCompleted && joined && writerClosed &&
                context.firstPhaseReturned == 1 && context.returned == 1 && nonblockingPassed &&
                readinessPassed;

  FileDescriptor::OpenFileDescriptionLease readerDescription = reader->acquireOpenFileDescription();
  DescriptorLease closingReader;
  const bool readerAcquired = subsystem->acquireFileDescriptor(ReadDescriptor, closingReader);
  const bool readerClosed =
      readerAcquired && subsystem->closeFileDescriptor(ReadDescriptor, closingReader);
  closingReader.reset();
  passed = passed && readerClosed && readerDescription->descriptorOwnerCount() == 0 &&
           readerDescription->getFile() == pipe && pipe->getReaderCount() == 0;
  readerDescription.reset();
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL pipe-poll-readiness: "
        "POLLIN wakeup, EOF hangup, or atomic nonblocking pipe I/O was incorrect");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS pipe-poll-readiness");
  return true;
}

int exerciseEpollReadiness(void* parameter) {
  EpollReadinessContext* context = reinterpret_cast<EpollReadinessContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  LinuxEpollEvent events[2] = {};
  char byte = 1;

  LinuxEpollEvent interest = {LinuxEpoll::In | LinuxEpoll::ReadNormal | LinuxEpoll::EdgeTriggered,
                              EpollInitialData};
  context->addResult =
      context->instance->control(LinuxEpoll::ControlAdd, context->readFd, &interest);

  thread->setErrno(0);
  context->regularAddResult =
      context->instance->control(LinuxEpoll::ControlAdd, context->regularFd, &interest);
  context->regularAddError = thread->getErrno();

  context->createdFd = posix_epoll_create1(LinuxEpoll::CloseOnExec);
  DescriptorLease createdDescriptor;
  if (context->createdFd >= 0) {
    context->createdDescriptorAcquired = context->subsystem->acquireFileDescriptor(
        static_cast<size_t>(context->createdFd), createdDescriptor);
    if (context->createdDescriptorAcquired) {
      context->createdStatusFlags = createdDescriptor->getStatusFlags();
      context->createdDescriptorFlags = createdDescriptor->getFlags();
      context->createdCloseResult = context->subsystem->closeFileDescriptor(
          static_cast<size_t>(context->createdFd), createdDescriptor);
    }
  }
  createdDescriptor.reset();

  thread->setErrno(0);
  context->duplicateAddResult =
      context->instance->control(LinuxEpoll::ControlAdd, context->readFd, &interest);
  context->duplicateAddError = thread->getErrno();

  LinuxEpollEvent exclusive = {LinuxEpoll::In | LinuxEpoll::Exclusive, EpollInitialData};
  thread->setErrno(0);
  context->exclusiveModifyResult =
      context->instance->control(LinuxEpoll::ControlModify, context->readFd, &exclusive);
  context->exclusiveModifyError = thread->getErrno();

  context->waitEntered += 1;
  context->waitEntryGate.release();
  context->firstWaitResult = context->instance->wait(events, 2, 5000);
  context->firstWaitEvents = events[0].events;
  context->firstWaitData = events[0].data;

  events[0] = {};
  context->repeatedWaitResult = context->instance->wait(events, 2, 0);
  context->repeatedWaitEvents = events[0].events;
  context->repeatedWaitData = events[0].data;
  context->sameReadyWriteResult = posix_write(context->writeFd, &byte, 1, false);
  context->sameReadyWaitResult = context->instance->wait(events, 2, 0);
  char edgeBytes[2] = {};
  context->firstDrainResult = posix_read(context->readFd, edgeBytes, sizeof(edgeBytes));
  context->drainedWaitResult = context->instance->wait(events, 2, 0);

  context->transitionWriteResult = posix_write(context->writeFd, &byte, 1, false);
  events[0] = {};
  context->transitionWaitResult = context->instance->wait(events, 2, 0);
  context->transitionWaitEvents = events[0].events;
  context->transitionWaitData = events[0].data;
  context->transitionDrainResult = posix_read(context->readFd, &byte, 1);

  LinuxEpollEvent level = {LinuxEpoll::In, EpollLevelData};
  context->levelModifyResult =
      context->instance->control(LinuxEpoll::ControlModify, context->readFd, &level);
  context->levelWriteResult = posix_write(context->writeFd, &byte, 1, false);
  events[0] = {};
  context->levelWaitResult = context->instance->wait(events, 2, 0);
  context->levelWaitEvents = events[0].events;
  context->levelWaitData = events[0].data;
  context->levelRepeatedWaitResult = context->instance->wait(events, 2, 0);
  context->levelDrainResult = posix_read(context->readFd, &byte, 1);

  LinuxEpollEvent oneShot = {LinuxEpoll::In | LinuxEpoll::EdgeTriggered | LinuxEpoll::OneShot,
                             EpollOneShotData};
  context->oneShotModifyResult =
      context->instance->control(LinuxEpoll::ControlModify, context->readFd, &oneShot);
  context->oneShotWriteResult = posix_write(context->writeFd, &byte, 1, false);
  events[0] = {};
  context->oneShotWaitResult = context->instance->wait(events, 2, 0);
  context->oneShotWaitEvents = events[0].events;
  context->oneShotWaitData = events[0].data;
  context->oneShotSuppressedResult = context->instance->wait(events, 2, 0);

  LinuxEpollEvent rearmed = {LinuxEpoll::In | LinuxEpoll::EdgeTriggered | LinuxEpoll::OneShot,
                             EpollRearmedData};
  context->rearmResult =
      context->instance->control(LinuxEpoll::ControlModify, context->readFd, &rearmed);
  events[0] = {};
  context->rearmedWaitResult = context->instance->wait(events, 2, 0);
  context->rearmedWaitEvents = events[0].events;
  context->rearmedWaitData = events[0].data;
  context->oneShotDrainResult = posix_read(context->readFd, &byte, 1);

  context->deleteResult =
      context->instance->control(LinuxEpoll::ControlDelete, context->readFd, nullptr);
  context->postDeleteWriteResult = posix_write(context->writeFd, &byte, 1, false);
  context->postDeleteWaitResult = context->instance->wait(events, 2, 0);
  context->postDeleteDrainResult = posix_read(context->readFd, &byte, 1);

  context->eventFd = posix_eventfd2(0, LinuxEventFd::NonBlock);
  LinuxEpollEvent eventFdInterest = {LinuxEpoll::In | LinuxEpoll::EdgeTriggered, EpollEventFdData};
  if (context->eventFd >= 0) {
    context->eventFdAddResult =
        context->instance->control(LinuxEpoll::ControlAdd, context->eventFd, &eventFdInterest);
    uint64_t eventValue = 1;
    context->eventFdFirstWriteResult = posix_write(
        context->eventFd, reinterpret_cast<char*>(&eventValue), sizeof(eventValue), false);
    events[0] = {};
    context->eventFdFirstWaitResult = context->instance->wait(events, 2, 0);
    context->eventFdFirstWaitEvents = events[0].events;
    context->eventFdFirstWaitData = events[0].data;

    // libuv deliberately leaves its Linux async eventfd readable. A second
    // producer write must therefore generate another edge without a read in
    // between, even though the counter's readable level never went false.
    context->eventFdSecondWriteResult = posix_write(
        context->eventFd, reinterpret_cast<char*>(&eventValue), sizeof(eventValue), false);
    events[0] = {};
    context->eventFdSecondWaitResult = context->instance->wait(events, 2, 0);
    context->eventFdSecondWaitEvents = events[0].events;
    context->eventFdSecondWaitData = events[0].data;
    context->eventFdReadResult =
        posix_read(context->eventFd, reinterpret_cast<char*>(&context->eventFdReadValue),
                   sizeof(context->eventFdReadValue));
    context->eventFdDeleteResult =
        context->instance->control(LinuxEpoll::ControlDelete, context->eventFd, nullptr);

    DescriptorLease closingEventFd;
    const bool eventFdAcquired = context->subsystem->acquireFileDescriptor(
        static_cast<size_t>(context->eventFd), closingEventFd);
    context->eventFdCloseResult =
        eventFdAcquired && context->subsystem->closeFileDescriptor(
                               static_cast<size_t>(context->eventFd), closingEventFd);
    closingEventFd.reset();
  }

  LinuxEpollEvent aliasLifetime = {LinuxEpoll::In, EpollAliasData};
  context->aliasAddResult =
      context->instance->control(LinuxEpoll::ControlAdd, context->readFd, &aliasLifetime);

  DescriptorLease closingOriginal;
  const bool originalAcquired =
      context->subsystem->acquireFileDescriptor(context->readFd, closingOriginal);
  context->originalCloseResult =
      originalAcquired && context->subsystem->closeFileDescriptor(context->readFd, closingOriginal);
  closingOriginal.reset();
  context->ownersAfterOriginalClose = context->readDescription->descriptorOwnerCount();

  context->aliasWriteResult = posix_write(context->writeFd, &byte, 1, false);
  events[0] = {};
  context->aliasWaitResult = context->instance->wait(events, 2, 0);
  context->aliasWaitEvents = events[0].events;
  context->aliasWaitData = events[0].data;
  context->aliasDrainResult = posix_read(context->aliasFd, &byte, 1);

  DescriptorLease closingAlias;
  const bool aliasAcquired =
      context->subsystem->acquireFileDescriptor(context->aliasFd, closingAlias);
  context->aliasCloseResult =
      aliasAcquired && context->subsystem->closeFileDescriptor(context->aliasFd, closingAlias);
  closingAlias.reset();
  context->ownersAfterAliasClose = context->readDescription->descriptorOwnerCount();
  context->prunedWaitResult = context->instance->wait(events, 2, 0);
  context->descriptionRefsAfterPrune = context->readDescription.refcount();

  context->returned += 1;
  return 0;
}

bool epollLevelOneShotAndOfdLifetime(Process* kernelProcess) {
  constexpr size_t ReadDescriptor = 47;
  constexpr size_t WriteDescriptor = 48;
  constexpr size_t AliasDescriptor = 49;
  constexpr size_t RegularDescriptor = 50;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Pipe* pipe = new Pipe(String(""), 0, 0, 0, 0, nullptr, 0, nullptr, true);
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY | O_NONBLOCK);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY | O_NONBLOCK);
  FileDescriptor* alias = new FileDescriptor(*reader);
  alias->fd = AliasDescriptor;
  File* regularFile = new File;
  FileDescriptor* regular = new FileDescriptor(regularFile, 0, RegularDescriptor, 0, O_RDONLY);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);
  subsystem->addFileDescriptor(AliasDescriptor, alias);
  subsystem->addFileDescriptor(RegularDescriptor, regular);

  SharedPointer<EpollInstance> instance(new EpollInstance);
  EpollReadinessContext context(subsystem, instance, ReadDescriptor, WriteDescriptor,
                                AliasDescriptor, RegularDescriptor);
  context.readDescription = reader->acquireOpenFileDescription();

  Thread* worker =
      new Thread(process, exerciseEpollReadiness, &context, nullptr, false, true, true);
  worker->setName("hosted epoll readiness worker");
  const bool started = worker->start();
  const bool waitEntered = started && context.waitEntryGate.acquireForCompletion();

  bool waitBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && waitEntered; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (worker->getWaitDebugInfo(info) && info.queue && info.queued &&
        worker->getStatus() == Thread::Sleeping) {
      waitBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  char byte = 1;
  const int wakeWriteResult = writer->write(1, reinterpret_cast<uintptr_t>(&byte));
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool edgePassed =
      waitBlocked && wakeWriteResult == 1 && context.firstWaitResult == 1 &&
      context.firstWaitEvents == (LinuxEpoll::In | LinuxEpoll::ReadNormal) &&
      context.firstWaitData == EpollInitialData && context.repeatedWaitResult == 0 &&
      context.sameReadyWriteResult == 1 && context.sameReadyWaitResult == 0 &&
      context.firstDrainResult == 2 && context.drainedWaitResult == 0 &&
      context.transitionWriteResult == 1 && context.transitionWaitResult == 1 &&
      context.transitionWaitEvents == (LinuxEpoll::In | LinuxEpoll::ReadNormal) &&
      context.transitionWaitData == EpollInitialData && context.transitionDrainResult == 1;
  const bool levelPassed = context.levelModifyResult == 0 && context.levelWriteResult == 1 &&
                           context.levelWaitResult == 1 &&
                           context.levelWaitEvents == LinuxEpoll::In &&
                           context.levelWaitData == EpollLevelData &&
                           context.levelRepeatedWaitResult == 1 && context.levelDrainResult == 1;
  const bool controlPassed =
      context.addResult == 0 && context.regularAddResult == -1 &&
      context.regularAddError == Error::NotEnoughPermissions && context.createdFd >= 0 &&
      context.createdDescriptorAcquired && context.createdStatusFlags == O_RDWR &&
      context.createdDescriptorFlags == FD_CLOEXEC && context.createdCloseResult &&
      context.duplicateAddResult == -1 && context.duplicateAddError == Error::FileExists &&
      context.exclusiveModifyResult == -1 &&
      context.exclusiveModifyError == Error::OperationNotSupported &&
      context.oneShotModifyResult == 0 && context.oneShotWriteResult == 1 &&
      context.oneShotWaitResult == 1 && context.oneShotWaitEvents == LinuxEpoll::In &&
      context.oneShotWaitData == EpollOneShotData && context.oneShotSuppressedResult == 0 &&
      context.rearmResult == 0 && context.rearmedWaitResult == 1 &&
      context.rearmedWaitEvents == LinuxEpoll::In && context.rearmedWaitData == EpollRearmedData &&
      context.oneShotDrainResult == 1 && context.deleteResult == 0 &&
      context.postDeleteWriteResult == 1 && context.postDeleteWaitResult == 0 &&
      context.postDeleteDrainResult == 1;
  const bool lifetimePassed =
      context.aliasAddResult == 0 && context.originalCloseResult &&
      context.ownersAfterOriginalClose == 1 && context.aliasWriteResult == 1 &&
      context.aliasWaitResult == 1 && context.aliasWaitEvents == LinuxEpoll::In &&
      context.aliasWaitData == EpollAliasData && context.aliasDrainResult == 1 &&
      context.aliasCloseResult && context.ownersAfterAliasClose == 0 &&
      context.prunedWaitResult == 0 && context.descriptionRefsAfterPrune == 1;
  const bool eventFdPassed =
      context.eventFd >= 0 && context.eventFdAddResult == 0 &&
      context.eventFdFirstWriteResult == static_cast<int>(sizeof(uint64_t)) &&
      context.eventFdFirstWaitResult == 1 && context.eventFdFirstWaitEvents == LinuxEpoll::In &&
      context.eventFdFirstWaitData == EpollEventFdData &&
      context.eventFdSecondWriteResult == static_cast<int>(sizeof(uint64_t)) &&
      context.eventFdSecondWaitResult == 1 && context.eventFdSecondWaitEvents == LinuxEpoll::In &&
      context.eventFdSecondWaitData == EpollEventFdData &&
      context.eventFdReadResult == static_cast<int>(sizeof(uint64_t)) &&
      context.eventFdReadValue == 2 && context.eventFdDeleteResult == 0 &&
      context.eventFdCloseResult;
  bool passed = started && waitEntered && joined && context.waitEntered == 1 &&
                context.returned == 1 && edgePassed && levelPassed && controlPassed &&
                lifetimePassed && eventFdPassed;

  DescriptorLease closingWriter;
  const bool writerAcquired = subsystem->acquireFileDescriptor(WriteDescriptor, closingWriter);
  const bool writerClosed =
      writerAcquired && subsystem->closeFileDescriptor(WriteDescriptor, closingWriter);
  closingWriter.reset();
  passed = passed && writerClosed;

  instance.reset();
  context.instance.reset();
  context.readDescription.reset();
  delete process;
  delete regularFile;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL epoll-level-oneshot-ofd-lifetime: "
        "target admission, descriptor flags, edge and level delivery, one-shot rearm, control "
        "errors, eventfd generations, or OFD retirement was incorrect");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS epoll-level-oneshot-ofd-lifetime");
  return true;
}

bool epollReorderedTransitionPublication(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 51;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  ReorderedReadinessFile* source = new ReorderedReadinessFile;
  FileDescriptor* descriptor =
      new FileDescriptor(source, 0, DescriptorNumber, 0, O_RDWR | O_NONBLOCK);
  subsystem->addFileDescriptor(DescriptorNumber, descriptor);

  SharedPointer<EpollInstance> instance(new EpollInstance);
  ReorderedEpollContext context(instance, DescriptorNumber, source);
  Thread* waiter =
      new Thread(process, waitAcrossReorderedReadiness, &context, nullptr, false, true, true);
  waiter->setName("hosted reordered epoll waiter");
  const bool waiterStarted = waiter->start();
  const bool initialComplete = waiterStarted && context.initialComplete.acquireForCompletion();

  Thread* drain = nullptr;
  bool drainStarted = false;
  bool drainMutated = false;
  if (initialComplete) {
    drain = new Thread(process, publishDelayedDrain, &context, nullptr, false, true, true);
    drain->setName("hosted delayed epoll drain publisher");
    drainStarted = drain->start();
    drainMutated = drainStarted && context.drainMutated.acquireForCompletion();
  }

  // The refill callback deliberately overtakes the callback for the drain.
  // Both callbacks therefore sample the final readable level; only the
  // source-side generation proves that a reusable edge occurred between them.
  if (drainMutated) {
    source->setReady(true, true);
    source->publishReadiness();
  }
  context.publishDrain.release();
  const bool drainJoined = drainStarted && drain->joinForCompletion();
  if (drain && !drainStarted) {
    delete drain;
  }

  context.collectFinal.release();
  const bool waiterJoined = waiterStarted && waiter->joinForCompletion();
  if (!waiterStarted) {
    delete waiter;
  }

  const bool transitionPassed =
      context.addResult == 0 && context.initialWaitResult == 1 &&
      context.initialWaitEvents == (LinuxEpoll::In | LinuxEpoll::Out) &&
      context.initialWaitData == EpollReorderedData && context.finalWaitResult == 1 &&
      context.finalWaitEvents == (LinuxEpoll::In | LinuxEpoll::Out) &&
      context.finalWaitData == EpollReorderedData && context.deleteResult == 0 &&
      source->readinessGenerations().read == 2 && source->readinessGenerations().write == 2;
  bool passed = waiterStarted && initialComplete && drainStarted && drainMutated && drainJoined &&
                waiterJoined && context.waiterReturned == 1 && context.drainReturned == 1 &&
                transitionPassed;

  DescriptorLease closing;
  const bool descriptorAcquired = subsystem->acquireFileDescriptor(DescriptorNumber, closing);
  const bool descriptorClosed =
      descriptorAcquired && subsystem->closeFileDescriptor(DescriptorNumber, closing);
  closing.reset();
  passed = passed && descriptorClosed;

  instance.reset();
  context.instance.reset();
  delete process;
  delete source;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL epoll-reordered-transition-publication: "
        "a refill edge was lost when its callback overtook the drain callback");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS epoll-reordered-transition-publication");
  return true;
}

bool epollPersistentFifoReopenReclose(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 52;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  ReorderedFifo* fifo = new ReorderedFifo;
  FileDescriptor* writer = new FileDescriptor(fifo, 0, DescriptorNumber, 0, O_WRONLY | O_NONBLOCK);
  subsystem->addFileDescriptor(DescriptorNumber, writer);

  fifo->increaseRefCount(false);
  char fill[PIPE_BUF_MAX] = {};
  const int fillResult = writer->write(sizeof(fill), reinterpret_cast<uintptr_t>(fill));
  fifo->decreaseRefCount(false);

  SharedPointer<EpollInstance> instance(new EpollInstance);
  ReorderedFifoEpollContext context(instance, DescriptorNumber);
  Thread* waiter =
      new Thread(process, waitAcrossReorderedFifoReopen, &context, nullptr, false, true, true);
  waiter->setName("hosted reordered FIFO epoll waiter");
  const bool waiterStarted = waiter->start();
  const bool initialComplete = waiterStarted && context.initialComplete.acquireForCompletion();

  const ReadinessGenerations initialGenerations = fifo->readinessGenerations();
  bool reopenMadeNotReady = false;
  if (initialComplete) {
    // Delay the notification for the falling reader-reopen transition. The
    // re-close publishes first, so both callbacks sample the final OUT|ERR
    // level and the Pipe generation is the only evidence of the OUT rise.
    fifo->reopenReaderWithoutPublishing();
    reopenMadeNotReady = fifo->queryReady(false, true) == ReadyNone;
    fifo->decreaseRefCount(false);
    fifo->publishReopen();
  }
  const ReadinessGenerations finalGenerations = fifo->readinessGenerations();

  context.collectFinal.release();
  const bool waiterJoined = waiterStarted && waiter->joinForCompletion();
  if (!waiterStarted) {
    delete waiter;
  }

  const uint32_t expectedEvents = LinuxEpoll::Out | LinuxEpoll::Error;
  const bool edgePassed =
      fillResult == PIPE_BUF_MAX && context.addResult == 0 && context.initialWaitResult == 1 &&
      context.initialWaitEvents == expectedEvents && context.initialWaitData == EpollFifoData &&
      reopenMadeNotReady && context.finalWaitResult == 1 &&
      context.finalWaitEvents == expectedEvents && context.finalWaitData == EpollFifoData &&
      context.deleteResult == 0 && finalGenerations.write != initialGenerations.write &&
      finalGenerations.error != initialGenerations.error;
  bool passed =
      waiterStarted && initialComplete && waiterJoined && context.returned == 1 && edgePassed;

  DescriptorLease closing;
  const bool descriptorAcquired = subsystem->acquireFileDescriptor(DescriptorNumber, closing);
  const bool descriptorClosed =
      descriptorAcquired && subsystem->closeFileDescriptor(DescriptorNumber, closing);
  closing.reset();
  passed = passed && descriptorClosed;

  instance.reset();
  context.instance.reset();
  delete process;
  delete fifo;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL epoll-persistent-fifo-reopen-reclose: "
        "a full FIFO lost EPOLLOUT when reader re-close overtook reopen publication");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS epoll-persistent-fifo-reopen-reclose");
  return true;
}

int pollAcrossCloseReuse(void* parameter) {
  PollCloseReuseContext* context = reinterpret_cast<PollCloseReuseContext*>(parameter);
  context->entered += 1;
  // A missed wakeup must report a bounded regression failure. The old
  // infinite poll made the outer harness timeout the only evidence.
  context->result = posix_poll_safe(&context->descriptor, 1, PollCloseReuseTimeoutMilliseconds);
  context->returned += 1;
  return context->result == 1 ? 0 : 1;
}

bool pollCloseReuseCleanup(Process* kernelProcess) {
  constexpr size_t DescriptorNumber = 39;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  Atomic<size_t> aQueries(0);
  Atomic<size_t> aNotifications(0);
  Atomic<size_t> aNetworkDestructions(0);
  Atomic<size_t> aDescriptorDestructions(0);
  PollGenerationProbe* aNetwork =
      new PollGenerationProbe(aQueries, aNotifications, aNetworkDestructions);
  SharedPointer<NetworkSyscalls> aNetworkKeepalive(aNetwork);
  DescriptorRetirementProbe* aDescriptor = new DescriptorRetirementProbe(aDescriptorDestructions);
  aDescriptor->fd = DescriptorNumber;
  aDescriptor->setOffset(1);
  aDescriptor->setNetworkImpl(aNetworkKeepalive);
  subsystem->addFileDescriptor(DescriptorNumber, aDescriptor);

  Atomic<size_t> bQueries(0);
  Atomic<size_t> bNotifications(0);
  Atomic<size_t> bNetworkDestructions(0);
  Atomic<size_t> bDescriptorDestructions(0);
  PollGenerationProbe* bNetwork =
      new PollGenerationProbe(bQueries, bNotifications, bNetworkDestructions);
  SharedPointer<NetworkSyscalls> bNetworkKeepalive(bNetwork);
  DescriptorRetirementProbe* bDescriptor = new DescriptorRetirementProbe(bDescriptorDestructions);
  bDescriptor->fd = DescriptorNumber;
  bDescriptor->setOffset(2);
  bDescriptor->setNetworkImpl(bNetworkKeepalive);

  PollCloseReuseContext context(DescriptorNumber);
  Thread* worker = new Thread(process, pollAcrossCloseReuse, &context, nullptr, false, true, true);
  worker->setName("hosted poll close-reuse worker");
  const bool started = worker->start();
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "worker-started");
  bool blockedOnA = false;
  for (size_t attempt = 0; attempt < HostedAttempts && started; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context.entered && aQueries >= 2 && worker->getWaitDebugInfo(info) && info.queue &&
        info.queued && worker->getStatus() == Thread::Sleeping) {
      blockedOnA = true;
      break;
    }
    Scheduler::instance().yield();
  }

  bool passed = started && blockedOnA && aQueries >= 2;
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "waiter-published-a blocked="
      << blockedOnA << " queries=" << aQueries.value());
  DescriptorLease closingA;
  const bool acquiredA = subsystem->acquireFileDescriptor(DescriptorNumber, closingA);
  const bool closedA = acquiredA && subsystem->closeFileDescriptor(DescriptorNumber, closingA);
  closingA.reset();
  subsystem->addFileDescriptor(DescriptorNumber, bDescriptor);
  passed = passed && closedA && aDescriptorDestructions == 0 && aNetworkDestructions == 0;
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "closed-a-published-b");

  if (aQueries >= 2) {
    aNetwork->makeReadable();
  } else {
    // Failure cleanup: if the worker did not pin A, allow any lookup of B
    // to finish rather than leaving the hosted smoke run blocked.
    bNetwork->makeReadable();
  }
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "release-published");

  const bool joined = started && worker->joinForCompletion();
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "worker-returned joined="
      << joined << " returned=" << context.returned.value()
      << " result=" << context.result.value());
  passed = passed && joined && context.returned == 1 && context.result == 1 &&
           (context.descriptor.revents & POLLIN) && aNotifications == 1 && bNotifications == 0 &&
           bQueries == 0 && aDescriptorDestructions == 1 && aNetworkDestructions == 0;
  aNetworkKeepalive.reset();
  passed = passed && aNetworkDestructions == 1;

  DescriptorLease closingB;
  const bool acquiredB = subsystem->acquireFileDescriptor(DescriptorNumber, closingB);
  const bool closedB = acquiredB && subsystem->closeFileDescriptor(DescriptorNumber, closingB);
  closingB.reset();
  passed = passed && closedB && bDescriptorDestructions == 1 && bNetworkDestructions == 0;
  bNetworkKeepalive.reset();
  passed = passed && bNetworkDestructions == 1;

  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL poll-close-reuse-cleanup: "
        "poll cleanup followed the reused fd instead of its registered "
        "descriptor generation");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS poll-close-reuse-cleanup");
  return true;
}

struct PosixTeardownContext {
  explicit PosixTeardownContext(Process* process)
      : process(process),
        releaseGate(0, false),
        holderEntered(0),
        holderReturned(0),
        reaperEntered(0),
        processDeleted(0) {}

  Process* process;
  Semaphore releaseGate;
  Atomic<size_t> holderEntered;
  Atomic<size_t> holderReturned;
  Atomic<size_t> reaperEntered;
  Atomic<size_t> processDeleted;
};

int holdMemoryMapLifecycleGate(void* parameter) {
  PosixTeardownContext* context = reinterpret_cast<PosixTeardownContext*>(parameter);
  MemoryMapManager::instance().acquireLifecycleGateForHostedTest();
  context->holderEntered += 1;
  const bool released = context->releaseGate.acquireForCompletion();
  MemoryMapManager::instance().releaseLifecycleGateForHostedTest();
  context->holderReturned += 1;
  return released ? 0 : 1;
}

int deletePosixProcess(void* parameter) {
  PosixTeardownContext* context = reinterpret_cast<PosixTeardownContext*>(parameter);
  context->reaperEntered += 1;
  delete context->process;
  context->processDeleted += 1;
  return 0;
}

bool posixTeardownContention(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  PosixTeardownContext context(process);

  Thread* holder =
      new Thread(kernelProcess, holdMemoryMapLifecycleGate, &context, nullptr, false, true, true);
  holder->setName("hosted mmap lifecycle holder");
  bool passed = holder->start();

  for (size_t attempt = 0; attempt < HostedAttempts && passed && !context.holderEntered;
       ++attempt) {
    Scheduler::instance().yield();
  }
  passed = passed && context.holderEntered == 1;

  Thread* reaper = nullptr;
  bool blocked = false;
  if (passed) {
    reaper = new Thread(kernelProcess, deletePosixProcess, &context, nullptr, false, true, true);
    reaper->setName("hosted POSIX process reaper");
    passed = reaper->start();

    for (size_t attempt = 0; attempt < HostedAttempts && passed; ++attempt) {
      Thread::WaitDebugInfo info = {};
      if (context.reaperEntered && reaper->getWaitDebugInfo(info) && info.queue && info.queued &&
          info.channelOwner == MemoryMapManager::instance().lifecycleGateAddressForHostedTest() &&
          reaper->getStatus() == Thread::Sleeping) {
        blocked = true;
        break;
      }
      Scheduler::instance().yield();
    }
    passed = passed && blocked && context.processDeleted == 0;
  }

  context.releaseGate.release();
  passed = holder->join() && passed;
  if (reaper) {
    passed = reaper->join() && passed;
    passed = passed && context.processDeleted == 1;
  } else {
    delete process;
  }

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-teardown-contention: "
        "real PosixSubsystem destruction did not sleep and resume on "
        "the memory-map lifecycle gate");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-teardown-contention");
  return true;
}

bool zeroResultWinsSignal(Thread* thread) {
  thread->setErrno(0);
  thread->setInterruptionReason(Thread::InterruptedBySignal);
  const bool completed = finishInterruptibleSocketCall(thread, static_cast<ssize_t>(0));
  const bool passed =
      completed && thread->getInterruptionReason() == Thread::NotInterrupted && !thread->getErrno();
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL socket-zero-result-signal: "
        "EOF or zero-length success was replaced with EINTR");
    thread->clearInterruption();
    thread->setErrno(0);
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS socket-zero-result-signal");
  return true;
}

bool cloneStateDropsParentErrnoDestination() {
  long error = 0;
  SyscallState parent = {};
  parent.error_ptr = reinterpret_cast<uintptr_t>(&error);
  parent.result = 37;

  const SyscallState child = posix_copy_clone_state(parent);
  const bool passed = !child.error_ptr && child.result == parent.result &&
                      parent.error_ptr == reinterpret_cast<uintptr_t>(&error);
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL clone-errno-lifetime: "
        "the child retained its parent's stack-local errno destination");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS clone-errno-lifetime");
  return true;
}

enum CloneVmBeforeStartAction {
  CancelChildBeforeStart,
  WaitForProcessExit,
};

struct CloneVmExitRaceContext {
  explicit CloneVmExitRaceContext(CloneVmBeforeStartAction action)
      : process(nullptr),
        caller(nullptr),
        terminator(nullptr),
        child(nullptr),
        action(action),
        beforeStart(0, true),
        parentTid(-1),
        childTid(-1),
        observedTid(0),
        childTls(0),
        callerEntered(0),
        callerReturned(0),
        terminatorEntered(0),
        terminatorReturned(0),
        hookCalls(0),
        tidsReady(0),
        terminationElectionCalls(0),
        ownershipWindowReleased(0),
        ownershipCancellationObserved(0),
        ownershipStartObserved(0),
        controlledHookRelease(0),
        electionTimedOut(0),
        schedulerPredicateInstalled(0),
        schedulerPredicateInstallFailed(0),
        schedulerPredicateReleased(0),
        cloneResult(static_cast<size_t>(-1)),
        childCancellationRequested(0),
        childCancellationReapable(0),
        terminatorStarted(0),
        terminalCancellation(0),
        unexpectedHookRelease(0),
        hookTimedOut(0),
        rescueCancellation(0),
        processDestructions(0),
        subsystemDestructions(0) {}

  Process* process;
  Thread* caller;
  Thread* terminator;
  Atomic<Thread*> child;
  CloneVmBeforeStartAction action;
  Semaphore beforeStart;
  int parentTid;
  int childTid;
  size_t observedTid;
  alignas(uintptr_t) uintptr_t childTls;
  Atomic<size_t> callerEntered;
  Atomic<size_t> callerReturned;
  Atomic<size_t> terminatorEntered;
  Atomic<size_t> terminatorReturned;
  Atomic<size_t> hookCalls;
  Atomic<size_t> tidsReady;
  Atomic<size_t> terminationElectionCalls;
  Atomic<size_t> ownershipWindowReleased;
  Atomic<size_t> ownershipCancellationObserved;
  Atomic<size_t> ownershipStartObserved;
  Atomic<size_t> controlledHookRelease;
  Atomic<size_t> electionTimedOut;
  Atomic<size_t> schedulerPredicateInstalled;
  Atomic<size_t> schedulerPredicateInstallFailed;
  Atomic<size_t> schedulerPredicateReleased;
  Atomic<size_t> cloneResult;
  Atomic<size_t> childCancellationRequested;
  Atomic<size_t> childCancellationReapable;
  Atomic<size_t> terminatorStarted;
  Atomic<size_t> terminalCancellation;
  Atomic<size_t> unexpectedHookRelease;
  Atomic<size_t> hookTimedOut;
  Atomic<size_t> rescueCancellation;
  Atomic<size_t> processDestructions;
  Atomic<size_t> subsystemDestructions;
  alignas(16) uint8_t childStack[4096];
};

CloneVmExitRaceContext* g_CloneVmExitRaceContext = nullptr;

class CloneVmExitRaceProcess final : public PosixProcess {
 public:
  CloneVmExitRaceProcess(Process* parent, Atomic<size_t>& destructions)
      : PosixProcess(parent), m_Destructions(destructions) {}

  ~CloneVmExitRaceProcess() override {
    m_Destructions += 1;
  }

 private:
  Atomic<size_t>& m_Destructions;
};

class CloneVmExitRaceSubsystem final : public PosixSubsystem {
 public:
  explicit CloneVmExitRaceSubsystem(Atomic<size_t>& destructions)
      : PosixSubsystem(), m_Destructions(destructions) {}

  ~CloneVmExitRaceSubsystem() override {
    m_Destructions += 1;
  }

 private:
  Atomic<size_t>& m_Destructions;
};

int terminateCloneVmProcess(void* parameter) {
  CloneVmExitRaceContext* context = reinterpret_cast<CloneVmExitRaceContext*>(parameter);
  context->terminatorEntered += 1;
  SyscallManager::instance().syscall(posix, POSIX_EXIT_GROUP, 0);
  context->terminatorReturned += 1;
  return 1;
}

bool cloneVmChildReady(void* parameter) {
  CloneVmExitRaceContext* context = reinterpret_cast<CloneVmExitRaceContext*>(parameter);
  if (!context || !context->schedulerPredicateReleased) {
    return false;
  }
  Thread* child = context->child.value();
  return child && child->getUnwindState() == Thread::TerminateThread;
}

void terminateCloneVmBeforeStart(Thread* child, size_t threadId, void* parameter) {
  CloneVmExitRaceContext* context = reinterpret_cast<CloneVmExitRaceContext*>(parameter);
  if (!context || !child || child->getParent() != context->process) {
    return;
  }
  context->child = child;
  context->observedTid = threadId;
  if (child->getId() == threadId && context->parentTid == static_cast<int>(threadId) &&
      context->childTid == static_cast<int>(threadId) &&
      context->childTls == reinterpret_cast<uintptr_t>(&context->childTls)) {
    context->tidsReady += 1;
  }
  context->hookCalls += 1;

  if (context->action == WaitForProcessExit) {
    if (child->setSchedulerReadyPredicate(cloneVmChildReady, context)) {
      context->schedulerPredicateInstalled += 1;
    } else {
      context->schedulerPredicateInstallFailed += 1;
      context->rescueCancellation = 1;
    }
  }

  bool cancelChild = context->action == CancelChildBeforeStart || context->rescueCancellation;
  if (!cancelChild) {
    const bool released = context->beforeStart.acquire(1, 5, 0);
    cancelChild = context->rescueCancellation;
    if (!cancelChild) {
      if (released && context->ownershipWindowReleased) {
        context->controlledHookRelease += 1;
        return;
      }
      Thread* current = Processor::information().getCurrentThread();
      if (!released && current && current->getUnwindState() == Thread::TerminateThread) {
        context->terminalCancellation += 1;
      } else {
        context->unexpectedHookRelease += 1;
        if (!released) {
          context->hookTimedOut += 1;
        }
      }
      return;
    }
  }

  child->setUnwindState(Thread::TerminateThread);
  context->schedulerPredicateReleased = 1;
  context->childCancellationRequested += 1;
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (child->isReapableForHostedTest()) {
      context->childCancellationReapable += 1;
      if (context->schedulerPredicateInstallFailed) {
        context->child = nullptr;
      }
      return;
    }
    Scheduler::instance().yield();
  }
  context->hookTimedOut += 1;
  if (context->schedulerPredicateInstallFailed) {
    context->child = nullptr;
  }
}

void observeCloneVmTerminationElection(Process* process, Thread* owner) {
  CloneVmExitRaceContext* context = __atomic_load_n(&g_CloneVmExitRaceContext, __ATOMIC_ACQUIRE);
  if (!context || process != context->process || owner != context->terminator) {
    return;
  }
  if (context->schedulerPredicateInstallFailed) {
    return;
  }

  context->terminationElectionCalls += 1;
  context->ownershipWindowReleased = 1;
  context->beforeStart.release();
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    Thread* child = context->child.value();
    if (child && context->callerReturned) {
      if (child->getUnwindState() == Thread::TerminateThread &&
          !child->wasStartPublishedForHostedTest()) {
        context->ownershipCancellationObserved += 1;
      } else {
        context->ownershipStartObserved += 1;
      }
      context->schedulerPredicateReleased = 1;
      return;
    }
    Scheduler::instance().yield();
  }
  context->electionTimedOut += 1;
  context->schedulerPredicateReleased = 1;
}

void clearCloneVmHooks() {
  posixSetCloneBeforeStartHookForTest(nullptr, nullptr);
  Process::setTerminationElectionHook(nullptr);
  __atomic_store_n(&g_CloneVmExitRaceContext, static_cast<CloneVmExitRaceContext*>(nullptr),
                   __ATOMIC_RELEASE);
}

int cloneVmWhileProcessExits(void* parameter) {
  CloneVmExitRaceContext* context = reinterpret_cast<CloneVmExitRaceContext*>(parameter);
  context->callerEntered += 1;
  context->cloneResult = SyscallManager::instance().syscall(
      posix, POSIX_CLONE, CLONE_VM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID,
      reinterpret_cast<uintptr_t>(context->childStack + sizeof(context->childStack)),
      reinterpret_cast<uintptr_t>(&context->parentTid),
      reinterpret_cast<uintptr_t>(&context->childTid),
      reinterpret_cast<uintptr_t>(&context->childTls));
  context->callerReturned += 1;
  return 1;
}

bool waitForCloneVmHookPause(CloneVmExitRaceContext* context) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context->hookCalls == static_cast<size_t>(1) && context->caller->getWaitDebugInfo(info) &&
        info.queue && info.queued && info.channelOwner == &context->beforeStart &&
        context->caller->getStatus() == Thread::Sleeping) {
      return true;
    }
    if (context->unexpectedHookRelease || context->hookTimedOut || context->callerReturned) {
      return false;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitForCloneVmThreadReapable(Thread* thread) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (thread->isReapableForHostedTest()) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitForCloneVmThreadCount(Process* process, size_t count) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (process->getNumThreads() == count) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

NORETURN void fatalCloneVmFixture(const char* detail) {
  clearCloneVmHooks();
  FATAL("HOSTED-SYSCALL-TEST: clone fixture could not retire safely: " << detail);
  panic(detail);
}

bool cloneVmDetachedCancellationReturnsCachedTid(Process* kernelProcess) {
  CloneVmExitRaceContext* context = new CloneVmExitRaceContext(CancelChildBeforeStart);
  CloneVmExitRaceProcess* process =
      new CloneVmExitRaceProcess(kernelProcess, context->processDestructions);
  process->setSubsystem(new CloneVmExitRaceSubsystem(context->subsystemDestructions));
  process->description() = "hosted clone detached-cancellation fixture";

  context->process = process;
  context->caller =
      new Thread(process, cloneVmWhileProcessExits, context, nullptr, false, true, true);
  context->caller->setName("hosted clone detached-cancellation caller");
  process->publish();

  posixSetCloneBeforeStartHookForTest(terminateCloneVmBeforeStart, context);
  const bool callerStarted = context->caller->start();
  bool callerReapable = callerStarted && waitForCloneVmThreadReapable(context->caller);
  if (!callerReapable) {
    context->rescueCancellation = 1;
    context->caller->setUnwindState(Thread::TerminateThread);
    context->beforeStart.release();
    callerReapable = waitForCloneVmThreadReapable(context->caller);
  }
  if (!callerReapable) {
    fatalCloneVmFixture("detached-cancellation caller remained live after rescue");
  }
  clearCloneVmHooks();

  const bool childDeletedBeforeReturn = waitForCloneVmThreadCount(process, 1);
  bool passed = callerStarted && callerReapable && childDeletedBeforeReturn &&
                process->getState() == Process::Active && context->callerEntered == 1 &&
                context->callerReturned == 1 && context->hookCalls == 1 &&
                context->tidsReady == 1 && context->childCancellationRequested == 1 &&
                context->childCancellationReapable == 1 && !context->hookTimedOut &&
                !context->unexpectedHookRelease && context->observedTid &&
                context->cloneResult == context->observedTid &&
                context->parentTid == static_cast<int>(context->observedTid) &&
                context->childTid == static_cast<int>(context->observedTid);

  if (!childDeletedBeforeReturn) {
    fatalCloneVmFixture("detached child remained live after its creator retired");
  }
  if (!context->caller->joinForCompletion()) {
    fatalCloneVmFixture("detached-cancellation caller could not be joined");
  }
  if (!waitForCloneVmThreadCount(process, 0)) {
    fatalCloneVmFixture("detached-cancellation process retained a live thread");
  }

  delete process;
  passed = passed && context->processDestructions == 1 && context->subsystemDestructions == 1;
  delete context;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL clone-vm-detached-cached-tid: "
        "POSIX clone did not return the cached ID after detached cancellation");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS clone-vm-detached-cached-tid");
  return true;
}

bool cloneVmTerminalStartCancellation(Process* kernelProcess) {
  CloneVmExitRaceContext* context = new CloneVmExitRaceContext(WaitForProcessExit);
  CloneVmExitRaceProcess* process =
      new CloneVmExitRaceProcess(kernelProcess, context->processDestructions);
  process->setSubsystem(new CloneVmExitRaceSubsystem(context->subsystemDestructions));
  process->description() = "hosted clone-vs-exit fixture";

  context->process = process;
  context->caller =
      new Thread(process, cloneVmWhileProcessExits, context, nullptr, false, true, true);
  context->caller->setName("hosted clone-vs-exit caller");
  context->terminator =
      new Thread(process, terminateCloneVmProcess, context, nullptr, false, true, true);
  context->terminator->setName("hosted clone-vs-exit terminator");
  process->publish();

  __atomic_store_n(&g_CloneVmExitRaceContext, context, __ATOMIC_RELEASE);
  Process::setTerminationElectionHook(observeCloneVmTerminationElection);
  posixSetCloneBeforeStartHookForTest(terminateCloneVmBeforeStart, context);
  const bool callerStarted = context->caller->start();
  const bool callerPaused = callerStarted && waitForCloneVmHookPause(context);
  Thread* pausedExpectedChild = context->child.value();
  Process::ThreadLease pausedChild;
  const bool pausedChildPinned = callerPaused && pausedExpectedChild &&
                                 process->acquireThread(pausedChild, pausedExpectedChild) &&
                                 pausedChild->getId() == context->observedTid;
  const bool terminatorStarted = context->terminator->start();
  if (terminatorStarted) {
    context->terminatorStarted += 1;
  } else {
    context->rescueCancellation = 1;
    Thread* rescueExpectedChild = context->child.value();
    bool publishedChildSafe = !rescueExpectedChild;
    if (pausedChildPinned) {
      pausedChild->setUnwindState(Thread::TerminateThread);
      context->schedulerPredicateReleased = 1;
      publishedChildSafe = true;
    } else if (rescueExpectedChild) {
      Process::ThreadLease rescueChild;
      if (process->acquireThread(rescueChild, rescueExpectedChild)) {
        rescueChild->setUnwindState(Thread::TerminateThread);
        context->schedulerPredicateReleased = 1;
        publishedChildSafe = true;
      } else {
        for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
          if (process->getNumThreads() == 2) {
            context->child = nullptr;
            publishedChildSafe = true;
            break;
          }
          Scheduler::instance().yield();
        }
      }
    }
    if (!publishedChildSafe) {
      pausedChild.reset();
      fatalCloneVmFixture("published child could not be cancelled for start-failure rescue");
    }
    pausedChild.reset();
    context->terminator->setUnwindState(Thread::TerminateThread);
    context->caller->setUnwindState(Thread::TerminateThread);
    context->beforeStart.release();
  }
  pausedChild.reset();

  bool terminated = false;
  for (size_t attempt = 0; attempt < HostedAttempts && terminatorStarted; ++attempt) {
    if (process->isTerminationReapableForHostedTest()) {
      terminated = true;
      break;
    }
    Scheduler::instance().yield();
  }
  if (terminatorStarted && !terminated) {
    context->beforeStart.release();
    for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
      if (process->isTerminationReapableForHostedTest()) {
        terminated = true;
        break;
      }
      Scheduler::instance().yield();
    }
  }

  if (!terminatorStarted) {
    const bool callerReapable = waitForCloneVmThreadReapable(context->caller);
    const bool terminatorReapable = waitForCloneVmThreadReapable(context->terminator);
    if (!callerReapable || !terminatorReapable) {
      fatalCloneVmFixture("start-failure rescue left a worker live");
    }
    clearCloneVmHooks();
    if (!context->caller->joinForCompletion() || !context->terminator->joinForCompletion()) {
      fatalCloneVmFixture("start-failure rescue could not join both workers");
    }
    if (!waitForCloneVmThreadCount(process, 0)) {
      fatalCloneVmFixture("start-failure rescue retained a cloned child");
    }
    delete process;
    const bool destroyed = context->processDestructions == 1 && context->subsystemDestructions == 1;
    delete context;
    if (!destroyed) {
      FATAL("HOSTED-SYSCALL-TEST: clone start-failure rescue did not destroy exact owners");
    }
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL clone-vm-terminal-start-cancellation: "
        "the exit worker did not start");
    return false;
  }
  if (!terminated) {
    fatalCloneVmFixture("process-exit cancellation did not become reapable");
  }
  clearCloneVmHooks();

  Thread* retiredChild = context->child.value();
  const bool retired = terminated && process->getState() == Process::Terminated &&
                       process->getNumThreads() == 3 &&
                       context->caller->getStatus() == Thread::AwaitingJoin &&
                       context->terminator->getStatus() == Thread::AwaitingJoin && retiredChild &&
                       retiredChild->getStatus() == Thread::AwaitingJoin &&
                       !retiredChild->wasStartPublishedForHostedTest();
  bool passed =
      callerPaused && retired && context->callerEntered == 1 && context->callerReturned == 1 &&
      context->terminatorEntered == 1 && !context->terminatorReturned && context->hookCalls == 1 &&
      context->tidsReady == 1 && context->terminationElectionCalls == 1 &&
      context->ownershipWindowReleased == 1 && context->ownershipCancellationObserved == 1 &&
      !context->ownershipStartObserved && context->controlledHookRelease == 1 &&
      !context->electionTimedOut && context->schedulerPredicateInstalled == 1 &&
      !context->schedulerPredicateInstallFailed && context->schedulerPredicateReleased == 1 &&
      context->terminatorStarted == 1 && !context->terminalCancellation &&
      !context->unexpectedHookRelease && !context->hookTimedOut && context->observedTid &&
      context->cloneResult == context->observedTid &&
      context->parentTid == static_cast<int>(context->observedTid) &&
      context->childTid == static_cast<int>(context->observedTid);

  passed = passed && pausedChildPinned;
  delete process;
  passed = passed && context->processDestructions == 1 && context->subsystemDestructions == 1;
  delete context;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL clone-vm-terminal-start-cancellation: "
        "terminal cancellation did not retire the published clone exactly once");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS clone-vm-terminal-start-cancellation");
  return true;
}

bool failedPinnedModuleRejectsUnload() {
  Module module;
  module.name.assign("hosted-failed-pinned-probe");
  module.unloadable = false;
  module.status = Module::Failed;

  if (KernelElf::claimModuleUnloadForTest(&module) != KernelElf::TestUnloadPinned ||
      module.status != Module::Failed || module.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL failed-pinned-module: "
        "failed initialisation did not preserve its pinned module image");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS failed-pinned-module");
  return true;
}

ModuleInfo* findStaticModuleInfo(const char* name, size_t& matches) {
  ModuleInfo* match = nullptr;
  matches = 0;
  for (size_t i = 0; i < g_StaticDriverN; ++i) {
    ModuleInfo* info = g_StaticDrivers[i];
    if (info && info->name && !StringCompare(info->name, name)) {
      match = info;
      ++matches;
    }
  }
  return match;
}

bool moduleInfoDependsOn(ModuleInfo* info, const char* dependency, bool optional = false) {
  const char** dependencies = optional ? info->opt_dependencies : info->dependencies;
  if (!dependencies) {
    return false;
  }
  for (size_t i = 0; dependencies[i]; ++i) {
    if (!StringCompare(dependencies[i], dependency)) {
      return true;
    }
  }
  return false;
}

bool linkerModuleMetadataIsPinned() {
  size_t matches = 0;
  ModuleInfo* linker = findStaticModuleInfo("linker", matches);

  if (matches != 1 || !linker || linker->unloadable || linker->runtimeUnloadable ||
      !moduleInfoDependsOn(linker, "vfs")) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL linker-pinned-metadata: "
        "the real linker ModuleInfo did not pin its dependency closure");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS linker-pinned-metadata");
  return true;
}

bool filesystemModuleUnloadPolicyIsCorrect() {
  size_t posixMatches = 0;
  size_t mountrootMatches = 0;
  size_t ramfsMatches = 0;
  size_t rawfsMatches = 0;
  ModuleInfo* posix = findStaticModuleInfo("posix", posixMatches);
  ModuleInfo* mountroot = findStaticModuleInfo("mountroot", mountrootMatches);
  ModuleInfo* ramfs = findStaticModuleInfo("ramfs", ramfsMatches);
  ModuleInfo* rawfs = findStaticModuleInfo("rawfs", rawfsMatches);

  const bool metadataValid =
      posixMatches == 1 && mountrootMatches == 1 && ramfsMatches == 1 && rawfsMatches == 1 &&
      posix && mountroot && ramfs && rawfs && posix->unloadable && !posix->runtimeUnloadable &&
      mountroot->unloadable && !mountroot->runtimeUnloadable && !ramfs->unloadable &&
      !ramfs->runtimeUnloadable && rawfs->unloadable && !rawfs->runtimeUnloadable &&
      moduleInfoDependsOn(posix, "mountroot") && moduleInfoDependsOn(posix, "ramfs") &&
      moduleInfoDependsOn(mountroot, "vfs") && moduleInfoDependsOn(mountroot, "rawfs") &&
      moduleInfoDependsOn(mountroot, "ramfs") && moduleInfoDependsOn(mountroot, "fat", true) &&
      moduleInfoDependsOn(mountroot, "ext2", true) &&
      moduleInfoDependsOn(mountroot, "iso9660", true) && moduleInfoDependsOn(ramfs, "vfs") &&
      moduleInfoDependsOn(rawfs, "vfs");
  if (!metadataValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL filesystem-unload-policy-metadata: "
        "the real filesystem owner metadata did not encode the expected unload policy or "
        "dependency closure");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS filesystem-unload-policy-metadata");
  return true;
}

bool runtimePinnedModuleAllowsLifecycleCleanup() {
  g_RuntimePinnedLifecycleCalls = 0;

  Module active;
  active.name.assign("hosted-runtime-pinned-active-probe");
  active.exit = runtimePinnedLifecycleProbe;
  active.runtimeUnloadable = false;
  active.status = Module::Active;

  bool runLifecycle = true;
  const KernelElf::TestModuleUnloadClaim explicitActive =
      KernelElf::claimModuleUnloadForTest(&active, false, &runLifecycle);
  const bool explicitActiveValid = explicitActive == KernelElf::TestUnloadRuntimePinned &&
                                   !runLifecycle && active.status == Module::Active &&
                                   !active.unloadComplete && !g_RuntimePinnedLifecycleCalls;
  if (explicitActive == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&active);
  }
  if (!explicitActiveValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "explicit unload escaped the runtime-only lifetime boundary");
    return false;
  }

  const KernelElf::TestModuleUnloadClaim shutdownActive =
      KernelElf::claimModuleUnloadForTest(&active, true, &runLifecycle);
  const bool shutdownActiveValid = shutdownActive == KernelElf::TestUnloadClaimed && runLifecycle &&
                                   active.status == Module::Unloading;
  if (shutdownActive == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&active, false, runLifecycle);
  }
  if (!shutdownActiveValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "shutdown could not claim an active runtime-pinned module");
    return false;
  }

  Module failed;
  failed.name.assign("hosted-runtime-pinned-failed-probe");
  failed.exit = runtimePinnedLifecycleProbe;
  failed.runtimeUnloadable = false;
  failed.status = Module::Failed;
  runLifecycle = true;
  const KernelElf::TestModuleUnloadClaim explicitFailed =
      KernelElf::claimModuleUnloadForTest(&failed, false, &runLifecycle);
  const bool explicitFailedValid = explicitFailed == KernelElf::TestUnloadRuntimePinned &&
                                   !runLifecycle && failed.status == Module::Failed &&
                                   !failed.unloadComplete;
  if (explicitFailed == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&failed, true);
  }
  if (!explicitFailedValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "explicit unload escaped a failed runtime-pinned module");
    return false;
  }

  const KernelElf::TestModuleUnloadClaim failureCleanup =
      KernelElf::claimModuleUnloadForTest(&failed, true, &runLifecycle);
  const bool failureCleanupValid = failureCleanup == KernelElf::TestUnloadClaimed && runLifecycle &&
                                   failed.status == Module::Unloading;
  if (failureCleanup == KernelElf::TestUnloadClaimed) {
    KernelElf::completeModuleUnloadForTest(&failed, true, runLifecycle);
  }
  if (!failureCleanupValid) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "failed initialisation could not claim lifecycle cleanup");
    return false;
  }

  if (!active.isUnloaded() || !active.unloadComplete || failed.status != Module::Failed ||
      !failed.unloadComplete || g_RuntimePinnedLifecycleCalls != 2) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL runtime-pinned-cleanup: "
        "shutdown or failure cleanup did not publish completion");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS runtime-pinned-cleanup");
  return true;
}

bool moduleUnloadOwnershipIsRetryable() {
  Module module;
  module.name.assign("hosted-unload-owner-probe");
  module.status = Module::Active;
  Module* fixtures[] = {&module};

  const KernelElf::TestModuleUnloadClaim first =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-owner-probe");
  const KernelElf::TestModuleUnloadClaim concurrent =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-owner-probe");
  KernelElf::completeModuleUnloadForTest(&module);
  const KernelElf::TestModuleUnloadClaim repeat =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-owner-probe");
  const KernelElf::TestModuleUnloadClaim missing =
      KernelElf::claimNamedModuleUnloadForTest(fixtures, 1, "hosted-unload-missing-probe");

  if (first != KernelElf::TestUnloadClaimed || concurrent != KernelElf::TestUnloadBusy ||
      repeat != KernelElf::TestUnloadComplete || missing != KernelElf::TestUnloadUnknown ||
      !module.isUnloaded() || !module.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL module-unload-ownership: "
        "the first owner, concurrent retry, or completed tombstone was lost");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS module-unload-ownership");
  return true;
}

bool moduleShutdownOrderIsDependencySafe() {
  const char* nicsOptional[] = {"ne2k", nullptr};
  const char* ne2kDependencies[] = {"network-stack", nullptr};

  Module networkStack;
  networkStack.name.assign("network-stack");
  networkStack.status = Module::Active;

  Module nics;
  nics.name.assign("nics");
  nics.depends_opt = nicsOptional;
  nics.runtimeUnloadable = false;
  nics.status = Module::Active;

  Module ne2k;
  ne2k.name.assign("ne2k");
  ne2k.depends = ne2kDependencies;
  ne2k.status = Module::Active;

  Module* modules[] = {&networkStack, &nics, &ne2k};
  Module* order[3] = {};
  const size_t planned = KernelElf::planModuleUnloadOrderForTest(modules, 3, order, 3);
  const size_t repeated = KernelElf::planModuleUnloadOrderForTest(modules, 3, order, 3);

  const char* cycleADependencies[] = {"cycle-b", nullptr};
  const char* cycleBDependencies[] = {"cycle-a", nullptr};
  Module cycleA;
  cycleA.name.assign("cycle-a");
  cycleA.depends = cycleADependencies;
  cycleA.status = Module::Active;
  Module cycleB;
  cycleB.name.assign("cycle-b");
  cycleB.depends = cycleBDependencies;
  cycleB.status = Module::Active;
  Module* cycle[] = {&cycleA, &cycleB};
  Module* cycleOrder[2] = {};
  const size_t cyclicPlanned = KernelElf::planModuleUnloadOrderForTest(cycle, 2, cycleOrder, 2);

  if (planned != 3 || order[0] != &nics || order[1] != &ne2k || order[2] != &networkStack ||
      repeated != 0 || cyclicPlanned != 0 || cycleA.unloadComplete || cycleB.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL module-shutdown-order: "
        "optional/mandatory dependents were not retired first or a cycle was torn down");
    return false;
  }

  Module permanent;
  permanent.name.assign("permanent-shutdown-probe");
  permanent.unloadable = false;
  permanent.runtimeUnloadable = false;
  permanent.status = Module::Active;
  Module runtimePinned;
  runtimePinned.name.assign("runtime-pinned-shutdown-probe");
  runtimePinned.runtimeUnloadable = false;
  runtimePinned.status = Module::Active;
  Module* retentionModules[] = {&permanent, &runtimePinned};
  Module* retentionOrder[2] = {};
  const size_t retentionPlanned =
      KernelElf::planModuleUnloadOrderForTest(retentionModules, 2, retentionOrder, 2);

  if (retentionPlanned != 1 || retentionOrder[0] != &runtimePinned ||
      !runtimePinned.unloadComplete || permanent.unloadComplete) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL module-shutdown-retention-policy: "
        "a runtime-only module was retained or a permanent pin was retired");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS module-shutdown-order");
  NOTICE("HOSTED-SYSCALL-TEST: PASS module-shutdown-retention-policy");
  return true;
}

bool publishTerminalBlockedHandlerFixture(Process* kernelProcess) {
  TerminalBlockedHandlerContext* context = new TerminalBlockedHandlerContext;
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->description() = "hosted blocked POSIX handler shutdown fixture";
  Thread* thread =
      new Thread(process, terminalBlockedHandlerEntry, context, nullptr, false, true, true);
  thread->setName("hosted blocked POSIX handler fixture");
  context->thread = thread;
  process->publish();

  g_TerminalBlockedHandlerContext = context;
  SyscallManager::instance().setHandlerPinHook(terminalBlockedHandlerPin);
  const bool started = thread->start();

  bool blocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && started; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (context->hookEntered == static_cast<size_t>(1) && thread->getWaitDebugInfo(info) &&
        info.queue && info.queued && info.channelOwner == &context->blocker &&
        thread->getStatus() == Thread::Sleeping) {
      blocked = true;
      break;
    }
    Scheduler::instance().yield();
  }

  SyscallManager::instance().setHandlerPinHook(nullptr);

  if (!started || !blocked || context->exitStaged != static_cast<size_t>(1) ||
      context->releasedByTermination || context->unexpectedRelease || context->syscallReturned) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-blocked-handler-fixture: "
        "the POSIX handler was not admitted and blocked with a staged process exit");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-blocked-handler-fixture-published");
  return true;
}

bool runRegressions() {
  NOTICE("HOSTED-SYSCALL-TEST: BEGIN real-event-boundaries");
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread || thread->getStateLevel()) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL real-event-boundaries: "
        "module initialisation was not at base state");
    return false;
  }

  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  if (!kernelProcess) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN usercopy");
  if (!runHostedUsercopyRegressions(kernelProcess)) {
    return false;
  }

  bool establishedAliasPassed = true;
  NOTICE("HOSTED-SYSCALL-TEST: BEGIN directory-retained-lookup-atomicity");
  establishedAliasPassed &= directoryRetainedLookupAtomicity(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN directory-retained-lookup-lifecycle");
  establishedAliasPassed &= directoryRetainedLookupLifecycle(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN vfs-established-alias-serialization");
  establishedAliasPassed &= establishedAliasRetainSerialization(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN file-established-alias-lifetime");
  establishedAliasPassed &= establishedFileAliasLifetime();

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN process-filesystem-context-lifetime");
  establishedAliasPassed &= processFilesystemContextLifetime(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN mmap-established-alias-lifetime");
  establishedAliasPassed &= establishedMappingAliasLifetime();

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN munmap-target-page-geometry");
  establishedAliasPassed &= munmapUsesTargetPageGeometry(thread);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN mmap-split-alias-lifetime");
  establishedAliasPassed &= mappingManagerSplitLifetime(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN posix-path-lookup-lifetime");
  establishedAliasPassed &= posixPathLookupLifetime(kernelProcess);
  if (!establishedAliasPassed) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-close-pinning");
  if (!descriptorClosePinning(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-close-generation");
  if (!descriptorCloseGeneration(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-open-file-description-state");
  if (!descriptorOpenFileDescriptionState()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-open-file-description-lifetime");
  if (!descriptorOpenFileDescriptionLifetime()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-append-policy");
  if (!descriptorAppendPolicy(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-nonblocking-policy");
  if (!descriptorNonblockingPolicy()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-position-alias-serialization");
  if (!descriptorPositionAliasSerialization(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-vector-io-serialization");
  if (!descriptorVectorIoSerialization(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN scalar-io-user-buffer-lifetime");
  if (!runHostedScalarIoRegressions(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-dup-contract");
  if (!descriptorDupContract(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN descriptor-position-policy");
  if (!descriptorPositionPolicy()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN select-projection-timeout");
  if (!selectProjectionAndTimeoutContract()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN pipe-poll-readiness");
  if (!pipePollReadiness(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN epoll-level-oneshot-ofd-lifetime");
  if (!epollLevelOneShotAndOfdLifetime(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN epoll-reordered-transition-publication");
  if (!epollReorderedTransitionPublication(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN epoll-persistent-fifo-reopen-reclose");
  if (!epollPersistentFifoReopenReclose(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN eventfd-counter-readiness-lifetime");
  if (!runHostedEventFdRegressions(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN poll-close-reuse-cleanup");
  if (!pollCloseReuseCleanup(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN posix-teardown-contention");
  if (!posixTeardownContention(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN unix-bind-replacement-lifetime");
  if (!runHostedUnixEndpointLifetimeRegression(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN socket-zero-result-signal");
  if (!zeroResultWinsSignal(thread)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN clone-errno-lifetime");
  if (!cloneStateDropsParentErrnoDestination()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN clone-vm-detached-cached-tid");
  if (!cloneVmDetachedCancellationReturnsCachedTid(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN clone-vm-terminal-start-cancellation");
  if (!cloneVmTerminalStartCancellation(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN failed-pinned-module");
  if (!failedPinnedModuleRejectsUnload()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN linker-pinned-metadata");
  if (!linkerModuleMetadataIsPinned()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN filesystem-unload-policy-metadata");
  if (!filesystemModuleUnloadPolicyIsCorrect()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN runtime-pinned-cleanup");
  if (!runtimePinnedModuleAllowsLifecycleCleanup()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN module-unload-ownership");
  if (!moduleUnloadOwnershipIsRetryable()) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN module-shutdown-order");
  if (!moduleShutdownOrderIsDependencySafe()) {
    return false;
  }

  SyscallManager& manager = SyscallManager::instance();
  static char pedigreeCModule[] = "pedigree-c";
  static char lwipModule[] = "lwip";
  static char networkStackModule[] = "network-stack";
  const uintptr_t sigretResult = manager.syscall(posix, PEDIGREE_SIGRET);
  const uintptr_t unwindResult = manager.syscall(posix, PEDIGREE_UNWIND_SIGNAL);
  const uintptr_t eventReturnResult = manager.syscall(pedigree_c, PEDIGREE_EVENT_RETURN);
  const uintptr_t selfUnloadResult = manager.syscall(pedigree_c, PEDIGREE_MODULE_UNLOAD,
                                                     reinterpret_cast<uintptr_t>(pedigreeCModule));
  const uintptr_t stillLoadedResult = manager.syscall(pedigree_c, PEDIGREE_MODULE_IS_LOADED,
                                                      reinterpret_cast<uintptr_t>(pedigreeCModule));
  const uintptr_t lwipUnloadResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_UNLOAD, reinterpret_cast<uintptr_t>(lwipModule));
  const uintptr_t lwipStillLoadedResult = manager.syscall(pedigree_c, PEDIGREE_MODULE_IS_LOADED,
                                                          reinterpret_cast<uintptr_t>(lwipModule));
  const uintptr_t networkStackUnloadResult = manager.syscall(
      pedigree_c, PEDIGREE_MODULE_UNLOAD, reinterpret_cast<uintptr_t>(networkStackModule));
  const uintptr_t networkStackStillLoadedResult = manager.syscall(
      pedigree_c, PEDIGREE_MODULE_IS_LOADED, reinterpret_cast<uintptr_t>(networkStackModule));

  if (sigretResult != static_cast<uintptr_t>(-1) || unwindResult != static_cast<uintptr_t>(-1) ||
      eventReturnResult != static_cast<uintptr_t>(-1) ||
      selfUnloadResult != static_cast<uintptr_t>(-1) || stillLoadedResult != 1 ||
      lwipUnloadResult != static_cast<uintptr_t>(-1) || lwipStillLoadedResult != 1 ||
      networkStackUnloadResult != static_cast<uintptr_t>(-1) ||
      networkStackStillLoadedResult != 1 || thread->getStateLevel() || thread->getErrno()) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL real-event-boundaries: "
        "a public misuse path escaped its lifetime boundary");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS real-event-boundaries");

  PosixProcess* terminalFixture = new PosixProcess(kernelProcess);
  terminalFixture->setSubsystem(new PosixSubsystem);
  terminalFixture->description() = "hosted zero-thread POSIX shutdown fixture";
  terminalFixture->publish();
  if (terminalFixture->getNumThreads() != 0 || terminalFixture->getType() != Process::Posix) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-drain-fixture: "
        "fixture was not published as an ownerless POSIX process");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-drain-fixture-published");

  if (!posixDuplicateInitRollbackPreservesProcessForTest(terminalFixture)) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-duplicate-init-rollback: "
        "an unowned duplicate initialisation retired an existing POSIX process");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-duplicate-init-rollback-preserved-process");

  PosixProcess* createdFixture = new PosixProcess(kernelProcess);
  createdFixture->setSubsystem(new PosixSubsystem);
  createdFixture->description() = "hosted Created-thread POSIX shutdown fixture";
  Thread* createdThread =
      new Thread(createdFixture, terminalCreatedFixtureEntry, nullptr, nullptr, false, true, true);
  createdThread->setName("hosted terminal Created-thread fixture");
  createdFixture->publish();
  if (createdFixture->getNumThreads() != 1 || createdThread->getStatus() != Thread::Created ||
      createdFixture->getType() != Process::Posix) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-drain-created-fixture: "
        "fixture did not retain its unstarted ordinary entry");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-drain-created-fixture-published");

  if (!publishTerminalBlockedHandlerFixture(kernelProcess)) {
    return false;
  }
  return true;
}

bool entry() {
  const bool passed = runRegressions();
  system_reset();
  return passed;
}

void exit() {
  TerminalBlockedHandlerContext* context = g_TerminalBlockedHandlerContext;
  g_TerminalBlockedHandlerContext = nullptr;
  if (!context) {
    return;
  }

  if (context->hookEntered != static_cast<size_t>(1) ||
      context->exitStaged != static_cast<size_t>(1) ||
      context->releasedByTermination != static_cast<size_t>(1) || context->unexpectedRelease ||
      context->syscallReturned) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-terminal-blocked-handler-release: "
        "terminal process teardown did not release the admitted handler before module exit");
  } else {
    NOTICE("HOSTED-SYSCALL-TEST: PASS posix-terminal-blocked-handler-released-by-process-exit");
  }
  delete context;
}
}  // namespace

MODULE_INFO("hosted-syscall-smoke", &entry, &exit, "posix", "pedigree-c");

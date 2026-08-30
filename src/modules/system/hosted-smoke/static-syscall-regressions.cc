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
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/subsys/posix/poll-syscalls.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/VFS.h"
#undef PEDIGREE_INIT_SIGRET
#undef PEDIGREE_SIGRET
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/linker/KernelElf.h"
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

#include "modules/subsys/posix/syscalls/posixSyscallNumbers.h"

extern void system_reset();
extern "C" bool posixDuplicateInitRollbackPreservesProcessForTest(Process* processIdentity);

namespace {
constexpr size_t HostedAttempts = 10000;
constexpr int PollCloseReuseTimeoutMilliseconds = 5000;
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

  bool remove(File* parent, File* file) override {
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

  bool publishEphemeral(File* file) {
    return addEphemeralFile(file);
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

struct DirectoryEmptyAbaContext {
  DirectoryEmptyAbaContext(RetainedLookupDirectory* directory, const String& alias,
                           RetainedLookupFile* oldChild, Atomic<size_t>& oldDestructions,
                           RetainedLookupFile* replacement)
      : directory(directory),
        alias(alias),
        oldChild(oldChild),
        oldDestructions(oldDestructions),
        replacement(replacement),
        callbacks(0),
        firstSawOld(false),
        oldStayedAliveAfterAliasRemoval(false),
        secondSawReplacement(false),
        replacementPublished(false),
        firstReplacementEmergency(false),
        secondReplacementEmergency(false) {}

  RetainedLookupDirectory* directory;
  String alias;
  RetainedLookupFile* oldChild;
  Atomic<size_t>& oldDestructions;
  RetainedLookupFile* replacement;
  size_t callbacks;
  bool firstSawOld;
  bool oldStayedAliveAfterAliasRemoval;
  bool secondSawReplacement;
  bool replacementPublished;
  bool firstReplacementEmergency;
  bool secondReplacementEmergency;
};

bool directoryEmptyAbaRemove(File* parent, File* file, void* opaque) {
  DirectoryEmptyAbaContext* context = reinterpret_cast<DirectoryEmptyAbaContext*>(opaque);
  ++context->callbacks;
  if (context->callbacks == 1) {
    context->firstSawOld = parent == context->directory && file == context->oldChild;
    if (!context->firstSawOld) {
      return false;
    }

    context->directory->remove(HashedStringView(context->alias));
    context->oldStayedAliveAfterAliasRemoval = context->oldDestructions == static_cast<size_t>(0);
    if (!context->oldStayedAliveAfterAliasRemoval) {
      return false;
    }
    context->directory->publish(context->alias, context->replacement);
    context->replacementPublished = true;
    context->firstReplacementEmergency = VFS::instance().retainTrackedFile(context->replacement);
    context->secondReplacementEmergency = context->firstReplacementEmergency &&
                                          VFS::instance().retainTrackedFile(context->replacement);
    return context->secondReplacementEmergency;
  }

  context->secondSawReplacement =
      context->callbacks == 2 && parent == context->directory && file == context->replacement;
  return false;
}

bool directoryEmptyPreservesSameKeyReplacement() {
  RetainedLookupFilesystem filesystem;
  RetainedLookupDirectory directory(String("retained-empty-aba-root"), &filesystem);
  filesystem.setRoot(&directory);
  Atomic<size_t> oldDestructions(0);
  Atomic<size_t> replacementDestructions(0);
  const String alias("retained-empty-aba-alias");
  RetainedLookupFile* oldChild = new RetainedLookupFile(String("retained-empty-aba-old"),
                                                        &filesystem, &directory, oldDestructions);
  RetainedLookupFile* replacement = new RetainedLookupFile(
      String("retained-empty-aba-replacement"), &filesystem, &directory, replacementDestructions);
  directory.publish(alias, oldChild);

  DirectoryEmptyAbaContext context(&directory, alias, oldChild, oldDestructions, replacement);
  filesystem.setRemoveHook(directoryEmptyAbaRemove, &context);
  const bool emptyRejected = !directory.empty();
  filesystem.setRemoveHook(nullptr, nullptr);

  const bool replacementStayedAlive = !replacementDestructions;
  Directory::ChildLease retainedReplacement;
  const bool replacementVisible =
      context.secondReplacementEmergency &&
      directory.lookupRetained(HashedStringView(alias), retainedReplacement) &&
      retainedReplacement.get() == replacement;

  directory.remove(HashedStringView(alias));
  retainedReplacement.reset();
  size_t oldCleanupReleases = 0;
  if (!oldDestructions) {
    oldCleanupReleases = drainTrackedFileOwners(oldChild, oldDestructions);
  }
  size_t replacementCleanupReleases = 0;
  if (context.replacementPublished) {
    replacementCleanupReleases = drainTrackedFileOwners(replacement, replacementDestructions);
  } else if (!replacementDestructions) {
    delete replacement;
  }

  return emptyRejected && context.callbacks == 2 && context.firstSawOld &&
         context.oldStayedAliveAfterAliasRemoval && context.secondSawReplacement &&
         context.replacementPublished && context.firstReplacementEmergency &&
         context.secondReplacementEmergency && replacementStayedAlive && replacementVisible &&
         oldCleanupReleases == 0 && replacementCleanupReleases == 2 &&
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
  const bool emptyAba = directoryEmptyPreservesSameKeyReplacement();
  const bool duplicateEphemeral = directoryRetainedDuplicateEphemeral();
  if (!disjoint || !deletion || !replacement || !emptyAba || !duplicateEphemeral) {
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
  PollGenerationProbe(Atomic<size_t>& registrations, Atomic<size_t>& unpolls,
                      Atomic<size_t>& destructions)
      : NetworkSyscalls(AF_UNSPEC, SOCK_STREAM, 0),
        m_Registrations(registrations),
        m_Unpolls(unpolls),
        m_Destructions(destructions),
        m_Ready(0),
        m_Waiter(nullptr) {}

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

  bool canPoll() const override {
    return true;
  }

  bool poll(bool& read, bool& write, bool& error, Semaphore* waiter) override {
    const bool readable = read && m_Ready;
    read = readable;
    write = false;
    error = false;
    if (waiter && !readable) {
      m_Waiter = waiter;
      m_Registrations += 1;
    }
    return readable;
  }

  void unPoll(Semaphore* waiter) override {
    m_Unpolls += 1;
    if (m_Waiter == waiter) {
      m_Waiter = nullptr;
    }
  }

  void makeReadable() {
    m_Ready = 1;
    Semaphore* waiter = m_Waiter;
    if (waiter) {
      waiter->release();
    }
  }

  const void* waiterAddress() const {
    return m_Waiter;
  }

 private:
  Atomic<size_t>& m_Registrations;
  Atomic<size_t>& m_Unpolls;
  Atomic<size_t>& m_Destructions;
  Atomic<size_t> m_Ready;
  Atomic<Semaphore*> m_Waiter;
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
  oldDescriptor->offset = 1;
  subsystem->addFileDescriptor(DescriptorNumber, oldDescriptor);

  DescriptorLease oldLease;
  bool passed = subsystem->acquireFileDescriptor(DescriptorNumber, oldLease);

  DescriptorRetirementProbe* replacement = new DescriptorRetirementProbe(replacementDestructions);
  replacement->fd = DescriptorNumber;
  replacement->offset = 2;
  subsystem->addFileDescriptor(DescriptorNumber, replacement);

  // An in-flight close of the old generation must not remove a descriptor
  // which has since reused the same numeric fd.
  passed = passed && !subsystem->closeFileDescriptor(DescriptorNumber, oldLease) &&
           oldDestructions == 0 && replacementDestructions == 0;

  DescriptorLease replacementLease;
  passed = passed && subsystem->acquireFileDescriptor(DescriptorNumber, replacementLease) &&
           replacementLease->offset == 2;

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

struct PollCloseReuseContext {
  explicit PollCloseReuseContext(size_t fd)
      : descriptor{static_cast<int>(fd), POLLIN, 0}, result(-2), entered(0), returned(0) {}

  struct pollfd descriptor;
  Atomic<int> result;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

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

  Atomic<size_t> aRegistrations(0);
  Atomic<size_t> aUnpolls(0);
  Atomic<size_t> aNetworkDestructions(0);
  Atomic<size_t> aDescriptorDestructions(0);
  PollGenerationProbe* aNetwork =
      new PollGenerationProbe(aRegistrations, aUnpolls, aNetworkDestructions);
  SharedPointer<NetworkSyscalls> aNetworkKeepalive(aNetwork);
  DescriptorRetirementProbe* aDescriptor = new DescriptorRetirementProbe(aDescriptorDestructions);
  aDescriptor->fd = DescriptorNumber;
  aDescriptor->offset = 1;
  aDescriptor->networkImpl = aNetworkKeepalive;
  subsystem->addFileDescriptor(DescriptorNumber, aDescriptor);

  Atomic<size_t> bRegistrations(0);
  Atomic<size_t> bUnpolls(0);
  Atomic<size_t> bNetworkDestructions(0);
  Atomic<size_t> bDescriptorDestructions(0);
  PollGenerationProbe* bNetwork =
      new PollGenerationProbe(bRegistrations, bUnpolls, bNetworkDestructions);
  SharedPointer<NetworkSyscalls> bNetworkKeepalive(bNetwork);
  DescriptorRetirementProbe* bDescriptor = new DescriptorRetirementProbe(bDescriptorDestructions);
  bDescriptor->fd = DescriptorNumber;
  bDescriptor->offset = 2;
  bDescriptor->networkImpl = bNetworkKeepalive;

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
    if (context.entered && aRegistrations && worker->getWaitDebugInfo(info) && info.queue &&
        info.queued && info.channelOwner == aNetwork->waiterAddress() &&
        worker->getStatus() == Thread::Sleeping) {
      blockedOnA = true;
      break;
    }
    Scheduler::instance().yield();
  }

  bool passed = started && blockedOnA && aRegistrations == 1;
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "waiter-published-a blocked="
      << blockedOnA << " registrations=" << aRegistrations.value());
  DescriptorLease closingA;
  const bool acquiredA = subsystem->acquireFileDescriptor(DescriptorNumber, closingA);
  const bool closedA = acquiredA && subsystem->closeFileDescriptor(DescriptorNumber, closingA);
  closingA.reset();
  subsystem->addFileDescriptor(DescriptorNumber, bDescriptor);
  passed = passed && closedA && aDescriptorDestructions == 0 && aNetworkDestructions == 0;
  NOTICE(
      "HOSTED-SYSCALL-TEST: PHASE poll-close-reuse-cleanup "
      "closed-a-published-b");

  if (aRegistrations) {
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
           (context.descriptor.revents & POLLIN) && aUnpolls == 1 && bUnpolls == 0 &&
           bRegistrations == 0 && aDescriptorDestructions == 1 && aNetworkDestructions == 0;
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

  if (planned != 3 || order[0] != &nics || order[1] != &ne2k ||
      order[2] != &networkStack || repeated != 0 || cyclicPlanned != 0 ||
      cycleA.unloadComplete || cycleB.unloadComplete) {
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

  bool establishedAliasPassed = true;
  NOTICE("HOSTED-SYSCALL-TEST: BEGIN directory-retained-lookup-atomicity");
  establishedAliasPassed &= directoryRetainedLookupAtomicity(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN directory-retained-lookup-lifecycle");
  establishedAliasPassed &= directoryRetainedLookupLifecycle(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN vfs-established-alias-serialization");
  establishedAliasPassed &= establishedAliasRetainSerialization(kernelProcess);

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN file-established-alias-lifetime");
  establishedAliasPassed &= establishedFileAliasLifetime();

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

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN poll-close-reuse-cleanup");
  if (!pollCloseReuseCleanup(kernelProcess)) {
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: BEGIN posix-teardown-contention");
  if (!posixTeardownContention(kernelProcess)) {
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
      manager.syscall(pedigree_c, PEDIGREE_MODULE_UNLOAD,
                      reinterpret_cast<uintptr_t>(lwipModule));
  const uintptr_t lwipStillLoadedResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_IS_LOADED,
                      reinterpret_cast<uintptr_t>(lwipModule));
  const uintptr_t networkStackUnloadResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_UNLOAD,
                      reinterpret_cast<uintptr_t>(networkStackModule));
  const uintptr_t networkStackStillLoadedResult =
      manager.syscall(pedigree_c, PEDIGREE_MODULE_IS_LOADED,
                      reinterpret_cast<uintptr_t>(networkStackModule));

  if (sigretResult != static_cast<uintptr_t>(-1) || unwindResult != static_cast<uintptr_t>(-1) ||
      eventReturnResult != static_cast<uintptr_t>(-1) ||
      selfUnloadResult != static_cast<uintptr_t>(-1) || stillLoadedResult != 1 ||
      lwipUnloadResult != static_cast<uintptr_t>(-1) || lwipStillLoadedResult != 1 ||
      networkStackUnloadResult != static_cast<uintptr_t>(-1) ||
      networkStackStillLoadedResult != 1 ||
      thread->getStateLevel() || thread->getErrno()) {
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

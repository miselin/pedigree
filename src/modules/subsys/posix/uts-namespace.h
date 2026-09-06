/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_UTS_NAMESPACE_H
#define POSIX_UTS_NAMESPACE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

class Process;
class Thread;
class PosixUtsProcessView;
class PosixUtsTaskBinding;
class PosixNamespaceContext;

enum class UtsStatus { Success, NoMemory, NoSpace, Missing, Denied, Invalid };

class EXPORTED_PUBLIC PosixUtsNamespace {
 public:
  struct Snapshot {
    char node[65] = {};
    char domain[65] = {};
  };
  PosixUtsNamespace(uint64_t identity, const Snapshot& names, bool charged);
  ~PosixUtsNamespace();
  uint64_t identity() const {
    return m_Identity;
  }
  Snapshot snapshot() const;
  void setName(bool domain, const char* bytes, size_t length);

 private:
  PosixUtsNamespace(const PosixUtsNamespace&) = delete;
  PosixUtsNamespace& operator=(const PosixUtsNamespace&) = delete;
  const uint64_t m_Identity;
  const bool m_Charged;
  mutable Mutex m_Lock;
  Snapshot m_Names;
};
using UtsRef = SharedPointer<PosixUtsNamespace>;

class EXPORTED_PUBLIC PosixUtsTarget {
 public:
  PosixUtsTarget();
  PosixUtsTarget(const PosixUtsTarget&);
  PosixUtsTarget& operator=(const PosixUtsTarget&);
  ~PosixUtsTarget();
  explicit operator bool() const;

 private:
  friend class PosixNamespaceContext;
  friend UtsStatus posix_uts_acquire_target(const PosixUtsTarget&, UtsRef&);
  SharedPointer<PosixUtsProcessView> m_View;
  SharedPointer<PosixUtsTaskBinding> m_Task;
  size_t m_ExpectedTaskId = 0;
};

class EXPORTED_PUBLIC PreparedUtsThread {
 public:
  PreparedUtsThread();
  ~PreparedUtsThread();

 private:
  friend class PosixNamespaceContext;
  friend UtsStatus posix_uts_prepare_thread(const UtsRef&, bool, UniquePointer<PreparedUtsThread>&);
  PreparedUtsThread(const PreparedUtsThread&) = delete;
  PreparedUtsThread& operator=(const PreparedUtsThread&) = delete;
  SharedPointer<PosixUtsTaskBinding> m_Binding;
  PreparedUtsThread* m_Next = nullptr;
};

class EXPORTED_PUBLIC PosixNamespaceContext {
 public:
  PosixNamespaceContext();
  ~PosixNamespaceContext();
  bool valid() const;
  void attach(Process& process);
  bool acquireThread(const Thread& thread, UtsRef& result) const;
  void publishThread(UniquePointer<PreparedUtsThread>& prepared, Thread& thread, bool leader);
  void promoteExec(const Thread& thread);
  void retireThread(const Thread& thread);
  void close();
  UtsStatus replaceThread(const Thread& thread, const UtsRef& replacement);

  bool leaderTarget(PosixUtsTarget& result) const;
  bool taskTarget(size_t taskId, PosixUtsTarget& result) const;
  bool threadTarget(const Thread& thread, PosixUtsTarget& result) const;
  bool nextTaskTarget(size_t afterTaskId, size_t& taskId, PosixUtsTarget& result) const;

 private:
  PosixNamespaceContext(const PosixNamespaceContext&) = delete;
  PosixNamespaceContext& operator=(const PosixNamespaceContext&) = delete;
  friend UtsStatus posix_uts_acquire_target(const PosixUtsTarget&, UtsRef&);
  SharedPointer<PosixUtsProcessView> m_View;
};

EXPORTED_PUBLIC UtsStatus posix_uts_initial(UtsRef& result);
EXPORTED_PUBLIC UtsStatus posix_uts_copy(const UtsRef& source, UtsRef& result);
EXPORTED_PUBLIC UtsStatus posix_uts_prepare_thread(const UtsRef& source, bool copy,
                                                   UniquePointer<PreparedUtsThread>& result);
EXPORTED_PUBLIC UtsStatus posix_uts_acquire_target(const PosixUtsTarget& target, UtsRef& result);
EXPORTED_PUBLIC int posix_uts_error(UtsStatus status);

#endif

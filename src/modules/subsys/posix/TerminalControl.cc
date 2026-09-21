/* Copyright (c) 2026, Pedigree Developers. */
#include "TerminalControl.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <signal.h>

#include "PosixProcess.h"
#include "PosixSubsystem.h"

namespace {
Mutex terminalPolicy;

class TerminalBinding final : public Process::ControllingTerminal {
 public:
  TerminalBinding(ConsoleFile* console, const SharedPointer<ConsoleControlState>& control)
      : m_Console(console), m_Control(control) {}
  ~TerminalBinding() override {
    m_Console->releaseVfsReference();
  }
  File* file() const override {
    return static_cast<TerminalControl*>(m_Control.get())->active() ? m_Console : nullptr;
  }

 private:
  ConsoleFile* m_Console;
  SharedPointer<ConsoleControlState> m_Control;
};

PosixProcess* currentProcess() {
  Process* process = Processor::information().getCurrentThread()->getParent();
  return process->getType() == Process::Posix ? static_cast<PosixProcess*>(process) : nullptr;
}

bool currentTerminal(PosixProcess& process, ConsoleFile& console) {
  auto context = process.acquireCttyContext();
  return context && context->file() == &console;
}
}  // namespace

TerminalControl::TerminalControl(size_t session, size_t foreground)
    : m_Active(true), m_Session(session), m_Foreground(foreground) {}

bool TerminalControl::active() const {
  return __atomic_load_n(&m_Active, __ATOMIC_ACQUIRE);
}

void TerminalControl::controlCharacter(Character character) {
  if (!active())
    return;
  const size_t foreground = __atomic_load_n(&m_Foreground, __ATOMIC_ACQUIRE);
  const int signal = character == Character::Interrupt ? SIGINT
                     : character == Character::Quit    ? SIGQUIT
                                                       : SIGTSTP;
  for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i) {
    Scheduler::ProcessLease recipient;
    if (!Scheduler::instance().acquireProcess(recipient, i) ||
        recipient->getType() != Process::Posix)
      continue;
    auto* process = static_cast<PosixProcess*>(recipient.get());
    {
      RecursingLockGuard<Spinlock> guard(ProcessGroupManager::instance().lock());
      size_t group = 0;
      if (process->getSessionId() != m_Session || !process->getProcessGroupId(group) ||
          group != foreground)
        continue;
    }
    Process::ThreadLease thread;
    auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
    if (subsystem && process->acquireProcessSignalThread(thread) && active())
      subsystem->sendSignal(thread.get(), signal, false, true);
  }
}

void TerminalControl::invalidate() {
  __atomic_store_n(&m_Active, false, __ATOMIC_RELEASE);
}

Mutex& TerminalControl::lock() {
  return terminalPolicy;
}

int TerminalControl::attach(ConsoleFile& console, bool steal, bool automatic,
                            const SharedPointer<ConsoleIoState>& opened) {
  TerminationDeferral deferral;
  // Opening can wait for an earlier revocation, so it precedes policy admission.
  auto epoch = opened ? opened : console.captureOpenEpoch();
  if (!epoch) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  LockGuard<Mutex> guard(terminalPolicy);
  if (epoch->revoked()) {
    SYSCALL_ERROR(IoError);
    return -1;
  }
  PosixProcess* process = currentProcess();
  if (!process || console.isMaster()) {
    SYSCALL_ERROR(NotAConsole);
    return automatic ? 0 : -1;
  }
  auto context = process->acquireCttyContext();
  File* existing = context ? context->file() : nullptr;
  const size_t session = process->getSessionId();
  if (session != process->getUserspaceId() || (existing && existing != &console)) {
    if (automatic)
      return 0;
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  auto previous = console.controlState();
  auto* old = static_cast<TerminalControl*>(previous.get());
  if (old && old->active() && old->m_Session == session && existing == &console)
    return 0;
  if (old && old->active() && old->m_Session != session &&
      (!steal || process->getEffectiveUserId() != 0)) {
    if (automatic)
      return 0;
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  size_t group = 0;
  if (!process->getProcessGroupId(group)) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  auto control = SharedPointer<ConsoleControlState>::tryAdopt(new TerminalControl(session, group));
  if (!control || !console.retainVfsReference()) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  auto* rawBinding = new TerminalBinding(&console, control);
  if (!rawBinding) {
    console.releaseVfsReference();
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  auto binding = SharedPointer<Process::ControllingTerminal>::tryAdopt(rawBinding);
  if (!binding) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  if (old)
    old->invalidate();
  console.setControlState(control);
  process->setCttyContext(binding);
  return 0;
}

int TerminalControl::setForeground(ConsoleFile& console, int group,
                                   const SharedPointer<ConsoleIoState>& opened) {
  TerminationDeferral deferral;
  LockGuard<Mutex> guard(terminalPolicy);
  if (opened && opened->revoked()) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }
  PosixProcess* process = currentProcess();
  auto slot = console.controlState();
  auto* control = static_cast<TerminalControl*>(slot.get());
  if (!process || !control || !control->active() || !currentTerminal(*process, console) ||
      control->m_Session != process->getSessionId()) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }
  if (group < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  {
    RecursingLockGuard<Spinlock> groupGuard(ProcessGroupManager::instance().lock());
    ProcessGroup* target = ProcessGroupManager::instance().findGroup(group);
    if (!target || target->sessionId != control->m_Session) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return -1;
    }
  }
  __atomic_store_n(&control->m_Foreground, static_cast<size_t>(group), __ATOMIC_RELEASE);
  return 0;
}

int TerminalControl::foreground(ConsoleFile& console, const SharedPointer<ConsoleIoState>& opened) {
  LockGuard<Mutex> guard(terminalPolicy);
  if (opened && opened->revoked()) {
    SYSCALL_ERROR(IoError);
    return -1;
  }
  PosixProcess* process = currentProcess();
  auto slot = console.controlState();
  auto* control = static_cast<TerminalControl*>(slot.get());
  if (!process || !control || !control->active() || !currentTerminal(*process, console) ||
      control->m_Session != process->getSessionId()) {
    SYSCALL_ERROR(NotAConsole);
    return -1;
  }
  return static_cast<int>(__atomic_load_n(&control->m_Foreground, __ATOMIC_ACQUIRE));
}

int TerminalControl::hangup() {
  TerminationDeferral deferral;
  Process* caller = Processor::information().getCurrentThread()->getParent();
  if (caller->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  Scheduler::ProcessLease leader;
  SharedPointer<Process::ControllingTerminal> context;
  SharedPointer<ConsoleIoState> retired, replacement;
  ConsoleFile* console = nullptr;
  {
    LockGuard<Mutex> guard(terminalPolicy);
    context = caller->acquireCttyContext();
    File* file = context ? context->file() : nullptr;
    if (!file)
      return 0;
    if (!ConsoleManager::instance().isConsole(file)) {
      SYSCALL_ERROR(NotAConsole);
      return -1;
    }
    console = static_cast<ConsoleFile*>(file);
    replacement = SharedPointer<ConsoleIoState>::tryAllocate();
    if (!replacement) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    auto slot = console->controlState();
    auto* control = static_cast<TerminalControl*>(slot.get());
    if (!console->beginRevocation(retired)) {
      SYSCALL_ERROR(DeviceBusy);
      return -1;
    }
    if (control && control->active()) {
      if (Scheduler::instance().acquireProcessByUserspaceId(leader, control->m_Session) &&
          (leader->getType() != Process::Posix ||
           static_cast<PosixProcess*>(leader.get())->getSessionId() != control->m_Session))
        leader.reset();
      control->invalidate();
    }
    console->setControlState(SharedPointer<ConsoleControlState>());
    caller->setCttyContext(SharedPointer<Process::ControllingTerminal>());
  }
  console->finishRevocation(retired, replacement);
  if (leader) {
    Process::ThreadLease thread;
    auto* subsystem = static_cast<PosixSubsystem*>(leader->getSubsystem());
    if (subsystem && leader->acquireProcessSignalThread(thread)) {
      subsystem->sendSignal(thread.get(), SIGHUP, false, true);
      subsystem->sendSignal(thread.get(), SIGCONT, false, true);
    }
  }
  return 0;
}

void TerminalControl::processTerminated(PosixProcess& process) {
  TerminationDeferral deferral;
  size_t session = 0, foreground = 0;
  SharedPointer<Process::ControllingTerminal> context;
  SharedPointer<ConsoleIoState> retired;
  ConsoleFile* revoking = nullptr;
  {
    LockGuard<Mutex> guard(terminalPolicy);
    context = process.acquireCttyContext();
    File* file = context ? context->file() : nullptr;
    if (file && ConsoleManager::instance().isConsole(file)) {
      auto* console = static_cast<ConsoleFile*>(file);
      auto slot = console->controlState();
      auto* control = static_cast<TerminalControl*>(slot.get());
      // A PTY master must be able to drain output after its session leader exits.
      const bool preservePtyData = console->isPtySlave();
      if (control && control->active() && control->m_Session == process.getUserspaceId() &&
          (preservePtyData || console->beginRevocation(retired))) {
        session = control->m_Session;
        foreground = __atomic_load_n(&control->m_Foreground, __ATOMIC_ACQUIRE);
        control->invalidate();
        console->setControlState(SharedPointer<ConsoleControlState>());
        if (!preservePtyData)
          revoking = console;
      }
    }
    process.setCttyContext(SharedPointer<Process::ControllingTerminal>());
  }
  // Exit cannot fail allocation. A later open prepares its own fresh state.
  if (revoking)
    revoking->finishRevocation(retired, SharedPointer<ConsoleIoState>());
  if (!session || !foreground)
    return;
  for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i) {
    Scheduler::ProcessLease recipient;
    if (!Scheduler::instance().acquireProcess(recipient, i) ||
        recipient->getType() != Process::Posix || recipient.get() == &process)
      continue;
    auto* target = static_cast<PosixProcess*>(recipient.get());
    size_t group = 0;
    if (target->getSessionId() != session || !target->getProcessGroupId(group) ||
        group != foreground)
      continue;
    Process::ThreadLease thread;
    auto* subsystem = static_cast<PosixSubsystem*>(target->getSubsystem());
    if (subsystem && target->acquireProcessSignalThread(thread)) {
      subsystem->sendSignal(thread.get(), SIGHUP, false, true);
      subsystem->sendSignal(thread.get(), SIGCONT, false, true);
    }
  }
}

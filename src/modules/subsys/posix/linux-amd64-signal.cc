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

#include "linux-amd64-signal.h"

#if X64

#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/NMFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/lib.h"

#include <errno.h>
#include <signal.h>

#include "linux-amd64-signal-abi.h"
#include "signal-syscalls.h"
#include "syscalls/translate.h"
#include "system-syscalls.h"

namespace {
using namespace LinuxAmd64SignalAbi;

constexpr uint64_t UnblockableSignals =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));
constexpr uint64_t HandlerFlagsToClear = 0x100 | 0x400 | 0x10000;
constexpr uint64_t RestorableRflags = 0x50DD5;
constexpr uint64_t SafeUserRflags = 0x202;
constexpr uint64_t SupportedUcontextFlags = 0x6;
constexpr uintptr_t MaximumCanonicalUserAddress = 0x00007FFFFFFFFFFF;
constexpr uint16_t IretUserCodeSegment = 0x1B;
constexpr uint16_t SysretUserCodeSegment = 0x2B;
constexpr uint16_t UserStackSegment = 0x23;

bool userCodeSegment(uint16_t selector) {
  // Initial userspace entry uses IRET, while every subsequent syscall return
  // derives the second valid code selector from IA32_STAR.
  return selector == IretUserCodeSegment || selector == SysretUserCodeSegment;
}

uintptr_t alignDown(uintptr_t value, uintptr_t alignment) {
  return value & ~(alignment - 1);
}

bool userBounds(uintptr_t address, size_t extent) {
  if (!extent || !address || extent - 1 > (~static_cast<uintptr_t>(0) - address)) {
    return false;
  }

  uintptr_t end = address + extent - 1;
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  return address >= va.getUserStart() && end <= MaximumCanonicalUserAddress &&
         end < va.getKernelStart() && va.isAddressValid(reinterpret_cast<void*>(address)) &&
         va.isAddressValid(reinterpret_cast<void*>(end));
}

bool userRegion(uintptr_t address, size_t extent, size_t access) {
  if (!userBounds(address, extent)) {
    return false;
  }

  // Keep the inclusive-end check local to the signal copy boundary even if
  // the shared range validator changes.
  return PosixSubsystem::checkAddress(address, extent, access) &&
         PosixSubsystem::checkAddress(address + extent - 1, 1, access);
}

bool userExecutable(uintptr_t address) {
  return userRegion(address, 1, PosixSubsystem::SafeExecute);
}

bool onAlternateStack(uintptr_t stackPointer, const Thread::AlternateSignalStack& stack) {
  if (!stack.enabled || !stack.base || stack.size > (~static_cast<uintptr_t>(0) - stack.base)) {
    return false;
  }

  return stackPointer >= stack.base && stackPointer < stack.base + stack.size;
}

void setSiginfo32(Siginfo& info, size_t offset, int32_t value) {
  MemoryCopy(info.bytes + offset, &value, sizeof(value));
}

void setSiginfo64(Siginfo& info, size_t offset, uint64_t value) {
  MemoryCopy(info.bytes + offset, &value, sizeof(value));
}

void populateSigcontext(Sigcontext& context, const InterruptState& state, uint64_t oldMask,
                        uintptr_t faultAddress, uintptr_t errorCode, uintptr_t fpstate) {
  context.rax = state.getRegister(0);
  context.rbx = state.getRegister(1);
  context.rcx = state.getRegister(2);
  context.rdx = state.getRegister(3);
  context.rdi = state.getRegister(4);
  context.rsi = state.getRegister(5);
  context.rbp = state.getRegister(6);
  context.r8 = state.getRegister(7);
  context.r9 = state.getRegister(8);
  context.r10 = state.getRegister(9);
  context.r11 = state.getRegister(10);
  context.r12 = state.getRegister(11);
  context.r13 = state.getRegister(12);
  context.r14 = state.getRegister(13);
  context.r15 = state.getRegister(14);
  context.rsp = state.getStackPointer();
  context.rip = state.getInstructionPointer();
  context.rflags = state.getFlags();
  context.cs = state.getCodeSegment();
  context.ss = state.getStackSegment();
  context.errorCode = errorCode;
  context.trapNumber = state.getInterruptNumber();
  context.oldMask = oldMask;
  context.cr2 = faultAddress;
  context.fpstate = fpstate;
}

void populateSigcontext(Sigcontext& context, const SyscallState& state, uint64_t oldMask,
                        uintptr_t fpstate) {
  context.rax = state.getRegister(0);
  context.rbx = state.getRegister(1);
  context.rcx = state.getInstructionPointer();
  context.rdx = state.getRegister(2);
  context.rdi = state.getRegister(3);
  context.rsi = state.getRegister(4);
  context.rbp = state.getRegister(5);
  context.r8 = state.getRegister(6);
  context.r9 = state.getRegister(7);
  context.r10 = state.getRegister(8);
  context.r11 = state.getFlags();
  context.r12 = state.getRegister(9);
  context.r13 = state.getRegister(10);
  context.r14 = state.getRegister(11);
  context.r15 = state.getRegister(12);
  context.rsp = state.getStackPointer();
  context.rip = state.getInstructionPointer();
  context.rflags = state.getFlags();
  context.cs = SysretUserCodeSegment;
  context.ss = UserStackSegment;
  context.oldMask = oldMask;
  context.fpstate = fpstate;
}

int signalCode(Subsystem::ExceptionType exception, uintptr_t errorCode) {
  switch (exception) {
    case Subsystem::PageFault:
      return (errorCode & 1) ? SEGV_ACCERR : SEGV_MAPERR;
    case Subsystem::InvalidOpcode:
      return ILL_ILLOPC;
    case Subsystem::GeneralProtectionFault:
    case Subsystem::FileMappingFault:
      return BUS_ADRERR;
    case Subsystem::DivideByZero:
      return FPE_INTDIV;
    case Subsystem::FpuError:
    case Subsystem::SpecialFpuError:
      return FPE_FLTINV;
    default:
      return SI_KERNEL;
  }
}

uintptr_t signalAddress(Subsystem::ExceptionType exception, const InterruptState& state,
                        uintptr_t faultAddress) {
  return exception == Subsystem::PageFault || exception == Subsystem::FileMappingFault
             ? faultAddress
             : state.getInstructionPointer();
}

void badFrame() {
  Processor::information().getCurrentThread()->deferSignalExit(SIGSEGV);
}

struct AsyncHandlerState {
  uintptr_t frameAddress;
  uintptr_t infoAddress;
  uintptr_t ucontextAddress;
  uintptr_t handlerAddress;
  uint64_t flags;
};

bool restartableSyscall(const SyscallState& state) {
  switch (state.getSyscallNumber()) {
    case PedigreeLinuxAmd64Syscall_read:
    case PedigreeLinuxAmd64Syscall_write:
    case PedigreeLinuxAmd64Syscall_open:
    case PedigreeLinuxAmd64Syscall_openat:
    case PedigreeLinuxAmd64Syscall_readv:
    case PedigreeLinuxAmd64Syscall_writev:
    case PedigreeLinuxAmd64Syscall_pread64:
    case PedigreeLinuxAmd64Syscall_pwrite64:
    case PedigreeLinuxAmd64Syscall_preadv:
    case PedigreeLinuxAmd64Syscall_pwritev:
    case PedigreeLinuxAmd64Syscall_preadv2:
    case PedigreeLinuxAmd64Syscall_pwritev2:
    case PedigreeLinuxAmd64Syscall_wait4:
    case PedigreeLinuxAmd64Syscall_accept:
    case PedigreeLinuxAmd64Syscall_accept4:
    case PedigreeLinuxAmd64Syscall_sendto:
    case PedigreeLinuxAmd64Syscall_recvfrom:
    case PedigreeLinuxAmd64Syscall_sendmsg:
    case PedigreeLinuxAmd64Syscall_recvmsg:
    case PedigreeLinuxAmd64Syscall_mq_timedsend:
    case PedigreeLinuxAmd64Syscall_mq_timedreceive:
      return true;
    case PedigreeLinuxAmd64Syscall_futex:
      // Replaying a relative timeout would extend the caller's deadline.
      // Only the implemented, untimed FUTEX_WAIT operation is eligible.
      return (state.getSyscallParameter(7) & ~static_cast<uintptr_t>(128)) == 0 &&
             state.getSyscallParameter(9) == 0;
    default:
      // Sleeps and poll/select/epoll waits always report interruption.
      // connect may already own an in-flight attempt when interrupted.
      return false;
  }
}

void restartInterruptedSyscall(Thread* thread, uint32_t flags, Sigcontext& context) {
  const SyscallState* original = thread->getOriginalSyscallState();
  if (!(flags & SA_RESTART) || context.rax != static_cast<uint64_t>(-EINTR) || !original ||
      original->getSyscallService() != linuxCompat ||
      context.rip != original->getInstructionPointer() ||
      context.rsp != original->getStackPointer() || context.rip < 2 ||
      !restartableSyscall(*original)) {
    return;
  }

  // The entry image predates result publication and ABI argument translation.
  // Replaying only -EINTR leaves all successful partial transfers untouched.
  context.rax = original->getRegister(0);
  context.rdi = original->getSyscallParameter(6);
  context.rsi = original->getSyscallParameter(7);
  context.rdx = original->getSyscallParameter(8);
  context.r10 = original->getSyscallParameter(9);
  context.r8 = original->getSyscallParameter(10);
  context.r9 = original->getSyscallParameter(11);
  context.rip -= 2;
}

Event::UserReturnDelivery resolveAsyncDisposition(Thread* thread, SignalEvent& event,
                                                  PosixSubsystem::SignalDisposition& disposition) {
  if (!event.deliveryActive())
    return Event::UserReturnDelivery::Delivered;
  const size_t signal = event.getNumber();
  Process* process = thread ? thread->getParent() : nullptr;
  PosixSubsystem* subsystem =
      process ? static_cast<PosixSubsystem*>(process->getSubsystem()) : nullptr;
  if (!subsystem || !subsystem->getSignalDisposition(signal, disposition, true)) {
    badFrame();
    return Event::UserReturnDelivery::Failed;
  }
  if (disposition.type == 0) {
    return Event::UserReturnDelivery::NotApplicable;
  }

  if (disposition.type == 1 && signal != SIGCHLD && signal != SIGURG && signal != SIGWINCH) {
    thread->setCurrentSignalDelivery(signal, event.getContinuationEpoch());
    reinterpret_cast<_sig_func_ptr>(disposition.handler)(static_cast<int>(signal));
  }
  return Event::UserReturnDelivery::Delivered;
}

bool buildAsyncFrame(Thread* thread, LinuxAmd64Signal::AsyncEvent& event, Sigcontext& context,
                     const PosixSubsystem::SignalDisposition& disposition,
                     AsyncHandlerState& handlerState) {
  const int signal = static_cast<int>(event.getNumber());
  if (!thread || thread != Processor::information().getCurrentThread() || signal <= 0 ||
      signal > 64 || !userCodeSegment(context.cs) || context.ss != UserStackSegment ||
      !(disposition.flags & SA_RESTORER) || !userExecutable(disposition.handler) ||
      !userExecutable(disposition.restorer)) {
    return false;
  }

  const uintptr_t originalStack = context.rsp;
  if (!userBounds(originalStack, 1)) {
    return false;
  }

  Thread::AlternateSignalStack& alternate = thread->getAlternateSignalStack();
  const bool wasOnAlternate = onAlternateStack(originalStack, alternate);
  const bool enterAlternate =
      (disposition.flags & SA_ONSTACK) && alternate.enabled && !wasOnAlternate;

  uintptr_t stackTop = 0;
  if (enterAlternate) {
    if (!alternate.base || alternate.size > (~static_cast<uintptr_t>(0) - alternate.base)) {
      return false;
    }
    stackTop = alternate.base + alternate.size;
  } else {
    if (originalStack < 128) {
      return false;
    }
    stackTop = originalStack - 128;
  }

  if (stackTop < sizeof(Fpstate)) {
    return false;
  }
  const uintptr_t fpstateAddress = alignDown(stackTop - sizeof(Fpstate), 64);
  if (fpstateAddress < sizeof(RtSigframe) + 8) {
    return false;
  }
  const uintptr_t frameAddress = alignDown(fpstateAddress - sizeof(RtSigframe), 16) - 8;

  if (!userRegion(frameAddress, sizeof(RtSigframe), PosixSubsystem::SafeWrite) ||
      !userRegion(fpstateAddress, sizeof(Fpstate), PosixSubsystem::SafeWrite)) {
    return false;
  }
  if (enterAlternate || wasOnAlternate) {
    const uintptr_t alternateEnd = alternate.base + alternate.size;
    if (frameAddress < alternate.base || fpstateAddress + sizeof(Fpstate) > alternateEnd) {
      return false;
    }
  }

  RtSigframe frame = {};
  Fpstate savedFpstate = {};
  if (!NMFaultHandler::saveCurrentThreadFpuState(&savedFpstate, true)) {
    return false;
  }
  Fpstate fpstate = savedFpstate;
  ByteSet(fpstate.reserved3, 0, sizeof(fpstate.reserved3));

  const uint64_t currentMask = thread->getSignalMask();
  const uint64_t oldMask = thread->getSignalMaskForReturnFrame();
  frame.restorer = disposition.restorer;
  frame.ucontext.flags = SupportedUcontextFlags;
  if (alternate.enabled) {
    frame.ucontext.stack.stackPointer = alternate.base;
    frame.ucontext.stack.size = alternate.size;
    frame.ucontext.stack.flags = wasOnAlternate ? SS_ONSTACK : 0;
  } else {
    frame.ucontext.stack.flags = SS_DISABLE;
  }
  context.oldMask = oldMask;
  context.fpstate = fpstateAddress;
  frame.ucontext.mcontext = context;
  frame.ucontext.signalMask = oldMask;

  setSiginfo32(frame.info, 0, signal);
  setSiginfo32(frame.info, 4, 0);
  setSiginfo32(frame.info, 8, event.getSignalCode());
  setSiginfo32(frame.info, 16, event.getSenderProcess());
  setSiginfo32(frame.info, 20, static_cast<int32_t>(event.getSenderUser()));
  setSiginfo64(frame.info, 24, event.getSignalValue());
  int32_t timerId = 0, overrun = 0;
  event.timerInfo(timerId, overrun);
  if (event.getSignalCode() == -2) {
    setSiginfo32(frame.info, 16, timerId);
    setSiginfo32(frame.info, 20, overrun);
  }

  if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(fpstateAddress), &fpstate,
                                  sizeof(fpstate)) ||
      !PosixSubsystem::copyToUser(reinterpret_cast<void*>(frameAddress), &frame, sizeof(frame))) {
    if (!NMFaultHandler::restoreCurrentThreadFpuState(&savedFpstate)) {
      FATAL("Could not restore FPU state after a failed signal-frame copy.");
    }
    return false;
  }

  event.completeSignalDelivery(overrun);
  uint64_t handlerMask = currentMask | disposition.signalMask;
  if (!(disposition.flags & SA_NODEFER)) {
    handlerMask |= static_cast<uint64_t>(1) << (signal - 1);
  }
  thread->commitSignalHandlerMask(handlerMask & ~UnblockableSignals);
  alternate.inUse = wasOnAlternate || enterAlternate;

  handlerState.frameAddress = frameAddress;
  handlerState.infoAddress = frameAddress + __builtin_offsetof(RtSigframe, info);
  handlerState.ucontextAddress = frameAddress + __builtin_offsetof(RtSigframe, ucontext);
  handlerState.handlerAddress = disposition.handler;
  handlerState.flags = (context.rflags & ~HandlerFlagsToClear) | SafeUserRflags;
  return true;
}
}  // namespace

LinuxAmd64Signal::AsyncEvent::AsyncEvent(uintptr_t handler, size_t signal, uint64_t signalMask,
                                         bool deferSignal, uint32_t flags, uintptr_t restorer,
                                         bool useAlternateStack, bool isDeletable)
    : SignalEvent(handler, signal, ~0UL, signalMask, deferSignal, isDeletable,
                  Event::HandlerPrivilege::User, DeliveryDisposition::CaughtHandler,
                  useAlternateStack),
      m_Flags(flags),
      m_Restorer(restorer) {}

LinuxAmd64Signal::AsyncEvent::AsyncEvent(const AsyncEvent& other, bool isDeletable)
    : SignalEvent(other, isDeletable), m_Flags(other.m_Flags), m_Restorer(other.m_Restorer) {}

Event* LinuxAmd64Signal::AsyncEvent::cloneForDelivery() {
  if (isDeletable()) {
    return this;
  }
  return new AsyncEvent(*this, true);
}

SignalEvent* LinuxAmd64Signal::AsyncEvent::cloneForDisposition() {
  return new AsyncEvent(*this, false);
}

Event::UserReturnDelivery LinuxAmd64Signal::AsyncEvent::deliverAtUserReturn(InterruptState& state) {
  Sigcontext context = {};
  populateSigcontext(context, state, 0, 0, state.getErrorCode(), 0);
  AsyncHandlerState handlerState = {};
  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem::SignalDisposition disposition;
  const Event::UserReturnDelivery resolved = resolveAsyncDisposition(thread, *this, disposition);
  if (resolved != Event::UserReturnDelivery::NotApplicable) {
    return resolved;
  }
  if (!buildAsyncFrame(thread, *this, context, disposition, handlerState)) {
    badFrame();
    return Event::UserReturnDelivery::Failed;
  }

  state.setRegister(0, 0);
  state.setRegister(4, getNumber());
  state.setRegister(5, handlerState.infoAddress);
  state.setRegister(3, handlerState.ucontextAddress);
  state.setInstructionPointer(handlerState.handlerAddress);
  state.setStackPointer(handlerState.frameAddress);
  state.setFlags(handlerState.flags);
  return Event::UserReturnDelivery::Delivered;
}

Event::UserReturnDelivery LinuxAmd64Signal::AsyncEvent::deliverAtUserReturn(SyscallState& state) {
  Sigcontext context = {};
  populateSigcontext(context, state, 0, 0);
  AsyncHandlerState handlerState = {};
  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem::SignalDisposition disposition;
  const Event::UserReturnDelivery resolved = resolveAsyncDisposition(thread, *this, disposition);
  if (resolved != Event::UserReturnDelivery::NotApplicable) {
    return resolved;
  }
  restartInterruptedSyscall(thread, disposition.flags, context);
  if (!buildAsyncFrame(thread, *this, context, disposition, handlerState)) {
    badFrame();
    return Event::UserReturnDelivery::Failed;
  }

  state.setRegister(0, 0);
  state.setRegister(3, getNumber());
  state.setRegister(4, handlerState.infoAddress);
  state.setRegister(2, handlerState.ucontextAddress);
  state.setInstructionPointer(handlerState.handlerAddress);
  state.setStackPointer(handlerState.frameAddress);
  state.setFlags(handlerState.flags);
  return Event::UserReturnDelivery::Delivered;
}

LinuxAmd64Signal::DeliveryResult LinuxAmd64Signal::deliverSynchronous(
    Thread* thread, int signal, const PosixSubsystem::SignalDisposition& disposition,
    Subsystem::ExceptionType exception, InterruptState& state, uintptr_t faultAddress,
    uintptr_t errorCode) {
  if (!thread || thread != Processor::information().getCurrentThread() || disposition.type != 0) {
    return NotApplicable;
  }

  if (signal <= 0 || signal > 64 || state.kernelMode() ||
      !userCodeSegment(state.getCodeSegment()) || state.getStackSegment() != UserStackSegment ||
      !(disposition.flags & SA_RESTORER) || !userExecutable(disposition.handler) ||
      !userExecutable(disposition.restorer)) {
    return Failed;
  }

  uintptr_t originalStack = state.getStackPointer();
  if (!userBounds(originalStack, 1)) {
    return Failed;
  }

  Thread::AlternateSignalStack& alternate = thread->getAlternateSignalStack();
  bool wasOnAlternate = onAlternateStack(originalStack, alternate);
  bool enterAlternate = (disposition.flags & SA_ONSTACK) && alternate.enabled && !wasOnAlternate;

  uintptr_t stackTop = 0;
  if (enterAlternate) {
    if (!alternate.base || alternate.size > (~static_cast<uintptr_t>(0) - alternate.base)) {
      return Failed;
    }
    stackTop = alternate.base + alternate.size;
  } else {
    if (originalStack < 128) {
      return Failed;
    }
    stackTop = originalStack - 128;
  }

  if (stackTop < sizeof(Fpstate)) {
    return Failed;
  }
  uintptr_t fpstateAddress = alignDown(stackTop - sizeof(Fpstate), 64);
  if (fpstateAddress < sizeof(RtSigframe) + 8) {
    return Failed;
  }
  uintptr_t frameAddress = alignDown(fpstateAddress - sizeof(RtSigframe), 16) - 8;

  if (!userRegion(frameAddress, sizeof(RtSigframe), PosixSubsystem::SafeWrite) ||
      !userRegion(fpstateAddress, sizeof(Fpstate), PosixSubsystem::SafeWrite)) {
    return Failed;
  }

  if (enterAlternate || wasOnAlternate) {
    uintptr_t alternateEnd = alternate.base + alternate.size;
    if (frameAddress < alternate.base || fpstateAddress + sizeof(Fpstate) > alternateEnd) {
      return Failed;
    }
  }

  RtSigframe frame = {};
  Fpstate savedFpstate = {};
  if (!NMFaultHandler::saveCurrentThreadFpuState(&savedFpstate, true)) {
    return Failed;
  }
  Fpstate fpstate = savedFpstate;
  ByteSet(fpstate.reserved3, 0, sizeof(fpstate.reserved3));

  const uint64_t currentMask = thread->getSignalMask();
  const uint64_t oldMask = thread->getSignalMaskForReturnFrame();
  frame.restorer = disposition.restorer;
  frame.ucontext.flags = SupportedUcontextFlags;
  if (alternate.enabled) {
    frame.ucontext.stack.stackPointer = alternate.base;
    frame.ucontext.stack.size = alternate.size;
    frame.ucontext.stack.flags = wasOnAlternate ? SS_ONSTACK : 0;
  } else {
    frame.ucontext.stack.flags = SS_DISABLE;
  }
  populateSigcontext(frame.ucontext.mcontext, state, oldMask,
                     exception == Subsystem::PageFault || exception == Subsystem::FileMappingFault
                         ? faultAddress
                         : 0,
                     errorCode, fpstateAddress);
  frame.ucontext.signalMask = oldMask;

  setSiginfo32(frame.info, 0, signal);
  setSiginfo32(frame.info, 4, 0);
  setSiginfo32(frame.info, 8, signalCode(exception, errorCode));
  setSiginfo64(frame.info, 16, signalAddress(exception, state, faultAddress));
  setSiginfo32(frame.info, 24, static_cast<int32_t>(state.getInterruptNumber()));

  if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(fpstateAddress), &fpstate,
                                  sizeof(fpstate)) ||
      !PosixSubsystem::copyToUser(reinterpret_cast<void*>(frameAddress), &frame, sizeof(frame))) {
    if (!NMFaultHandler::restoreCurrentThreadFpuState(&savedFpstate)) {
      FATAL("Could not restore FPU state after a failed signal-frame copy.");
    }
    return Failed;
  }

  uint64_t handlerMask = currentMask | disposition.signalMask;
  if (!(disposition.flags & SA_NODEFER)) {
    handlerMask |= static_cast<uint64_t>(1) << (signal - 1);
  }
  thread->commitSignalHandlerMask(handlerMask & ~UnblockableSignals);
  alternate.inUse = wasOnAlternate || enterAlternate;

  state.setRegister(0, 0);
  state.setRegister(4, static_cast<uintptr_t>(signal));
  state.setRegister(5, frameAddress + __builtin_offsetof(RtSigframe, info));
  state.setRegister(3, frameAddress + __builtin_offsetof(RtSigframe, ucontext));
  state.setInstructionPointer(disposition.handler);
  state.setStackPointer(frameAddress);
  state.setFlags((state.getFlags() & ~HandlerFlagsToClear) | SafeUserRflags);
  return Delivered;
}

void LinuxAmd64Signal::sigreturn(SyscallState& state) {
  uintptr_t syscallStack = state.getStackPointer();
  if (syscallStack < 8) {
    badFrame();
    return;
  }
  uintptr_t frameAddress = syscallStack - 8;
  if (!userRegion(frameAddress, sizeof(RtSigframe), PosixSubsystem::SafeRead)) {
    badFrame();
    return;
  }

  RtSigframe frame = {};
  if (!PosixSubsystem::copyFromUser(&frame, reinterpret_cast<const void*>(frameAddress),
                                    sizeof(frame))) {
    badFrame();
    return;
  }
  const Sigcontext& context = frame.ucontext.mcontext;

  if ((frame.ucontext.flags & ~SupportedUcontextFlags) || !userCodeSegment(context.cs) ||
      context.ss != UserStackSegment || !userBounds(context.rip, 1) ||
      !userBounds(context.rsp, 1) || !context.fpstate || (context.fpstate & 0x3F) ||
      !userRegion(context.fpstate, sizeof(Fpstate), PosixSubsystem::SafeRead)) {
    badFrame();
    return;
  }

  int alternateFlags = frame.ucontext.stack.flags;
  if (alternateFlags & ~(SS_ONSTACK | SS_DISABLE) ||
      ((alternateFlags & SS_ONSTACK) && (alternateFlags & SS_DISABLE))) {
    badFrame();
    return;
  }

  Thread::AlternateSignalStack restoredAlternate;
  if (!(alternateFlags & SS_DISABLE)) {
    restoredAlternate.base = frame.ucontext.stack.stackPointer;
    restoredAlternate.size = frame.ucontext.stack.size;
    restoredAlternate.enabled = true;
    restoredAlternate.inUse = alternateFlags & SS_ONSTACK;
    if (restoredAlternate.size < MINSIGSTKSZ ||
        !userBounds(restoredAlternate.base, restoredAlternate.size) ||
        (restoredAlternate.inUse && !onAlternateStack(context.rsp, restoredAlternate))) {
      badFrame();
      return;
    }
  }

  Fpstate fpstate = {};
  if (!PosixSubsystem::copyFromUser(&fpstate, reinterpret_cast<const void*>(context.fpstate),
                                    sizeof(fpstate))) {
    badFrame();
    return;
  }
  for (size_t i = 0; i < 12; ++i) {
    if (fpstate.reserved3[i]) {
      badFrame();
      return;
    }
  }
  if (!NMFaultHandler::restoreCurrentThreadFpuState(&fpstate)) {
    badFrame();
    return;
  }

  Thread* thread = Processor::information().getCurrentThread();
  thread->setSignalMask(frame.ucontext.signalMask & ~UnblockableSignals);
  thread->getAlternateSignalStack() = restoredAlternate;

  X64ProcessorState restored;
  restored.r8 = context.r8;
  restored.r9 = context.r9;
  restored.r10 = context.r10;
  restored.r11 = context.r11;
  restored.r12 = context.r12;
  restored.r13 = context.r13;
  restored.r14 = context.r14;
  restored.r15 = context.r15;
  restored.rdi = context.rdi;
  restored.rsi = context.rsi;
  restored.rbp = context.rbp;
  restored.rbx = context.rbx;
  restored.rdx = context.rdx;
  restored.rax = context.rax;
  restored.rcx = context.rcx;
  restored.rip = context.rip;
  restored.rflags = (context.rflags & RestorableRflags) | SafeUserRflags;
  restored.rsp = context.rsp;

  if (!SyscallManager::instance().requestStateRestore(restored)) {
    FATAL("Signal state restoration was not dispatched.");
  }
}

#endif

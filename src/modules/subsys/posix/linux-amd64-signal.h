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

#ifndef POSIX_LINUX_AMD64_SIGNAL_H
#define POSIX_LINUX_AMD64_SIGNAL_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/state_forward.h"

#include <config.h>

#include "PosixSubsystem.h"

namespace LinuxAmd64Signal {
enum DeliveryResult { NotApplicable, Delivered, Failed };

#if X64
class AsyncEvent final : public SignalEvent {
 public:
  AsyncEvent(uintptr_t handler, size_t signal, uint64_t signalMask, bool deferSignal,
             uint32_t flags, uintptr_t restorer, bool useAlternateStack, bool isDeletable = false);

  bool requiresExactUserReturnState() const override {
    return true;
  }

  Event::UserReturnDelivery deliverAtUserReturn(InterruptState& state) override;
  Event::UserReturnDelivery deliverAtUserReturn(SyscallState& state) override;
  Event* cloneForDelivery() override;
  SignalEvent* cloneForDisposition() override;

  uint32_t getFlags() const {
    return m_Flags;
  }

  uintptr_t getRestorer() const {
    return m_Restorer;
  }

 private:
  AsyncEvent(const AsyncEvent& other, bool isDeletable);

  uint32_t m_Flags;
  uintptr_t m_Restorer;
};

DeliveryResult deliverSynchronous(Thread* thread, int signal,
                                  const PosixSubsystem::SignalDisposition& disposition,
                                  Subsystem::ExceptionType exception, InterruptState& state,
                                  uintptr_t faultAddress, uintptr_t errorCode);

void sigreturn(SyscallState& state);
#endif
}  // namespace LinuxAmd64Signal

#endif

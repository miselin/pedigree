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

#ifndef POSIX_LINUX_AMD64_SIGNAL_ABI_H
#define POSIX_LINUX_AMD64_SIGNAL_ABI_H

#include "pedigree/kernel/processor/types.h"

namespace LinuxAmd64SignalAbi {
struct Stack {
  uint64_t stackPointer;
  int32_t flags;
  uint32_t padding;
  uint64_t size;
};

struct Sigcontext {
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t rdi;
  uint64_t rsi;
  uint64_t rbp;
  uint64_t rbx;
  uint64_t rdx;
  uint64_t rax;
  uint64_t rcx;
  uint64_t rsp;
  uint64_t rip;
  uint64_t rflags;
  uint16_t cs;
  uint16_t gs;
  uint16_t fs;
  uint16_t ss;
  uint64_t errorCode;
  uint64_t trapNumber;
  uint64_t oldMask;
  uint64_t cr2;
  uint64_t fpstate;
  uint64_t reserved[8];
};

struct Ucontext {
  uint64_t flags;
  uint64_t link;
  Stack stack;
  Sigcontext mcontext;
  uint64_t signalMask;
};

struct alignas(8) Siginfo {
  uint8_t bytes[128];
};

struct RtSigframe {
  uint64_t restorer;
  Ucontext ucontext;
  Siginfo info;
};

struct alignas(16) Fpstate {
  uint16_t cwd;
  uint16_t swd;
  uint16_t ftw;
  uint16_t fop;
  uint64_t rip;
  uint64_t rdp;
  uint32_t mxcsr;
  uint32_t mxcsrMask;
  uint32_t stSpace[32];
  uint32_t xmmSpace[64];
  uint32_t reserved2[12];
  uint32_t reserved3[12];
};

static_assert(sizeof(Stack) == 24, "Linux amd64 stack_t must be 24 bytes");
static_assert(sizeof(Sigcontext) == 256, "Linux amd64 sigcontext must be 256 bytes");
static_assert(sizeof(Ucontext) == 304, "Linux amd64 ucontext must be 304 bytes");
static_assert(sizeof(Siginfo) == 128, "Linux siginfo_t must be 128 bytes");
static_assert(alignof(Siginfo) == 8, "Linux siginfo_t must be 8-byte aligned");
static_assert(sizeof(RtSigframe) == 440, "Linux amd64 rt_sigframe base must be 440 bytes");
static_assert(sizeof(Fpstate) == 512, "Linux amd64 fpstate must be 512 bytes");
static_assert(alignof(Fpstate) == 16, "Linux amd64 fpstate must be 16-byte aligned");

static_assert(__builtin_offsetof(Ucontext, mcontext) == 40,
              "Linux amd64 ucontext mcontext offset must be 40");
static_assert(__builtin_offsetof(RtSigframe, ucontext) == 8,
              "Linux amd64 rt_sigframe ucontext offset must be 8");
static_assert(__builtin_offsetof(RtSigframe, info) == 312,
              "Linux amd64 rt_sigframe siginfo offset must be 312");
static_assert(__builtin_offsetof(Sigcontext, fpstate) == 184,
              "Linux amd64 sigcontext fpstate offset must be 184");
}  // namespace LinuxAmd64SignalAbi

#endif

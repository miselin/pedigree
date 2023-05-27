; Copyright (c) 2008-2014, Pedigree Developers
; 
; Please see the CONTRIB file in the root of the source tree for a full
; list of contributors.
; 
; Permission to use, copy, modify, and distribute this software for any
; purpose with or without fee is hereby granted, provided that the above
; copyright notice and this permission notice appear in all copies.
; 
; THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
; WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
; MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
; ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
; WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
; ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
; OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

; uintptr_t ProcessorBase::getBasePointer()
global _ZN13ProcessorBase14getBasePointerEv:function hidden
; uintptr_t ProcessorBase::getStackPointer()
global _ZN13ProcessorBase15getStackPointerEv:function hidden
; uintptr_t ProcessorBase::getInstructionPointer()
global _ZN13ProcessorBase21getInstructionPointerEv:function hidden
; uintptr_t ProcessorBase::getDebugStatus()
global _ZN13ProcessorBase14getDebugStatusEv:function hidden
; void ProcessorBase::switchToUserMode()
global _ZN13ProcessorBase16switchToUserModeEmm:function hidden
; void ProcessorBase::contextSwitch(InterruptState*)
global _ZN13ProcessorBase13contextSwitchEP17X64InterruptState:function hidden

;##############################################################################
;### Code section #############################################################
;##############################################################################
[bits 64]
[section .text]

_ZN13ProcessorBase14getBasePointerEv:
  mov rax, rbp
  ret

_ZN13ProcessorBase15getStackPointerEv:
  mov rax, rsp
  ret

_ZN13ProcessorBase21getInstructionPointerEv:
  mov rax, [rsp]
  ret

_ZN13ProcessorBase14getDebugStatusEv:
  mov rax, dr6
  ret

_ZN13ProcessorBase13contextSwitchEP17X64InterruptState:
  ; Change the stack pointer to point to the top of the passed InterruptState object.
  mov rsp, rdi

  ; Restore the registers
  pop r15
  pop r14
  pop r13
  pop r12
  pop r11
  pop r10
  pop r9
  pop r8
  pop rbp
  pop rsi
  pop rdi
  pop rdx
  pop rcx
  pop rbx
  pop rax

  ; Remove the errorcode and the interrupt number from the stack
  add rsp, 0x10
  iretq

_ZN13ProcessorBase16switchToUserModeEmm:
  mov ax, 0x23       ; Load the new data segment descriptor with an RPL of 3.
  mov ds, ax         ; Propagate the change to all segment registers.
  mov es, ax
  mov fs, ax
  mov gs, ax

  mov rdx, rdi       ; First parameter is the address to jump to. Store in RDX.
  mov rdi, rsi       ; Second parameter to this function is the first to the called function.

  mov rax, rsp       ; Save the stack pointer before we pushed anything.
  push 0x23          ; push the new stack segment with an RPL of 3.
  push rax           ; Push the value of RSP before we pushed anything.
  pushfq             ; Push the RFLAGS register.

  pop rax            ; Pull the RFLAGS value back.
  or rax, 0x200      ; OR-in 0x200 = 1<<9 = IF = Interrupt flag enable.
  push rax           ; Push the doctored RFLAGS value back.

  push 0x1B          ; Push the new code segment with an RPL of 3.
  push rdx           ; Push the RIP to IRET to.

  iretq

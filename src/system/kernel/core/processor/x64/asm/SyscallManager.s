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

extern pedigree_restore_syscall_entry

; X64SyscallManager::syscall(SyscallState &syscallState)
extern _ZN17X64SyscallManager7syscallER15X64SyscallState

; Export the syscall handler
global syscall_handler:function hidden
global pedigree_syscall_entry_swapgs:function hidden
global pedigree_syscall_entry_kernel_gs:function hidden
global pedigree_syscall_kernel_stack:function hidden
global pedigree_syscall_exit_swapgs:function hidden
global pedigree_syscall_exit_user_gs:function hidden

;##############################################################################
;### Code section #############################################################
;##############################################################################
[bits 64]
[section .text]

;##############################################################################
;### assembler stub for syscalls ##############################################
;##############################################################################
syscall_handler:
  ; Preserve the user's saved RFLAGS in R11 while establishing the kernel ABI.
  cld

  ; IA32_FMASK masks IRQs; paranoid IST entry covers the NMI/exception windows.
pedigree_syscall_entry_swapgs:
  swapgs
pedigree_syscall_entry_kernel_gs:
  lfence
  mov [gs: 8], rsp
  mov rsp, [gs: 0]
pedigree_syscall_kernel_stack:
  push qword [gs: 8]
  push rcx
  push r11
  push rax
  push rbx
  push rdx
  push rdi
  push rsi
  push rbp
  push r8
  push r9
  push r10
  push r12
  push r13
  push r14
  push r15

  sub rsp, 32
  mov rax, [rsp+128]
  mov [rsp+24], rax
  ; Call the C++ handler function
  mov rdi, rsp
  call _ZN17X64SyscallManager7syscallER15X64SyscallState

  cli

  ; A bounded call can retain the original user selectors and inactive GS base.
  test al, al
  jz .fast_return
  mov rdi, rsp
  call pedigree_restore_syscall_entry

.fast_return:
  add rsp, 32
        
  pop r15
  pop r14
  pop r13
  pop r12
  pop r10
  pop r9
  pop r8
  pop rbp
  pop rsi
  pop rdi
  pop rdx
  pop rbx
  pop rax
  pop r11
  pop rcx
  pop rsp

pedigree_syscall_exit_swapgs:
  swapgs
pedigree_syscall_exit_user_gs:
  db 0x48
  sysret

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

; void ProcessorBase::jumpUser(volatile uintptr_t *, uintptr_t, uintptr_t,
;                              uintptr_t, uintptr_t, uintptr_t, uintptr_t)
global _ZN13ProcessorBase8jumpUserEPVmmmmmmm
global hostedCaptureKernelFs
global hostedSetKernelFs
global hostedSetUserFs
global hostedSignalTrampoline
global hosted_kernel_fs_base
global hosted_user_fs_base
; void callOnStack(void *, void *, void *, void *, void *, void *)
global callOnStack

extern hostedSignalHandler

%define LINUX_SYS_ARCH_PRCTL 158
%define LINUX_ARCH_SET_FS 0x1002
%define LINUX_ARCH_GET_FS 0x1003

[bits 64]
[section .text]

hostedCaptureKernelFs:
    mov eax, LINUX_SYS_ARCH_PRCTL
    mov edi, LINUX_ARCH_GET_FS
    lea rsi, [rel hosted_kernel_fs_base]
    syscall
    ret

hostedSetKernelFs:
    mov eax, LINUX_SYS_ARCH_PRCTL
    mov edi, LINUX_ARCH_SET_FS
    mov rsi, [rel hosted_kernel_fs_base]
    syscall
    ret

hostedSetUserFs:
    mov eax, LINUX_SYS_ARCH_PRCTL
    mov edi, LINUX_ARCH_SET_FS
    mov rsi, [rel hosted_user_fs_base]
    syscall
    ret

; Linux enters signal handlers without changing FS. Capture the interrupted
; base before entering instrumented C++, then restore it before sigreturn.
hostedSignalTrampoline:
    push rdi
    push rsi
    push rdx
    sub rsp, 16

    mov rax, [rel hosted_kernel_fs_base]
    mov [rsp], rax
    mov eax, LINUX_SYS_ARCH_PRCTL
    mov edi, LINUX_ARCH_GET_FS
    mov rsi, rsp
    syscall

    mov rax, [rsp]
    cmp rax, [rel hosted_kernel_fs_base]
    jne .signal_from_user

    add rsp, 16
    pop rdx
    pop rsi
    pop rdi
    xor ecx, ecx
    jmp hostedSignalHandler WRT ..plt

.signal_from_user:
    call hostedSetKernelFs

    mov rdi, [rsp + 32]
    mov rsi, [rsp + 24]
    mov rdx, [rsp + 16]
    mov ecx, 1
    call hostedSignalHandler WRT ..plt

    mov eax, LINUX_SYS_ARCH_PRCTL
    mov edi, LINUX_ARCH_SET_FS
    mov rsi, [rsp]
    syscall

    add rsp, 16
    pop rdx
    pop rsi
    pop rdi
    ret

; [rsp+0x8] p4
; [r9]     p3
; [r8]     p2
; [rcx]    p1
; [rdx]    stack
; [rsi]    address
; [rdi]    lock
_ZN13ProcessorBase8jumpUserEPVmmmmmmm:
    mov r15, rdi
    mov r13, rsi
    mov r14, rdx
    mov r10, rcx
    mov r12, [rsp + 8]

    ; ELF entry points consume the initial process state directly from RSP.
    mov rsp, r14

    ; The old thread can only be unlocked once its stack is no longer active.
    test r15, r15
    jz .user_no_lock
    mov qword [r15], 1
.user_no_lock:

    call hostedSetUserFs

    ; Preserve the Processor API's parameter calling convention.
    mov rdi, r10
    mov rsi, r8
    mov rdx, r9
    mov rcx, r12

    xor rbp, rbp
    jmp r13

; [r9]     p4
; [r8]     p3
; [rcx]    p2
; [rdx]    p1
; [rsi]    func
; [rdi]    stack
callOnStack:
    push rbp
    mov rbp, rsp

    ; Load function call target and switch stack.
    mov rsp, rdi
    mov r10, rsi

    ; Shuffle parameters into correct registers.
    mov rdi, rdx
    mov rsi, rcx
    mov rdx, r8
    mov rcx, r9

    ; Call desired function.
    call r10

    ; Restore stack and return.
    mov rsp, rbp
    pop rbp
    ret

[section .data]
align 8
hosted_kernel_fs_base:
    dq 0
hosted_user_fs_base:
    dq 0

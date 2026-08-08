; Copyright (c) 2026, Pedigree Developers
;
; Minimal Mach-O x86-64 host boundary. Darwin hosted execution intentionally
; has no emulated userspace FS-base transition; that boundary is rejected by
; Processor.cc before reaching this file.

[bits 64]
[section .text]

; int callOnStack(uintptr_t stack, uintptr_t func,
;                 uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4)
global callOnStack
callOnStack:
    push rbp
    mov rbp, rsp

    mov rsp, rdi
    and rsp, -16
    mov r10, rsi

    mov rdi, rdx
    mov rsi, rcx
    mov rdx, r8
    mov rcx, r9
    call r10

    mov rsp, rbp
    pop rbp
    ret

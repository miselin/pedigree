; Copyright (c) 2026, Pedigree Developers
;
; Terminal hosted-thread handoff for Mach-O x86-64.

[bits 64]
[section .text]

global _ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR20HostedSchedulerStatePVm
extern _ZN21PerProcessorScheduler12deleteThreadEP6Thread
extern _ZN13ProcessorBase12restoreStateER20HostedSchedulerStatePVm

_ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR20HostedSchedulerStatePVm:
    mov rcx, rsi

    test rdx, rdx
    jz .no_caller_lock
    mov qword [rdx], 1
.no_caller_lock:

    xor rbp, rbp
    lea rsp, [rel safe_stack_top]
    sub rsp, 8

    push rcx
    call _ZN21PerProcessorScheduler12deleteThreadEP6Thread
    pop rcx

    mov rdi, rcx
    xor rsi, rsi
    jmp _ZN13ProcessorBase12restoreStateER20HostedSchedulerStatePVm

[section .data]
align 16
global safe_stack_top
safe_stack:
    times 0x10000 db 0
safe_stack_top:

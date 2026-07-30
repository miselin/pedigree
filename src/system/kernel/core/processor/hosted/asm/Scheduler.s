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

; void PerProcessorScheduler::deleteThreadThenRestoreState(Thread*, SchedulerState&)
global _ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR20HostedSchedulerStatePVm
; syscall entry method (at syscall entry address)
global syscall_enter
; void Processor::restoreState(volatile uintptr_t *, SyscallState &)
global _ZN13ProcessorBase12restoreStateER18HostedSyscallStatePVm

; HostedProcessorInformation::getKernelStack() const
extern _ZNK26HostedProcessorInformation14getKernelStackEv
; ProcessorBase::information()
extern _ZN13ProcessorBase11informationEv
; Raw FS-base boundaries for hosted userspace.
extern hostedSetKernelFs
extern hostedSetUserFs

; void PerProcessorScheduler::deleteThread(Thread *)
extern _ZN21PerProcessorScheduler12deleteThreadEP6Thread
; void Processor::restoreState(SchedulerState &, volatile uintptr_t *)
extern _ZN13ProcessorBase12restoreStateER20HostedSchedulerStatePVm

; void HostedSyscallManager::syscall(SyscallState &syscallState)
extern _ZN20HostedSyscallManager7syscallER18HostedSyscallState
; void __sanitizer_start_switch_fiber(void** fake_stack_save,
;                                     const void* bottom, size_t size);
extern __sanitizer_start_switch_fiber

; void *safe_stack_top
global safe_stack_top

[bits 64]
[section .text]

_ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR20HostedSchedulerStatePVm:
    ; Load the state pointer
    mov rcx, rsi

    ; Thread* already in rdi will be passed to PerProcessorScheduler::deleteThread(Thread *)

    ; We need to get OFF the current stack as it may get unmapped by the
    ; Thread deletion coming. We will use a temporary stack for the frame.
    ; TODO: this will break if we have multiprocessing.
    xor rbp, rbp
    mov rsp, safe_stack_top
    sub rsp, 8  ; Align as needed for SSE et al.

    ; Ready to go.
    push rcx
    call _ZN21PerProcessorScheduler12deleteThreadEP6Thread WRT ..plt
    pop rcx

    mov rdi, 0  ; fake_stack
    mov rsi, safe_stack
    mov rdx, 0x10000
    push rcx
    ; call __sanitizer_start_switch_fiber WRT ..plt
    pop rcx

    ; Get out of here (no need to pass a lock).
    mov rdi, rcx
    xor rsi, rsi
    jmp _ZN13ProcessorBase12restoreStateER20HostedSchedulerStatePVm WRT ..plt

; [rsi] Lock
; [rdi] State pointer.
_ZN13ProcessorBase12restoreStateER18HostedSyscallStatePVm:
    ; The state pointer is on the current thread kernel stack, so change to it.
    mov     rsp, rdi

    ; Stack changed, now we can unlock the old thread.
    cmp     rsi, 0
    jz      .no_lock
    ; Release lock.
    mov     qword [rsi], 1
.no_lock:

    jmp syscall_tail

[section .syscall exec]
; [rsp+0x10] p5
; [rsp+0x18] p6
; [rsp+0x8] p4
; [rsp+0x0] (return address)
; [r9]     p3
; [r8]     p2
; [rcx]    p1
; [rdx]    Error*
; [rsi]    Function
; [rdi]    Service
syscall_enter:
    ; Preserve callee-save registers, and the current stack.
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov r12, rsp

    ; Preserve register arguments across the getKernelStack() call. The
    ; stack-based arguments remain above the saved callee-save registers.
    push r9
    push r8
    push rcx
    push rdx
    push rsi
    push rdi

    ; Host libc, sanitizers, and all kernel C++ require the original FS base.
    sub rsp, 8
    call hostedSetKernelFs WRT ..plt

    ; Switch stacks (MUST be a kernel stack).
    call _ZN13ProcessorBase11informationEv WRT ..plt
    mov rdi, rax
    call _ZNK26HostedProcessorInformation14getKernelStackEv
    mov rsp, rax

    ; Create the SyscallState
    push r12
    push 0     ; result (return value)
    push qword [r12 - 32] ; error pointer
    push 0     ; error (to write into [rdx])
    push qword [r12 + 72] ; p6
    push qword [r12 + 64] ; p5
    push qword [r12 + 56] ; p4
    push qword [r12 - 8]  ; p3
    push qword [r12 - 16] ; p2
    push qword [r12 - 24] ; p1
    push qword [r12 - 40] ; function
    push qword [r12 - 48] ; service
    mov rdi, rsp
    call _ZN20HostedSyscallManager7syscallER18HostedSyscallState
syscall_tail:
    pop rdi
    pop rsi
    pop rcx
    pop r8
    pop r9
    pop r10
    pop r11
    pop rcx

    ; error value (modified by syscall)
    pop rax
    pop rdx
    cmp rdx, 0
    je .noerror
    mov [rdx], rax
.noerror:

    ; return value from syscall
    pop rax

    ; Bring back the old stack and callee-save registers.
    pop rsp
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Restore the active Pedigree thread's TLS only for raw userspace.
    mov r8, rax
    sub rsp, 8
    call hostedSetUserFs WRT ..plt
    add rsp, 8
    mov rax, r8
    ret

[section .bss]
align 16
safe_stack:
    resb 0x10000
safe_stack_top:

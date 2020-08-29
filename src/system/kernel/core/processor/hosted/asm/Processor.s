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

; void callOnStack(void *, void *, void *, void *, void *, void *)
global callOnStack

[bits 64]
[section .text]

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

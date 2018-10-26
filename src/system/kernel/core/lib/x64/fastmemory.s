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

; void memzero_xmm_aligned(void *, size_t)
global memzero_xmm_aligned:function
; void memzero_xmm(void *, size_t)
global memzero_xmm:function
        
[bits 64]
[section .text]

; rather quickly zeroes an aligned buffer of a 16-byte multiple length
; [rdi] destination buffer (aligned to 16 bytes)
; [rsi] number of bytes to zero (aligned to 16 bytes)
memzero_xmm_aligned:
    push rbp
    mov rbp, rsp
    and rsp, 0xfffffffffffffff0
    sub rsp, 16
    movdqa [rsp], xmm0
    pxor xmm0, xmm0
    .l:
    movdqa [rdi], xmm0
    add rdi, 16
    sub rsi, 16
    jnz .l
    movdqa xmm0, [rsp]
    mov rsp, rbp
    pop rbp
    ret

; rather quickly zeroes an aligned buffer of a 16-byte multiple length
; [rdi] destination buffer (any alignment)
; [rsi] number of bytes to zero (aligned to 16 bytes)
memzero_xmm:
    push rbp
    mov rbp, rsp
    and rsp, 0xfffffffffffffff0
    sub rsp, 16
    movdqa [rsp], xmm0
    pxor xmm0, xmm0
    .l:
    movdqu [rdi], xmm0
    add rdi, 16
    sub rsi, 16
    jnz .l
    movdqa xmm0, [rsp]
    mov rsp, rbp
    pop rbp
    ret

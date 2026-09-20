; SPDX-License-Identifier: ISC
; A disk-free ROM with a fixed 16-dispatch region and an explicit exception.
bits 16
org 0
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x8000
    mov ds, ax
    mov word [0x80 * 4], handler
    mov word [0x80 * 4 + 2], 0xf000
    jmp start
times 0x40 - ($ - $$) db 0x90
start:
    nop
    mov cx, 3
again:
    inc ax
    loop again
    call leaf
    int 0x80
    nop
    jmp stop
leaf:
    nop
    ret
handler:
    inc bx
    iret
times 0x80 - ($ - $$) db 0x90
stop:
    mov ax, 0x10
    out 0xf4, ax
    hlt
times 0xfff0 - ($ - $$) db 0xff
    jmp 0xf000:0
times 0x10000 - ($ - $$) db 0xff

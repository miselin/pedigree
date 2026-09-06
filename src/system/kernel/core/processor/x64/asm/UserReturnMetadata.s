; Copyright (c) 2026, Pedigree Developers.
[bits 64]
[section .text]
global pedigree_capture_user_entry:function protected
global pedigree_restore_user_entry:function protected

; RDI points at the 32-byte prefix. All clobbered GPRs are already saved.
; origRax belongs to the particular entry/construction path.
pedigree_capture_user_entry:
  mov [rdi], ds
  mov [rdi+2], es
  mov [rdi+4], fs
  mov [rdi+6], gs
  mov ecx, 0xc0000100
  rdmsr
  shl rdx, 32
  or rax, rdx
  mov [rdi+8], rax
  mov ecx, 0xc0000101
  rdmsr
  shl rdx, 32
  or rax, rdx
  mov [rdi+16], rax
  ret

; Selector loads may alter bases: restore the captured bases afterward.
; IA32_KERNEL_GS_BASE remains the scheduler's kernel-stack pointer.
pedigree_restore_user_entry:
  mov ax, [rdi]
  mov ds, ax
  mov ax, [rdi+2]
  mov es, ax
  mov ax, [rdi+4]
  mov fs, ax
  mov ax, [rdi+6]
  mov gs, ax
  mov rax, [rdi+8]
  mov rdx, rax
  shr rdx, 32
  mov ecx, 0xc0000100
  wrmsr
  mov rax, [rdi+16]
  mov rdx, rax
  shr rdx, 32
  mov ecx, 0xc0000101
  wrmsr
  ret

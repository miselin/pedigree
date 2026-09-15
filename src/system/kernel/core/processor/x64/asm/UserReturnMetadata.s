; Copyright (c) 2026, Pedigree Developers.
[bits 64]
[section .text]
global pedigree_capture_user_entry:function protected
global pedigree_restore_user_entry:function protected

%ifdef PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
extern pedigree_record_user_entry_capture
extern pedigree_record_user_entry_restore

[section .bss]
align 8
global pedigree_user_entry_capture_calls:data hidden
pedigree_user_entry_capture_calls:
  resq 1
global pedigree_user_entry_restore_calls:data hidden
pedigree_user_entry_restore_calls:
  resq 1

[section .text]
%endif

%macro CAPTURE_USER_ENTRY 0
%ifndef PEDIGREE_BENCHMARK_ABLATE_X64_USER_ENTRY_METADATA
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
%endif
%endmacro

%macro RESTORE_USER_ENTRY 0
%ifndef PEDIGREE_BENCHMARK_ABLATE_X64_USER_ENTRY_METADATA
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
%endif
%endmacro

; RDI points at the 32-byte prefix. All clobbered GPRs are already saved.
; origRax belongs to the particular entry/construction path.
pedigree_capture_user_entry:
%ifdef PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
  inc qword [rel pedigree_user_entry_capture_calls]
  test byte [rel pedigree_user_entry_capture_calls], 0xff
  jz .sampled
%endif
  CAPTURE_USER_ENTRY
  ret

%ifdef PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
.sampled:
  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  mov r8, rax
  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  sub rax, r8
  mov r9, rax

  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  mov r8, rax
  CAPTURE_USER_ENTRY
  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  sub rax, r8
  mov rdi, rax
  mov rsi, r9
  sub rsp, 8
  call pedigree_record_user_entry_capture
  add rsp, 8
  ret
%endif

; Selector loads may alter bases: restore the captured bases afterward.
; IA32_KERNEL_GS_BASE remains the scheduler's kernel-stack pointer.
pedigree_restore_user_entry:
%ifdef PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
  inc qword [rel pedigree_user_entry_restore_calls]
  test byte [rel pedigree_user_entry_restore_calls], 0xff
  jz .sampled
%endif
  RESTORE_USER_ENTRY
  ret

%ifdef PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
.sampled:
  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  mov r8, rax
  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  sub rax, r8
  mov r9, rax

  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  mov r8, rax
  RESTORE_USER_ENTRY
  lfence
  rdtsc
  shl rdx, 32
  or rax, rdx
  sub rax, r8
  mov rdi, rax
  mov rsi, r9
  sub rsp, 8
  call pedigree_record_user_entry_restore
  add rsp, 8
  ret
%endif

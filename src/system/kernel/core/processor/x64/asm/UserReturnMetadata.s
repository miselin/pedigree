; Copyright (c) 2026, Pedigree Developers.
[bits 64]
[section .text]
global pedigree_capture_user_entry:function protected
global pedigree_restore_user_entry:function protected
global pedigree_user_gs_restore_swapgs:function hidden
global pedigree_user_gs_restore_user:function hidden
global pedigree_user_gs_restore_kernel:function hidden

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
  mov ecx, 0xc0000102
  rdmsr
  shl rdx, 32
  or rax, rdx
  mov [rdi+16], rax
%endif
%endmacro

%macro RESTORE_USER_ENTRY 1
%ifndef PEDIGREE_BENCHMARK_ABLATE_X64_USER_ENTRY_METADATA
  ; Most returns leave this state installed. Check hardware rather than
  ; thread fields: user selector loads and nested entry can change the bases.
  mov ax, ds
  cmp ax, [rdi]
  jne %%restore
  mov ax, es
  cmp ax, [rdi+2]
  jne %%restore
  mov ax, fs
  cmp ax, [rdi+4]
  jne %%restore
  mov ax, gs
  cmp ax, [rdi+6]
  jne %%restore
  mov ecx, 0xc0000100
  rdmsr
  shl rdx, 32
  or rax, rdx
  cmp rax, [rdi+8]
  jne %%restore
  mov ecx, 0xc0000102
  rdmsr
  shl rdx, 32
  or rax, rdx
  cmp rax, [rdi+16]
  je %%done

%%restore:
  mov ax, [rdi]
  mov ds, ax
  mov ax, [rdi+2]
  mov es, ax
  mov ax, [rdi+4]
  mov fs, ax
  mov rax, [rdi+8]
  mov rdx, rax
  shr rdx, 32
  mov ecx, 0xc0000100
  wrmsr
  ; Loading a selector changes the active GS base. Keep the kernel anchor
  ; inactive until the user selector and its exact base have both been loaded.
  mov ax, [rdi+6]
%if %1
pedigree_user_gs_restore_swapgs:
%endif
  swapgs
%if %1
pedigree_user_gs_restore_user:
%endif
  mov gs, ax
  mov rax, [rdi+16]
  mov rdx, rax
  shr rdx, 32
  mov ecx, 0xc0000101
  wrmsr
  swapgs
%if %1
pedigree_user_gs_restore_kernel:
%endif
  lfence
%%done:
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
; Active GS remains the local kernel anchor; inactive GS holds the user base.
pedigree_restore_user_entry:
%ifdef PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
  inc qword [rel pedigree_user_entry_restore_calls]
  test byte [rel pedigree_user_entry_restore_calls], 0xff
  jz pedigree_restore_user_entry.sampled
%endif
  RESTORE_USER_ENTRY 1
  ret

%ifdef PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
pedigree_restore_user_entry.sampled:
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
  RESTORE_USER_ENTRY 0
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

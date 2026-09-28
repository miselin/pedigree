; Latency accounting may run only with IRQs masked and kernel GS active.
%ifdef PEDIGREE_LATENCY_ACCOUNTING
extern pedigree_irq_time_state
%endif

; Preserve the surrounding assembly ABI when no public register frame exists.
; "rflags" reads the target flags at the original RSP plus the optional offset.
%macro IRQ_TIME_STATE 1-2 0
%ifdef PEDIGREE_LATENCY_ACCOUNTING
  pushfq
  push rax
  push rcx
  push rdx
  push rsi
  push rdi
  push r8
  push r9
  push r10
  push r11
  mov r11, rsp
  and rsp, -16
  sub rsp, 16
  mov [rsp], r11
%ifidni %1, rflags
  mov rdi, [r11+80+%2]
  shr edi, 9
  and edi, 1
%else
  mov edi, %1
%endif
  call pedigree_irq_time_state wrt ..plt
  mov rsp, [rsp]
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdi
  pop rsi
  pop rdx
  pop rcx
  pop rax
  popfq
%endif
%endmacro

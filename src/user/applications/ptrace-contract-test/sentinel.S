#include "sentinel-layout.h"

.text
.globl tc_probe
.type tc_probe,@function
tc_probe:
    .cfi_startproc
    pushq %rbx
    .cfi_adjust_cfa_offset 8
    .cfi_offset %rbx,-16
    pushq %rbp
    .cfi_adjust_cfa_offset 8
    .cfi_offset %rbp,-24
    pushq %r12
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r12,-32
    pushq %r13
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r13,-40
    pushq %r14
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r14,-48
    pushq %r15
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r15,-56
    subq $56,%rsp
    .cfi_adjust_cfa_offset 56
    movq %rsi,0(%rsp)
    movl %edi,8(%rsp)

    # Capture selectors after an ordinary syscall has selected its user-return CS/SS.
    movl $39,%eax
    syscall
    movq 0(%rsp),%r11
    movq %rsp,TC_STACK(%r11)
    leaq .Lresume(%rip),%rax
    movq %rax,TC_LABEL(%r11)
    movl $0,%eax
    movw %cs,%ax
    movq %rax,TC_CS(%r11)
    movw %ss,%ax
    movq %rax,TC_SS(%r11)
    movw %ds,%ax
    movq %rax,TC_DS(%r11)
    movw %es,%ax
    movq %rax,TC_ES(%r11)
    movw %fs,%ax
    movq %rax,TC_FS(%r11)
    movw %gs,%ax
    movq %rax,TC_GS(%r11)

    # The fresh pipe has ample room; a failed setup write must never enter a trace stop.
    movl $1,%eax
    movl 8(%rsp),%edi
    movq 0(%rsp),%rsi
    movl $TC_SETUP_SIZE,%edx
    syscall
    cmpq $TC_SETUP_SIZE,%rax
    jne .Lfailed

    movq 0(%rsp),%r11
    movq TC_PID(%r11),%rdi
    movq TC_TID(%r11),%rsi
    movl $10,%edx
    movabsq $TC_SENT_R15,%r15
    movabsq $TC_SENT_R14,%r14
    movabsq $TC_SENT_R13,%r13
    movabsq $TC_SENT_R12,%r12
    movabsq $TC_SENT_RBP,%rbp
    movabsq $TC_SENT_RBX,%rbx
    movabsq $TC_SENT_R10,%r10
    movabsq $TC_SENT_R9,%r9
    movabsq $TC_SENT_R8,%r8
    movl $234,%eax
    pushfq
    popq TC_ENTRY_FLAGS(%r11)
    syscall
.Lresume:
    # MOV/LEA leave flags unchanged until the actual resumed RFLAGS is captured.
    movq %rax,16(%rsp)
    movq %rcx,24(%rsp)
    movq %r11,32(%rsp)
    movq 0(%rsp),%rax
    movq %r15,TC_RESUMED+TC_R15(%rax)
    movq %r14,TC_RESUMED+TC_R14(%rax)
    movq %r13,TC_RESUMED+TC_R13(%rax)
    movq %r12,TC_RESUMED+TC_R12(%rax)
    movq %rbp,TC_RESUMED+TC_RBP(%rax)
    movq %rbx,TC_RESUMED+TC_RBX(%rax)
    movq %r10,TC_RESUMED+TC_R10(%rax)
    movq %r9,TC_RESUMED+TC_R9(%rax)
    movq %r8,TC_RESUMED+TC_R8(%rax)
    movq %rdx,TC_RESUMED+TC_RDX(%rax)
    movq %rsi,TC_RESUMED+TC_RSI(%rax)
    movq %rdi,TC_RESUMED+TC_RDI(%rax)
    movq 16(%rsp),%rcx
    movq %rcx,TC_RESUMED+TC_RAX(%rax)
    movq 24(%rsp),%rcx
    movq %rcx,TC_RESUMED+TC_RCX(%rax)
    movq 32(%rsp),%rcx
    movq %rcx,TC_RESUMED+TC_R11(%rax)
    leaq .Lresume(%rip),%rcx
    movq %rcx,TC_RESUMED+TC_RIP(%rax)
    movq %rsp,TC_RESUMED+TC_RSP(%rax)
    pushfq
    popq %rcx
    movq %rcx,TC_RESUMED+TC_EFLAGS(%rax)
    movl $0,%ecx
    movw %cs,%cx
    movq %rcx,TC_RESUMED+TC_REG_CS(%rax)
    movw %ss,%cx
    movq %rcx,TC_RESUMED+TC_REG_SS(%rax)
    movw %ds,%cx
    movq %rcx,TC_RESUMED+TC_REG_DS(%rax)
    movw %es,%cx
    movq %rcx,TC_RESUMED+TC_REG_ES(%rax)
    movw %fs,%cx
    movq %rcx,TC_RESUMED+TC_REG_FS(%rax)
    movw %gs,%cx
    movq %rcx,TC_RESUMED+TC_REG_GS(%rax)
    xorl %eax,%eax
    jmp .Lreturn
.Lfailed:
    movl $-1,%eax
.Lreturn:
    addq $56,%rsp
    .cfi_adjust_cfa_offset -56
    popq %r15
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r15
    popq %r14
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r14
    popq %r13
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r13
    popq %r12
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r12
    popq %rbp
    .cfi_adjust_cfa_offset -8
    .cfi_restore %rbp
    popq %rbx
    .cfi_adjust_cfa_offset -8
    .cfi_restore %rbx
    ret
    .cfi_endproc
.size tc_probe,.-tc_probe

.section .note.GNU-stack,"",@progbits

#include "entry-layout.h"
#include "sentinel-layout.h"

.text
.globl tc_exec_entry
.type tc_exec_entry,@function
tc_exec_entry:
    # No interpreter or CRT runs before these stores. MOV leaves entry flags intact.
    movq %r15,entry_record+TC_ENTRY_REGISTERS+TC_R15(%rip)
    movq %r14,entry_record+TC_ENTRY_REGISTERS+TC_R14(%rip)
    movq %r13,entry_record+TC_ENTRY_REGISTERS+TC_R13(%rip)
    movq %r12,entry_record+TC_ENTRY_REGISTERS+TC_R12(%rip)
    movq %rbp,entry_record+TC_ENTRY_REGISTERS+TC_RBP(%rip)
    movq %rbx,entry_record+TC_ENTRY_REGISTERS+TC_RBX(%rip)
    movq %r11,entry_record+TC_ENTRY_REGISTERS+TC_R11(%rip)
    movq %r10,entry_record+TC_ENTRY_REGISTERS+TC_R10(%rip)
    movq %r9,entry_record+TC_ENTRY_REGISTERS+TC_R9(%rip)
    movq %r8,entry_record+TC_ENTRY_REGISTERS+TC_R8(%rip)
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_RAX(%rip)
    movq %rcx,entry_record+TC_ENTRY_REGISTERS+TC_RCX(%rip)
    movq %rdx,entry_record+TC_ENTRY_REGISTERS+TC_RDX(%rip)
    movq %rsi,entry_record+TC_ENTRY_REGISTERS+TC_RSI(%rip)
    movq %rdi,entry_record+TC_ENTRY_REGISTERS+TC_RDI(%rip)
    movq %rsp,entry_record+TC_ENTRY_REGISTERS+TC_RSP(%rip)
    pushfq
    popq entry_record+TC_ENTRY_REGISTERS+TC_EFLAGS(%rip)
    leaq tc_exec_entry(%rip),%rax
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_RIP(%rip)
    movl $0,%eax
    movw %cs,%ax
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_REG_CS(%rip)
    movw %ss,%ax
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_REG_SS(%rip)
    movw %ds,%ax
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_REG_DS(%rip)
    movw %es,%ax
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_REG_ES(%rip)
    movw %fs,%ax
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_REG_FS(%rip)
    movw %gs,%ax
    movq %rax,entry_record+TC_ENTRY_REGISTERS+TC_REG_GS(%rip)
    # The unavailable slots are placeholders, never machine-state observations.
    movq $-1,entry_record+TC_ENTRY_REGISTERS+TC_ORIG_RAX(%rip)
    movq $-1,entry_record+TC_ENTRY_REGISTERS+TC_REG_GS_BASE(%rip)
    movabsq $TC_ENTRY_MAGIC_VALUE,%rax
    movq %rax,entry_record+TC_ENTRY_MAGIC(%rip)
    movl $158,%eax
    movl $0x1003,%edi
    leaq entry_record+TC_ENTRY_REGISTERS+TC_REG_FS_BASE(%rip),%rsi
    syscall
    movq %rax,entry_record+TC_ENTRY_FS_RESULT(%rip)

    # Only argv[1], an inherited owned pipe descriptor, is accepted by this helper.
    cmpq $2,(%rsp)
    jne .Lbad
    movq 16(%rsp),%rsi
    xorl %edi,%edi
    movl $11,%ecx
.Ldigit:
    movzbl (%rsi),%eax
    testb %al,%al
    jz .Lwrite
    subl $48,%eax
    cmpl $9,%eax
    ja .Lbad
    imull $10,%edi,%edi
    jo .Lbad
    addl %eax,%edi
    jo .Lbad
    incq %rsi
    decl %ecx
    jnz .Ldigit
    jmp .Lbad
.Lwrite:
    cmpl $11,%ecx
    je .Lbad
    movl $1,%eax
    leaq entry_record(%rip),%rsi
    movl $TC_ENTRY_SIZE,%edx
    syscall
    cmpq $TC_ENTRY_SIZE,%rax
    jne .Lbad
    xorl %edi,%edi
    jmp .Lexit
.Lbad:
    movl $125,%edi
.Lexit:
    movl $60,%eax
    syscall
    ud2
.size tc_exec_entry,.-tc_exec_entry

.bss
.p2align 3
entry_record:
    .zero TC_ENTRY_SIZE
.section .note.GNU-stack,"",@progbits

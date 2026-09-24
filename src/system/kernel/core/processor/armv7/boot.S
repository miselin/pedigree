/* Initial QEMU virt entry. */
.syntax unified
.arch armv7-a
.arm

.section .boot.text, "ax"
.global armv7_start
.type armv7_start, %function
armv7_start:
  cpsid if
  ldr r3, =0x50445637
  cmp r0, r3
  moveq r11, r1
  movne r11, #0

  mrc p15, 0, r0, c1, c0, 2
  orr r0, r0, #(0xf << 20)
  mcr p15, 0, r0, c1, c0, 2  /* VFP access for kernel and userspace. */
  isb
  mov r0, #0x40000000
  vmsr fpexc, r0

  ldr r0, =armv7_boot_bss_start
  ldr r1, =armv7_boot_bss_end
  mov r2, #0
1:
  cmp r0, r1
  strlo r2, [r0], #4
  blo 1b

  /* Keep early identity mappings, alias the low MMIO window at 0x80000000,
   * and alias RAM above 0xc0000000. The rest of the high half is free. */
  ldr r4, =armv7_boot_l1
  mov r5, #0
  movw r6, #4096
  movw r8, #2048
  movw r10, #2304
  movw r9, #3072
2:
  cmp r5, r8
  blo 3f
  cmp r5, r10
  blo 8f
  cmp r5, r9
  blo 4f
  sub r7, r5, r8
  b 5f
3:
  mov r7, r5
5:
  lsl r7, r7, #20
  orr r7, r7, #0x400       /* Privileged read/write, domain 0. */
  orr r7, r7, #2           /* 1 MiB section, uncached at bootstrap. */
  b 6f
8:
  sub r7, r5, r8
  lsl r7, r7, #20
  orr r7, r7, #0x400
  orr r7, r7, #0x12      /* Device memory, privileged RW, execute-never. */
  b 6f
4:
  mov r7, #0
6:
  str r7, [r4, r5, lsl #2]
  add r5, r5, #1
  cmp r5, r6
  blo 2b

  mov r0, #1
  mcr p15, 0, r0, c2, c0, 2  /* TTBR0 below 2 GiB, TTBR1 above. */
  mcr p15, 0, r4, c2, c0, 0  /* TTBR0. */
  mcr p15, 0, r4, c2, c0, 1  /* TTBR1 keeps the shared kernel mapping. */
  mov r0, #1
  mcr p15, 0, r0, c3, c0, 0  /* Domain 0 client access. */
  mov r0, #0
  mcr p15, 0, r0, c8, c7, 0  /* Invalidate unified TLB. */
  dsb sy
  isb
  mrc p15, 0, r0, c1, c0, 0
  bic r0, r0, #(1 << 2)       /* Data cache off until attributes are set. */
  bic r0, r0, #(1 << 12)      /* Instruction cache off. */
  bic r0, r0, #(1 << 13)      /* Low exception vectors. */
  orr r0, r0, #1              /* Enable MMU. */
  mcr p15, 0, r0, c1, c0, 0
  isb
  ldr r0, =armv7_high_entry
  bx r0

.section .text, "ax"
.type armv7_high_entry, %function
armv7_high_entry:
  ldr r0, =armv7_boot_stack_end
  add r0, r0, #0x80000000
  mov sp, r0

  ldr r0, =armv7_bss_start
  ldr r1, =armv7_bss_end
  mov r2, #0
9:
  cmp r0, r1
  strlo r2, [r0], #4
  blo 9b

  ldr r0, =armv7_vectors
  mcr p15, 0, r0, c12, c0, 0
  isb

  mov r0, #0xc0000000       /* QEMU ELF boot places the DTB at RAM base. */
  mov r1, #0
  cmp r11, #0
  addne r1, r11, #0x80000000
  bl armv7BootMain
7:
  wfi
  b 7b

.balign 32
.global armv7_vectors
armv7_vectors:
  b armv7_reset_exception
  b armv7_undefined_exception
  b armv7_svc_exception
  b armv7_prefetch_exception
  b armv7_data_exception
  b armv7_reserved_exception
  b armv7_irq_exception
  b armv7_fiq_exception

.macro ARMV7_EXCEPTION name, number, correction
\name:
  cpsid i
  sub lr, lr, #\correction
  srsdb sp!, #0x13
  cps #0x13
  sub sp, sp, #224
  stmia sp, {r0-r12}
  mov r0, #\number
  b armv7_exception_save
.endm

ARMV7_EXCEPTION armv7_reset_exception, 0, 0
ARMV7_EXCEPTION armv7_undefined_exception, 1, 4
ARMV7_EXCEPTION armv7_svc_exception, 2, 0
ARMV7_EXCEPTION armv7_prefetch_exception, 3, 4
ARMV7_EXCEPTION armv7_data_exception, 4, 8
ARMV7_EXCEPTION armv7_reserved_exception, 7, 0
ARMV7_EXCEPTION armv7_irq_exception, 5, 4
ARMV7_EXCEPTION armv7_fiq_exception, 6, 4

armv7_exception_save:
  str r0, [sp, #76]             /* vector */
  add r1, sp, #88
  vstmia r1, {d0-d15}
  vmrs r1, fpscr
  str r1, [sp, #216]
  ldr r1, [sp, #224]            /* return PC from SRS */
  str r1, [sp, #60]
  ldr r1, [sp, #228]            /* interrupted CPSR */
  str r1, [sp, #64]
  and r2, r1, #0x1f
  cmp r2, #0x10
  beq 1f
  cmp r2, #0x1f
  beq 1f
  add r2, sp, #232
  str r2, [sp, #52]
  str lr, [sp, #56]
  b 2f
1:
  add r2, sp, #52
  stmia r2, {sp, lr}^
2:
  cmp r0, #3
  beq 3f
  cmp r0, #4
  beq 4f
  mov r2, #0
  str r2, [sp, #68]
  str r2, [sp, #72]
  b 5f
3:
  mrc p15, 0, r2, c6, c0, 2    /* IFAR */
  str r2, [sp, #68]
  mrc p15, 0, r2, c5, c0, 1    /* IFSR */
  str r2, [sp, #72]
  b 5f
4:
  mrc p15, 0, r2, c6, c0, 0    /* DFAR */
  str r2, [sp, #68]
  mrc p15, 0, r2, c5, c0, 0    /* DFSR */
  str r2, [sp, #72]
5:
  mov r2, #0
  cmp r0, #2
  bne 7f
  ldr r1, [sp, #64]
  ldr r2, [sp, #60]
  tst r1, #0x20                /* Thumb SVC has an 8-bit immediate. */
  subne r2, r2, #2
  subeq r2, r2, #4
  ldrhne r2, [r2]
  andne r2, r2, #0xff
  ldreq r2, [r2]
  biceq r2, r2, #0xff000000
7:
  str r2, [sp, #80]
  mov r0, sp
  bl armv7HandleException

  ldr r0, [sp, #216]
  vmsr fpscr, r0
  add r0, sp, #88
  vldmia r0, {d0-d15}
  ldr r0, [sp, #60]
  str r0, [sp, #224]
  ldr r0, [sp, #64]
  str r0, [sp, #228]
  and r0, r0, #0x1f
  cmp r0, #0x10
  beq 8f
  cmp r0, #0x1f
  beq 8f
  ldr lr, [sp, #56]
  b 9f
8:
  add r0, sp, #52
  ldmia r0, {sp, lr}^
9:
  ldmia sp, {r0-r12}
  add sp, sp, #224
  rfeia sp!

#ifndef ARMV7_BOOTSTRAP
.balign 4
.global armv7SaveSchedulerState
.type armv7SaveSchedulerState, %function
armv7SaveSchedulerState:
  stmia r0, {r4-r11}
  str sp, [r0, #32]
  str lr, [r0, #36]
  add r1, r0, #40
  vstmia r1, {d0-d15}
  mov r0, #0
  bx lr

.balign 4
.global armv7RestoreSchedulerState
.type armv7RestoreSchedulerState, %function
armv7RestoreSchedulerState:
  mov r3, r0
  cmp r1, #0
  beq 1f
  mov r2, #1
  dmb ish
  str r2, [r1]
1:
  add r2, r3, #40
  vldmia r2, {d0-d15}
  ldr sp, [r3, #32]
  ldr r12, [r3, #36]
  ldmia r3, {r4-r11}
  mov r0, #1
  bx r12

.balign 4
.global armv7ReturnToUser
.type armv7ReturnToUser, %function
armv7ReturnToUser:
  mov r12, r0
  cmp r1, #0
  beq 1f
  mov r3, #1
  dmb ish
  str r3, [r1]
1:
  cmp r2, #0
  movne sp, r2
  add r3, r12, #52
  ldmia r3, {sp, lr}^
  sub sp, sp, #8
  ldr r3, [r12, #60]
  str r3, [sp]
  ldr r3, [r12, #64]
  str r3, [sp, #4]
  ldmia r12, {r0-r12}
  rfeia sp!

.balign 4
.global armv7JumpKernel
.type armv7JumpKernel, %function
armv7JumpKernel:
  mov r12, r1
  mov r5, r2
  mov r4, r0
  mov r0, r3
  ldr r1, [sp]
  ldr r2, [sp, #4]
  ldr r3, [sp, #8]
  cmp r5, #0
  movne sp, r5
  cmp r4, #0
  beq 1f
  mov r5, #1
  dmb ish
  str r5, [r4]
1:
  mov r11, #0
  ldr lr, =_ZN6Thread12threadExitedEv
  cpsie i
  bx r12

.balign 4
.global armv7JumpUser
.type armv7JumpUser, %function
armv7JumpUser:
  cpsid i
  mov r10, r0
  mov r9, r1
  mov r8, r2
  mov r4, r3
  ldr r5, [sp]
  ldr r6, [sp, #4]
  ldr r7, [sp, #8]
  ldr r11, [sp, #12]
  cps #0x1f
  mov sp, r8
  mov lr, #0
  cps #0x13
  cmp r11, #0
  movne sp, r11
  sub sp, sp, #8
  tst r9, #1
  movne r0, #0x30
  moveq r0, #0x10
  bic r9, r9, #1
  str r9, [sp]
  str r0, [sp, #4]
  cmp r10, #0
  beq 1f
  mov r0, #1
  dmb ish
  str r0, [r10]
1:
  mov r0, r4
  mov r1, r5
  mov r2, r6
  mov r3, r7
  mov r4, #0
  mov r5, #0
  mov r6, #0
  mov r7, #0
  mov r8, #0
  mov r9, #0
  mov r10, #0
  mov r11, #0
  mov r12, #0
  rfeia sp!

.balign 4
.global _ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR19Armv7SchedulerStatePVj
.type _ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR19Armv7SchedulerStatePVj, %function
_ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR19Armv7SchedulerStatePVj:
  mov r4, r1
  cmp r2, #0
  beq 1f
  mov r3, #1
  dmb ish
  str r3, [r2]
1:
  ldr sp, [r4, #32]
  bl _ZN21PerProcessorScheduler12deleteThreadEP6Thread
  mov r0, r4
  mov r1, #0
  b armv7RestoreSchedulerState
#endif

.section .boot.bss, "aw", %nobits
.balign 16384
.global armv7_boot_l1
armv7_boot_l1:
  .skip 16384
.balign 16
armv7_boot_stack:
  .skip 16384
armv7_boot_stack_end:

.section .note.GNU-stack, "", %progbits

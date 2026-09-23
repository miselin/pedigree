// QEMU virt ELF entry. The loader places the DTB at the start of RAM.
.section .boot.text, "ax"
.align 4
.global arm64_start
.type arm64_start, %function
arm64_start:
  // The UEFI loader passes an explicit FDT, initrd and command line. Direct
  // QEMU ELF boot continues to use the FDT at the start of RAM.
  mov x19, x0
  mov x20, x1
  mov x21, x2
  mov x22, x3
  mov x23, x4
  mov x24, x5
  mov x25, x6
  mov x26, x7
  msr daifset, #0xf
  mrs x1, CurrentEL
  cmp x1, #0x8
  b.ne 1f

  mov x1, #(1 << 31)              // EL1 executes AArch64.
  msr hcr_el2, x1
  msr cptr_el2, xzr               // Keep FP/SIMD accessible at EL1.
  mrs x1, icc_sre_el2
  mov x2, #9
  orr x1, x1, x2                 // EL1 GICv3 system-register interface.
  msr icc_sre_el2, x1
  isb
  mrs x1, cnthctl_el2
  orr x1, x1, #3                 // EL1 may use physical and virtual timers.
  msr cnthctl_el2, x1
  msr cntvoff_el2, xzr
  adr x1, 1f
  msr elr_el2, x1
  mov x1, #0x3c5                 // EL1h, exceptions masked.
  msr spsr_el2, x1
  eret

1:
  mov x1, #(3 << 20)
  msr cpacr_el1, x1               // EL1 and EL0 may use FP/SIMD.
  isb
  // These objects are linked and loaded at the same low address.
  adrp x0, arm64_boot_bss_start
  add x0, x0, :lo12:arm64_boot_bss_start
  adrp x1, arm64_boot_bss_end
  add x1, x1, :lo12:arm64_boot_bss_end
2:
  cmp x0, x1
  b.hs 3f
  stp xzr, xzr, [x0], #16
  b 2b

3:
  adrp x0, arm64_boot_stack_end
  add x0, x0, :lo12:arm64_boot_stack_end
  mov sp, x0

  // A 48-bit TTBR0 identity map and TTBR1 direct map share the RAM blocks.
  adrp x0, arm64_boot_low_l0
  add x0, x0, :lo12:arm64_boot_low_l0
  adrp x1, arm64_boot_low_l1
  add x1, x1, :lo12:arm64_boot_low_l1
  orr x2, x1, #3
  str x2, [x0]
  adrp x0, arm64_boot_high_l0
  add x0, x0, :lo12:arm64_boot_high_l0
  adrp x2, arm64_boot_high_l1
  add x2, x2, :lo12:arm64_boot_high_l1
  orr x3, x2, #3
  str x3, [x0]

  // The first GiB contains QEMU virt MMIO. These addresses are device memory.
  movz x3, #0x401
  movz x4, #0x60, lsl #48       // PXN and UXN.
  orr x3, x3, x4
  str x3, [x1]
  str x3, [x2]

  // Map 1..64 GiB as normal memory. RAM starts at 1 GiB on QEMU virt.
  mov x3, #1
  mov x4, #0x705               // AttrIdx=1, inner-shareable, AF, valid block.
4:
  lsl x5, x3, #30
  orr x5, x5, x4
  str x5, [x1, x3, lsl #3]
  str x5, [x2, x3, lsl #3]
  add x3, x3, #1
  cmp x3, #64
  b.lo 4b

  adrp x0, arm64_boot_low_l0
  add x0, x0, :lo12:arm64_boot_low_l0
  msr ttbr0_el1, x0
  adrp x0, arm64_boot_high_l0
  add x0, x0, :lo12:arm64_boot_high_l0
  msr ttbr1_el1, x0
  mov x0, #0xff00
  msr mair_el1, x0

  // T0SZ=T1SZ=16, 4 KiB granules, WBWA table walks, 40-bit PA.
  ldr x0, =0x00000002b5103510
  msr tcr_el1, x0
  dsb sy
  isb
  mrs x0, sctlr_el1
  mov x1, #0x1005              // M, C and I.
  orr x0, x0, x1
  msr sctlr_el1, x0
  isb

  // Switch both PC and SP to their TTBR1 aliases before C++.
  ldr x0, =arm64_high_entry
  br x0

.section .text, "ax"
.align 4
arm64_high_entry:
  ldr x0, =arm64_boot_stack_end
  movz x1, #0xffff, lsl #48
  add x0, x0, x1
  mov sp, x0

  ldr x0, =arm64_bss_start
  ldr x1, =arm64_bss_end
5:
  cmp x0, x1
  b.hs 6f
  stp xzr, xzr, [x0], #16
  b 5b

6:
  ldr x0, =arm64_vectors
  msr vbar_el1, x0
  isb
  ldr x0, =0x5045444947524545
  cmp x23, x0
  b.ne 8f
  mov x0, x19
  mov x1, x20
  mov x2, x21
  mov x3, x22
  mov x4, x24
  mov x5, x25
  mov x6, x26
  b 9f
8:
  movz x0, #0x4000, lsl #16   // ELF -kernel DTB address: RAM base 0x40000000.
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  mov x4, xzr
  mov x5, xzr
  mov x6, xzr
9:
  ldr x8, =arm64BootMain
  blr x8
7:
  wfe
  b 7b

.section .boot.bss, "aw", %nobits
.balign 4096
.global arm64_boot_low_l0
arm64_boot_low_l0: .skip 4096
.global arm64_boot_low_l1
arm64_boot_low_l1: .skip 4096
.global arm64_boot_high_l0
arm64_boot_high_l0: .skip 4096
.global arm64_boot_high_l1
arm64_boot_high_l1: .skip 4096
.balign 16
arm64_boot_stack: .skip 65536
.global arm64_boot_stack_end
arm64_boot_stack_end:

// ARM64 exception vectors: each slot occupies exactly 128 bytes.
.section .text, "ax"
.balign 2048
.global arm64_vectors
arm64_vectors:
.macro VECTOR number
  sub sp, sp, #304
  stp x0, x1, [sp]
  mov x0, #\number
  b arm64_exception_save
  .balign 128
.endm
VECTOR 0
VECTOR 1
VECTOR 2
VECTOR 3
VECTOR 4
VECTOR 5
VECTOR 6
VECTOR 7
VECTOR 8
VECTOR 9
VECTOR 10
VECTOR 11
VECTOR 12
VECTOR 13
VECTOR 14
VECTOR 15

arm64_exception_save:
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x19, [sp, #144]
  stp x20, x21, [sp, #160]
  stp x22, x23, [sp, #176]
  stp x24, x25, [sp, #192]
  stp x26, x27, [sp, #208]
  stp x28, x29, [sp, #224]
  str x30, [sp, #240]
  cmp x0, #8
  b.lo 8f
  mrs x1, sp_el0
  b 9f
8:
  add x1, sp, #304
9:
  str x1, [sp, #248]
  mrs x1, elr_el1
  str x1, [sp, #256]
  mrs x1, spsr_el1
  str x1, [sp, #264]
  mrs x1, esr_el1
  str x1, [sp, #272]
  mrs x1, far_el1
  str x1, [sp, #280]
  str x0, [sp, #288]
  mov x0, sp
  bl arm64HandleException

  ldr x0, [sp, #256]
  msr elr_el1, x0
  ldr x0, [sp, #264]
  msr spsr_el1, x0
  ldr x0, [sp, #288]
  cmp x0, #8
  b.lo 10f
  ldr x0, [sp, #248]
  msr sp_el0, x0
10:
  ldp x0, x1, [sp]
  ldp x2, x3, [sp, #16]
  ldp x4, x5, [sp, #32]
  ldp x6, x7, [sp, #48]
  ldp x8, x9, [sp, #64]
  ldp x10, x11, [sp, #80]
  ldp x12, x13, [sp, #96]
  ldp x14, x15, [sp, #112]
  ldp x16, x17, [sp, #128]
  ldp x18, x19, [sp, #144]
  ldp x20, x21, [sp, #160]
  ldp x22, x23, [sp, #176]
  ldp x24, x25, [sp, #192]
  ldp x26, x27, [sp, #208]
  ldp x28, x29, [sp, #224]
  ldr x30, [sp, #240]
  add sp, sp, #304
  eret

.balign 16
.global arm64SaveSchedulerState
.type arm64SaveSchedulerState, %function
arm64SaveSchedulerState:
  stp x19, x20, [x0, #0]
  stp x21, x22, [x0, #16]
  stp x23, x24, [x0, #32]
  stp x25, x26, [x0, #48]
  stp x27, x28, [x0, #64]
  stp x29, x30, [x0, #80]
  mov x1, sp
  str x1, [x0, #96]
  adr x1, 1f
  str x1, [x0, #104]
  mrs x1, fpcr
  str x1, [x0, #112]
  mrs x1, fpsr
  str x1, [x0, #120]
  stp q0, q1, [x0, #128]
  stp q2, q3, [x0, #160]
  stp q4, q5, [x0, #192]
  stp q6, q7, [x0, #224]
  stp q8, q9, [x0, #256]
  stp q10, q11, [x0, #288]
  stp q12, q13, [x0, #320]
  stp q14, q15, [x0, #352]
  stp q16, q17, [x0, #384]
  stp q18, q19, [x0, #416]
  stp q20, q21, [x0, #448]
  stp q22, q23, [x0, #480]
  stp q24, q25, [x0, #512]
  stp q26, q27, [x0, #544]
  stp q28, q29, [x0, #576]
  stp q30, q31, [x0, #608]
  mov w0, #0
  ret
1:
  mov w0, #1
  ret

.balign 16
.global arm64RestoreSchedulerState
.type arm64RestoreSchedulerState, %function
arm64RestoreSchedulerState:
  mov x9, x0
  cbz x1, 1f
  mov x10, #1
  stlr x10, [x1]
1:
  ldr x16, [x9, #104]
  ldr x17, [x9, #96]
  ldr x18, [x9, #112]
  msr fpcr, x18
  ldr x18, [x9, #120]
  msr fpsr, x18
  ldp q0, q1, [x9, #128]
  ldp q2, q3, [x9, #160]
  ldp q4, q5, [x9, #192]
  ldp q6, q7, [x9, #224]
  ldp q8, q9, [x9, #256]
  ldp q10, q11, [x9, #288]
  ldp q12, q13, [x9, #320]
  ldp q14, q15, [x9, #352]
  ldp q16, q17, [x9, #384]
  ldp q18, q19, [x9, #416]
  ldp q20, q21, [x9, #448]
  ldp q22, q23, [x9, #480]
  ldp q24, q25, [x9, #512]
  ldp q26, q27, [x9, #544]
  ldp q28, q29, [x9, #576]
  ldp q30, q31, [x9, #608]
  ldp x19, x20, [x9, #0]
  ldp x21, x22, [x9, #16]
  ldp x23, x24, [x9, #32]
  ldp x25, x26, [x9, #48]
  ldp x27, x28, [x9, #64]
  ldp x29, x30, [x9, #80]
  mov sp, x17
  br x16

.balign 16
.global arm64ReturnToUser
.type arm64ReturnToUser, %function
arm64ReturnToUser:
  mov x30, x0
  ldr x9, [x30, #248]
  msr sp_el0, x9
  ldr x9, [x30, #256]
  msr elr_el1, x9
  ldr x9, [x30, #264]
  msr spsr_el1, x9
  cbz x1, 1f
  mov x9, #1
  stlr x9, [x1]
1:
  cbz x2, 2f
  mov sp, x2
2:
  ldp x0, x1, [x30, #0]
  ldp x2, x3, [x30, #16]
  ldp x4, x5, [x30, #32]
  ldp x6, x7, [x30, #48]
  ldp x8, x9, [x30, #64]
  ldp x10, x11, [x30, #80]
  ldp x12, x13, [x30, #96]
  ldp x14, x15, [x30, #112]
  ldp x16, x17, [x30, #128]
  ldp x18, x19, [x30, #144]
  ldp x20, x21, [x30, #160]
  ldp x22, x23, [x30, #176]
  ldp x24, x25, [x30, #192]
  ldp x26, x27, [x30, #208]
  ldp x28, x29, [x30, #224]
  ldr x30, [x30, #240]
  eret

.balign 16
.global arm64JumpKernel
.type arm64JumpKernel, %function
arm64JumpKernel:
  mov x9, x0
  mov x16, x1
  cbz x2, 1f
  mov sp, x2
1:
  mov x0, x3
  mov x1, x4
  mov x2, x5
  mov x3, x6
  cbz x9, 2f
  mov x10, #1
  stlr x10, [x9]
2:
  mov x29, xzr
  ldr x30, =_ZN6Thread12threadExitedEv
  msr daifclr, #2
  br x16

.balign 16
.global arm64JumpUser
.type arm64JumpUser, %function
arm64JumpUser:
  mov x9, x0
  msr elr_el1, x1
  msr sp_el0, x2
  msr spsr_el1, xzr          // EL0t, interrupts enabled.
  mov x0, x3
  mov x1, x4
  mov x2, x5
  mov x3, x6
  cbz x9, 1f
  mov x10, #1
  stlr x10, [x9]
1:
  cbz x7, 2f
  mov sp, x7
2:
  eret

.balign 16
.global _ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR19Arm64SchedulerStatePVm
.type _ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR19Arm64SchedulerStatePVm, %function
_ZN21PerProcessorScheduler28deleteThreadThenRestoreStateEP6ThreadR19Arm64SchedulerStatePVm:
  mov x19, x1                    // The replacement state survives deleteThread.
  cbz x2, 1f
  mov x9, #1
  stlr x9, [x2]
1:
  ldr x9, [x19, #96]
  mov sp, x9                    // The old Thread may free its own stack.
  bl _ZN21PerProcessorScheduler12deleteThreadEP6Thread
  mov x0, x19
  mov x1, xzr
  b arm64RestoreSchedulerState

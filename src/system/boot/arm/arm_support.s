# Copyright (c) 2008-2014, Pedigree Developers
# 
# Please see the CONTRIB file in the root of the source tree for a full
# list of contributors.
# 
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
# 
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

.extern __start
.global start
.type start, %function
.type __start, %function

.section .init, "x"
start:
    # Disable IRQs and FIQs.
    mrs r0, cpsr
    orr r1, r0, #0xC0
    msr cpsr_c, r1

    # Read the CPSR, mask out the lower 6 bits, and enter FIQ mode
    mrs r0, cpsr
    bic r0, r0, #0x3F
    orr r1, r0, #0x11
    msr cpsr_c, r1

    ldr sp, =stack_fiq + 0x1000
    
    # Read the CPSR, mask out the lower 6 bits, and enter IRQ mode
    mrs r0, cpsr
    bic r0, r0, #0x3F
    orr r1, r0, #0x12
    msr cpsr_c, r1
    
    ldr sp, =stack_irq + 0x1000
    
    # Concept from lk (geist) - a location to dump stuff during IRQ handling
    ldr r13, =irq_save
    
    # Read the CPSR, mask out the lower 6 bits, and enter SVC mode
    mrs r0, cpsr
    bic r0, r0, #0x3F
    orr r1, r0, #0x13
    msr cpsr_c, r1
    
    ldr sp, =stack_svc + 0x10000

    bl __start

.section .stacks, "Mw", @nobits
.comm stack_svc, 0x10000
.comm stack_irq, 0x1000
.comm stack_fiq, 0x1000
.comm irq_save, 0x10

.syntax unified
.arm
.text

.global sigret_stub
.global sigret_stub_end
.type sigret_stub, %function
sigret_stub:
    mov r4, r0
    mov r0, #0
    cmp r1, #0
    ldrbne r0, [r1]
    blx r4
    mov r7, #173
    svc #0
1:
    b 1b
sigret_stub_end:

.global pthread_stub
.global pthread_stub_end
.type pthread_stub, %function
pthread_stub:
    mov r4, r0
    mov r0, r1
    blx r4
    mov r7, #1
    svc #0
2:
    b 2b
pthread_stub_end:

.section .note.GNU-stack, "", %progbits

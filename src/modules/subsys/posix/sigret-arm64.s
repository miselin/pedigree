.text

.global sigret_stub
.global sigret_stub_end
.type sigret_stub, %function
sigret_stub:
    mov x9, x0
    mov x2, x1
    mov x1, x9
    mov w0, wzr
    cbz x2, 1f
    ldrb w0, [x2]
1:
    blr x9
    mov x8, #139
    svc #0
2:
    b 2b
sigret_stub_end:

.global pthread_stub
.global pthread_stub_end
.type pthread_stub, %function
pthread_stub:
    mov x9, x0
    mov x0, x1
    blr x9
    mov x8, #93
    svc #0
3:
    b 3b
pthread_stub_end:

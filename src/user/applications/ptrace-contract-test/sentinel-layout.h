#ifndef PTRACE_SENTINEL_LAYOUT_H
#define PTRACE_SENTINEL_LAYOUT_H

#define TC_PID 0
#define TC_TID 8
#define TC_FS_BASE 16
#define TC_STACK 24
#define TC_LABEL 32
#define TC_CS 40
#define TC_SS 48
#define TC_DS 56
#define TC_ES 64
#define TC_FS 72
#define TC_GS 80
#define TC_SETUP_SIZE 88
#define TC_ENTRY_FLAGS 88
#define TC_RESUMED 96
#define TC_CAPTURE_SIZE 312

#define TC_R15 0
#define TC_R14 8
#define TC_R13 16
#define TC_R12 24
#define TC_RBP 32
#define TC_RBX 40
#define TC_R11 48
#define TC_R10 56
#define TC_R9 64
#define TC_R8 72
#define TC_RAX 80
#define TC_RCX 88
#define TC_RDX 96
#define TC_RSI 104
#define TC_RDI 112
#define TC_ORIG_RAX 120
#define TC_RIP 128
#define TC_REG_CS 136
#define TC_EFLAGS 144
#define TC_RSP 152
#define TC_REG_SS 160
#define TC_REG_FS_BASE 168
#define TC_REG_GS_BASE 176
#define TC_REG_DS 184
#define TC_REG_ES 192
#define TC_REG_FS 200
#define TC_REG_GS 208
#define TC_REG_SIZE 216

#define TC_SENT_R15 0x1515151515151515
#define TC_SENT_R14 0x1414141414141414
#define TC_SENT_R13 0x1313131313131313
#define TC_SENT_R12 0x1212121212121212
#define TC_SENT_RBP 0x0b0b0b0b0b0b0b0b
#define TC_SENT_RBX 0x0c0c0c0c0c0c0c0c
#define TC_SENT_R10 0x1010101010101010
#define TC_SENT_R9 0x0909090909090909
#define TC_SENT_R8 0x0808080808080808

#endif

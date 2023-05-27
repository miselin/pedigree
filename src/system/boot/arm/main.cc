/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Elf32.h"

#include "autogen.h"

#define LOAD_ADDR 0x00100000
extern int ByteSet(void *buf, int c, size_t len);
struct BootstrapStruct_t
{
    // If we are passed via grub, this information will be completely different
    // to via the bootstrapper.
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;

    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    /* ELF information */
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;

    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;

    uint32_t config_table;

    uint32_t boot_loader_name;

    uint32_t apm_table;

    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint32_t vbe_mode;
    uint32_t vbe_interface_seg;
    uint32_t vbe_interface_off;
    uint32_t vbe_interface_len;
} __attribute__((packed));

inline void writeChar(char c)
{
#if defined(ARM_VERSATILE)
    volatile char *p = reinterpret_cast<volatile char *>(0x101f1000);
#elif defined(ARM_INTEGRATOR)
    volatile char *p = reinterpret_cast<volatile char *>(0x16000000);
#else
#error No valid ARM board!
#endif
    *p = c;
    asm volatile("" ::: "memory");
#if !SERIAL_IS_FILE
    *p = 0;
    asm volatile("" ::: "memory");
#endif
}

inline void writeStr(const char *str)
{
    char c;
    while ((c = *str++))
        writeChar(c);
}

void writeHex(unsigned int n)
{
    bool noZeroes = true;

    int i;
    unsigned int tmp;
    for (i = 28; i > 0; i -= 4)
    {
        tmp = (n >> i) & 0xF;
        if (tmp == 0 && noZeroes)
        {
            continue;
        }

        if (tmp >= 0xA)
        {
            noZeroes = false;
            writeChar(tmp - 0xA + 'a');
        }
        else
        {
            noZeroes = false;
            writeChar(tmp + '0');
        }
    }

    tmp = n & 0xF;
    if (tmp >= 0xA)
    {
        writeChar(tmp - 0xA + 'a');
    }
    else
    {
        writeChar(tmp + '0');
    }
}

extern "C" void __start();
extern "C" void start()
{
    // setup stack space (put the top of the stack at the bottom of this binary)
    // and jump to the C++ entry
    asm volatile("mov sp,$0x10000; mov ip, sp; b __start");
    for (;;)
        ;
}

extern "C" void arm_swint_handler()
{
    // what was the interrupt number?
    uint32_t intnum = 1;  //*((uint32_t*) (linkreg-4));
    asm volatile("mov %0, lr" : "=r"(intnum));

    writeStr("Link register: ");
    writeHex(intnum);
    writeHex(0xdeadbeef);
    writeChar(':');

    writeStr("\r\n");

    writeStr("softint\r\n");

    // do something
    switch (intnum)
    {
        case 0x1:
            writeStr("SWI01\r\n");
            break;
    }

    // asm volatile( "mov pc,r14_svc" );
    // while( 1 );
}

extern "C" void arm_instundef_handler()
{
    writeStr("undefined instruction!\n");
    while (1)
        ;
}

extern "C" void arm_fiq_handler()
{
    writeStr("fiq\r\n");
    while (1)
        ;
}

extern "C" void arm_irq_handler()
{
    writeStr("irq\r\n");
    while (1)
        ;
}

extern "C" void arm_reset_handler()
{
    writeStr("reset\r\n");
    while (1)
        ;
}

extern "C" void __arm_vector_table();

uint32_t arm_get_cpsr()
{
    uint32_t ret = 0;
    asm volatile("msr cpsr,r0" : "=r"(ret));
    return ret;
}

void MemoryCopy(void *dest, const void *src, size_t len);

extern "C" void __start()
{
    // 8 entries in the table, plus the literal table holding offsets of C
    // handlers
    MemoryCopy((void *) 0, (void *) __arm_vector_table, (4 * 8) + (4 * 6));

    // TODO: remove this when happy with relevant code
    writeStr("about to do software interrupt\r\n");
    asm volatile("swi #1");
    writeStr("swi done and returned\r\n");

    writeStr("Loading kernel...\r\n");

    Elf32 elf("kernel");
    elf.load((uint8_t *) file, 0);
    elf.writeSections();
    int (*main)(struct BootstrapStruct_t *) =
        (int (*)(struct BootstrapStruct_t *)) elf.getEntryPoint();

    struct BootstrapStruct_t bs;

    ByteSet(&bs, 0, sizeof(bs));
    bs.shndx = elf.m_pHeader->shstrndx;
    bs.num = elf.m_pHeader->shnum;
    bs.size = elf.m_pHeader->shentsize;
    bs.addr = (unsigned int) elf.m_pSectionHeaders;

    // For every section header, set .addr = .offset + m_pBuffer.
    for (int i = 0; i < elf.m_pHeader->shnum; i++)
    {
        elf.m_pSectionHeaders[i].addr =
            elf.m_pSectionHeaders[i].offset + (uint32_t) elf.m_pBuffer;
    }

    writeStr("Kernel loaded, jumping to entry point...\r\n");

    main(&bs);

    writeStr("Kernel returned\r\n");

    while (1)
        ;
}

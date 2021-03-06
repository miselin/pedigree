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

#define LOAD_ADDR 0x80200000
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

void writeChar(char c)
{
    unsigned int *p = reinterpret_cast<unsigned int *>(0x91100004);
    *p = static_cast<unsigned int>(c);
}

void writeStr(const char *str)
{
    char c;
    while ((c = *str++))
        writeChar(c);
}
extern "C" int
__start(char argc, char **argv, char **env, unsigned int ramsize);
extern "C" int start()
{
    asm volatile("li $sp, 0x800F0000");
    // Disable interrupts.
    asm volatile("mfc0 $t0, $12");         // get SR
    asm volatile("addi $t1, $zero, 0x1");  // Set $t1 = 1
    asm volatile("and $t0, $t0, $t1");     // $t0 = $t0 & 0x1
    asm volatile("mtc0 $t0, $12");         // set SR.
    asm volatile("j __start");
}

extern "C" int __start(char argc, char **argv, char **env, unsigned int ramsize)
{
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

    bs.mem_upper = ramsize;

    // For every section header, set .addr = .offset + m_pBuffer.
    for (int i = 0; i < elf.m_pHeader->shnum; i++)
    {
        elf.m_pSectionHeaders[i].addr =
            elf.m_pSectionHeaders[i].offset + (uint32_t) elf.m_pBuffer;
    }

    int a = main(&bs);
    return a;
}

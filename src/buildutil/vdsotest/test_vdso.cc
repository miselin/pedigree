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

#include <iostream>

#include <modules/subsys/posix/vdso.h>

extern "C" void vdso_init_from_auxv(void *auxv);
extern "C" void vdso_init_from_sysinfo_ehdr(uintptr_t base);
extern "C" void *vdso_sym(const char *version, const char *name);

int main(int argc, char *argv[])
{
    vdso_init_from_sysinfo_ehdr(reinterpret_cast<uintptr_t>(__vdso_so));

    void *gtod = vdso_sym("LINUX_2.6", "gettimeofday");
    void *vdso_gtod = vdso_sym("LINUX_2.6", "__vdso_gettimeofday");

    bool ok = true;

    if (gtod == nullptr)
    {
        std::cerr << "Could not successfully look up gettimeofday()" << std::endl;
        ok = false;
    }
    if (vdso_gtod == nullptr)
    {
        std::cerr << "Could not successfully look up __vdso_gettimeofday()" << std::endl;
        ok = false;
    }

    if (ok)
    {
        std::cout << "OK!" << std::endl;
    }

    return 0;
}

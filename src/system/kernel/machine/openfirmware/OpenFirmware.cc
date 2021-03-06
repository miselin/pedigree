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

#include "pedigree/kernel/machine/openfirmware/OpenFirmware.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/openfirmware/Device.h"

OpenFirmware OpenFirmware::m_Instance;

OpenFirmware::OpenFirmware() : m_Interface(0)
{
}

OpenFirmware::~OpenFirmware()
{
}

void OpenFirmware::initialise(OFInterface interface)
{
    m_Interface = interface;

    // Grab the current state of the SPRG registers.
    asm volatile("mfsprg0 %0" : "=r"(m_Sprg0));
    asm volatile("mfsprg1 %0" : "=r"(m_Sprg1));
    asm volatile("mfsprg2 %0" : "=r"(m_Sprg2));
    asm volatile("mfsprg3 %0" : "=r"(m_Sprg3));
}

OFHandle OpenFirmware::findDevice(const char *pName)
{
    return static_cast<OFHandle>(call(
        "finddevice", 1, reinterpret_cast<OFParam>(const_cast<char *>(pName))));
}

OFHandle OpenFirmware::getSibling(OFDevice *pDev)
{
    return static_cast<OFHandle>(
        call("peer", 1, static_cast<OFHandle>(pDev->m_Handle)));
}

OFHandle OpenFirmware::getFirstChild(OFDevice *pDev)
{
    return static_cast<OFHandle>(
        call("child", 1, static_cast<OFHandle>(pDev->m_Handle)));
}

OFParam OpenFirmware::call(
    const char *pService, int nArgs, OFParam p1, OFParam p2, OFParam p3,
    OFParam p4, OFParam p5, OFParam p6, OFParam p7, OFParam p8)
{
    static Spinlock lock;
    LockGuard<Spinlock> guard(lock);  // Grab a spinlock so that only one thing
                                      // can call the prom at once.

    // Revert the state of the SPRG registers to how OF left them.
    // These get mashed during our interrupt handlers, but OF needs them left
    // alone (apparently)!
    asm volatile("mtsprg0 %0" : : "r"(m_Sprg0));
    asm volatile("mtsprg1 %0" : : "r"(m_Sprg1));
    asm volatile("mtsprg2 %0" : : "r"(m_Sprg2));
    asm volatile("mtsprg3 %0" : : "r"(m_Sprg3));

    PromArgs pa;
    pa.service = pService;

    pa.args[0] = p1;
    pa.args[1] = p2;
    pa.args[2] = p3;
    pa.args[3] = p4;
    pa.args[4] = p5;
    pa.args[5] = p6;
    pa.args[6] = p7;
    pa.args[7] = p8;
    pa.args[8] = 0;
    pa.args[9] = 0;
    pa.nargs = nArgs;
    pa.nret = 1;

    if (m_Interface(&pa) < 0)
        return reinterpret_cast<OFParam>(-1);

    return pa.args[pa.nargs];
}

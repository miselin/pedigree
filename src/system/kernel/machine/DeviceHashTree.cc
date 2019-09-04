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

#include "pedigree/kernel/machine/DeviceHashTree.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/sha1/sha1.h"
#include "pedigree/kernel/utilities/utility.h"

DeviceHashTree DeviceHashTree::m_Instance;

DeviceHashTree::DeviceHashTree() : m_bInitialised(false), m_DeviceTree()
{
}

DeviceHashTree::~DeviceHashTree()
{
}

static Device *testDevice(Device *p)
{
    if (p->getType() != Device::Root)
        DeviceHashTree::instance().add(p);

    return p;
}

void DeviceHashTree::fill(Device *root)
{
    Device::foreach (testDevice, root);

    m_bInitialised = true;
}

void DeviceHashTree::add(Device *p)
{
    size_t hash = getHash(p);
    if (m_DeviceTree.lookup(hash))
        return;

    String dump;
    p->dump(dump);

    NOTICE("Device hash for `" << dump << "' is: " << hash << ".");
    m_DeviceTree.insert(hash, p);
}

Device *DeviceHashTree::getDevice(uint32_t hash)
{
    if (!m_bInitialised)
        return 0;
    else
        return m_DeviceTree.lookup(hash);
}

Device *DeviceHashTree::getDevice(const String &hash)
{
    if (!m_bInitialised)
        return 0;
    else
    {
        uint32_t inthash =
            StringToUnsignedLong(static_cast<const char *>(hash), 0, 16);
        return m_DeviceTree.lookup(inthash);
    }
}

size_t DeviceHashTree::getHash(Device *pChild)
{
    static SHA1 mySha1;

    // Grab the device information
    String name, dump;
    pChild->getName(name);
    pChild->dump(dump);
    uint32_t bus = pChild->getPciBusPosition();
    uint32_t dev = pChild->getPciDevicePosition();
    uint32_t func = pChild->getPciFunctionNumber();

    TinyStaticString busStr, devStr, funcStr;
    busStr.append(bus);
    devStr.append(dev);
    funcStr.append(func);

    // Build the string to be hashed
    Cord hashBuild;
    hashBuild.append(name);
    hashBuild.append("-");
    hashBuild.append(dump);
    hashBuild.append("-");
    hashBuild.append(busStr);
    hashBuild.append(".");
    hashBuild.append(devStr);
    hashBuild.append(".");
    hashBuild.append(funcStr);

    // Hash the string
    mySha1.Reset();
    for (auto it = hashBuild.segbegin(); it != hashBuild.segend(); ++it)
    {
        mySha1.Input(it.ptr(), it.length());
    }
    unsigned int digest[5];
    mySha1.Result(digest);

    // Only use 4 bytes of the hash
    return digest[0];
}

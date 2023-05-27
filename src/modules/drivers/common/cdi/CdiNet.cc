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

#include "CdiNet.h"
#include "modules/system/network-stack/NetworkStack.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/network/IpAddress.h"
#include "pedigree/kernel/network/MacAddress.h"
#include "pedigree/kernel/utilities/utility.h"

// Prototypes in the extern "C" block to ensure that they are not mangled
extern "C" {
    void cdi_cpp_net_register(void* void_pdev, struct cdi_net_device* device);
    void cdi_net_receive(struct cdi_net_device* device, void* buffer, size_t size);
    struct cdi_net_device* cdi_net_get_device(int num);
};

CdiNet::CdiNet(Network* pDev, struct cdi_net_device* device) :
    Network(pDev), m_Device(device)
{
    setSpecificType(String("CDI NIC"));

    /// \todo Check endianness - *should* be fine, but we'll see...
    uint64_t mac = m_Device->mac;
    m_StationInfo.mac.setMac(reinterpret_cast<uint16_t*>(&mac), false);

    NetworkStack::instance().registerDevice(this);
}

CdiNet::CdiNet(struct cdi_net_device* device) :
    Network(), m_Device(device)
{
    setSpecificType(String("CDI NIC"));

    /// \todo Check endianness - *should* be fine, but we'll see...
    uint64_t mac = m_Device->mac;
    m_StationInfo.mac.setMac(reinterpret_cast<uint16_t*>(&mac), false);

    NetworkStack::instance().registerDevice(this);
}

CdiNet::~CdiNet()
{
}

bool CdiNet::send(size_t nBytes, uintptr_t buffer)
{
    struct cdi_net_driver *driver = reinterpret_cast<struct cdi_net_driver *>(m_Device->dev.driver);
    if(driver)
    {
        driver->send_packet(m_Device, reinterpret_cast<void*>(buffer), nBytes);
        return true;
    }
    return false;
}

const StationInfo &CdiNet::getStationInfo()
{
    return m_StationInfo;
}

bool CdiNet::setStationInfo(const StationInfo &info)
{
    // free the old DNS servers list, if there is one
    if (m_StationInfo.dnsServers)
        delete [] m_StationInfo.dnsServers;

    // MAC isn't changeable, so set it all manually
    m_StationInfo.ipv4 = info.ipv4;
    NOTICE("cdi-net: Setting ipv4, " << info.ipv4.toString() << ", " << m_StationInfo.ipv4.toString() << "...");

    m_StationInfo.ipv6 = info.ipv6;
    m_StationInfo.nIpv6Addresses = info.nIpv6Addresses;
    NOTICE("cdi-net: Assigning " << Dec << info.nIpv6Addresses << Hex << " IPv6 addresses.");

    m_StationInfo.subnetMask = info.subnetMask;
    NOTICE("cdi-net: Setting subnet mask, " << info.subnetMask.toString() << ", " << m_StationInfo.subnetMask.toString() << "...");
    m_StationInfo.gateway = info.gateway;
    NOTICE("cdi-net: Setting gateway, " << info.gateway.toString() << ", " << m_StationInfo.gateway.toString() << "...");

    // Callers do not free their dnsServers memory
    m_StationInfo.dnsServers = info.dnsServers;
    m_StationInfo.nDnsServers = info.nDnsServers;
    NOTICE("cdi-net: Setting DNS servers [" << Dec << m_StationInfo.nDnsServers << Hex << " servers being set]...");

    return true;
}

EXPORTED_PUBLIC void cdi_cpp_net_register(void* void_pdev, struct cdi_net_device* device)
{
    // Create a new CdiNet node
    CdiNet *pCdiNet = new CdiNet(device);

    // Replace pDev with pCdiNet
    Device::addToRoot(pCdiNet);
}

/**
 * Wird von Netzwerktreibern aufgerufen, wenn ein Netzwerkpaket
 * empfangen wurde.
 */
EXPORTED_PUBLIC void cdi_net_receive(struct cdi_net_device* device, void* buffer, size_t size)
{
    auto f = [&] (Device *p) {
        if (p->getType() == Device::Network)
        {
            if (p->getSpecificType().compare("CDI NIC"))
            {
                CdiNet *pNet = static_cast<CdiNet *>(p);
                if (pNet->getCdiDevice() == device)
                {
                    // Submit the packet.
                    NetworkStack::instance().receive(size, reinterpret_cast<uintptr_t>(buffer), pNet, 0);
                }
            }
        }
        return p;
    };

    auto c = pedigree_std::make_callable(f);

    Device::foreach(c, 0);
}

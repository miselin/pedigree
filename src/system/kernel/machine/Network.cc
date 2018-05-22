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

#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

StationInfo::StationInfo()
    : ipv4(), ipv6(0), nIpv6Addresses(0), subnetMask(), broadcast(0xFFFFFFFF),
      gateway(), gatewayIpv6(IpAddress::IPv6), dnsServers(0), nDnsServers(0),
      mac(), nPackets(0), nDropped(0), nBad(0)
{
}

StationInfo::StationInfo(const StationInfo &info)
    : ipv4(info.ipv4), ipv6(info.ipv6), nIpv6Addresses(info.nIpv6Addresses),
      subnetMask(info.subnetMask), broadcast(info.broadcast),
      gateway(info.gateway), gatewayIpv6(info.gatewayIpv6),
      dnsServers(info.dnsServers), nDnsServers(info.nDnsServers), mac(info.mac),
      nPackets(info.nPackets), nDropped(info.nDropped), nBad(info.nBad)
{
}

StationInfo::~StationInfo()
{
}

Network::Network() : m_StationInfo()
{
    m_SpecificType = "Generic Network Device";
}

Network::Network(Network *pDev) : Device(pDev), m_StationInfo()
{
}

Network::~Network()
{
}

Device::Type Network::getType()
{
    return Device::Network;
}

void Network::getName(String &str)
{
    str = "Generic Network Device";
}

void Network::dump(String &str)
{
    str = "Generic Network Device";
}

bool Network::setStationInfo(const StationInfo &info)
{
    return false;  // failed by default
}

const StationInfo &Network::getStationInfo()
{
    static StationInfo info;
    return info;  // not to be trusted
}

bool Network::isConnected()
{
    return true;
}

uint32_t Network::convertToIpv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return a | (b << 8) | (c << 16) | (d << 24);
}

IpAddress Network::convertToIpv6(
    uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, uint8_t g,
    uint8_t h, uint8_t i, uint8_t j, uint8_t k, uint8_t l, uint8_t m, uint8_t n,
    uint8_t o, uint8_t p)
{
    uint8_t temp[16] = {a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p};
    IpAddress ipv6(temp);
    return temp;
}

uint16_t Network::calculateChecksum(uintptr_t buffer, size_t nBytes)
{
    uint32_t sum = 0;
    uint16_t *data = reinterpret_cast<uint16_t *>(buffer);

    while (nBytes > 1)
    {
        sum += *data++;
        nBytes -= sizeof(uint16_t);
    }

    // odd bytes
    if (nBytes > 0)
    {
        uint8_t *data8 = reinterpret_cast<uint8_t *>(data);
        sum += *data8;
    }

    // fold to 16 bits
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    uint16_t ret = static_cast<uint16_t>(~sum);
    return ret;
}

void Network::gotPacket()
{
    m_StationInfo.nPackets++;
}

void Network::droppedPacket()
{
    m_StationInfo.nDropped++;
}

void Network::badPacket()
{
    m_StationInfo.nBad++;
}

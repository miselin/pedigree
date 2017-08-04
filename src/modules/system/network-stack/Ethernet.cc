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

#include "Ethernet.h"
#include "Arp.h"
#include "Ipv4.h"
#include "Ipv6.h"
#include "RawManager.h"
#include "modules/Module.h"
#include "pedigree/kernel/Log.h"

#include "Filter.h"

Ethernet Ethernet::ethernetInstance;

Ethernet::Ethernet() = default;
Ethernet::~Ethernet() = default;

void Ethernet::receive(NetworkStack::Packet *packet)
    // size_t nBytes, uintptr_t packet, Network *pCard, uint32_t offset)
{
    if (!packet || !packet->getBuffer() || !packet->getLength() || !packet->getCard())
        return;

    uintptr_t packetBuffer = packet->getBuffer();
    size_t nBytes = packet->getLength();
    Network *pCard = packet->getCard();
    size_t offset = packet->getOffset();

    // Check for filtering
    if (!NetworkFilter::instance().filter(1, packetBuffer, nBytes))
    {
        pCard->droppedPacket();
        return;  // Drop the packet.
    }

    // grab the header
    ethernetHeader *ethHeader =
        reinterpret_cast<ethernetHeader *>(packetBuffer + offset);

#ifndef DISABLE_RAWNET
    // dump this packet into the RAW sockets
    RawManager::instance().receive(packetBuffer, nBytes, 0, -1, pCard);
#endif

    // what type is the packet?
    bool queued = false;
    switch (BIG_TO_HOST16(ethHeader->type))
    {
        case ETH_ARP:
            // NOTICE("ARP packet!");

            Arp::instance().receive(
                nBytes, packetBuffer, pCard, sizeof(ethernetHeader));

            break;

        case ETH_RARP:
            NOTICE("RARP packet!");
            break;

        case ETH_IPV4:
        case ETH_IPV6:
            // IP protocol packets are queued for asynchronous handling.
            queued = true;
            produce(reinterpret_cast<uint64_t>(packet));
            break;

        default:
            NOTICE(
                "Unknown ethernet packet - type is "
                << Hex << BIG_TO_HOST16(ethHeader->type) << "!");
            pCard->badPacket();
            break;
    }

    if (!queued)
    {
        delete packet;  // all done
    }
}

size_t Ethernet::injectHeader(
    uintptr_t packet, MacAddress destMac, MacAddress sourceMac, uint16_t type)
{
    // Basic checks for valid input
    if (!packet || !type)
        return 0;

    // Set up an Ethernet header
    ethernetHeader *pHeader = reinterpret_cast<ethernetHeader *>(packet);

    // Copy in the two MAC addresses
    MemoryCopy(pHeader->destMac, destMac.getMac(), 6);
    MemoryCopy(pHeader->sourceMac, sourceMac.getMac(), 6);

    // Set the packet type
    pHeader->type = HOST_TO_BIG16(type);

    // All done.
    return sizeof(ethernetHeader);
}

void Ethernet::getMacFromPacket(uintptr_t packet, MacAddress *mac)
{
    if (packet && mac)
    {
        // grab the header
        ethernetHeader *ethHeader = reinterpret_cast<ethernetHeader *>(packet);
        *mac = ethHeader->sourceMac;
    }
}

void Ethernet::send(
    size_t nBytes, uintptr_t packet, Network *pCard, MacAddress dest,
    uint16_t type)
{
    if (!pCard || !pCard->isConnected())
        return;  // NIC isn't active

    // Move the payload for the ethernet header to go in
    MemoryCopy(
        reinterpret_cast<void *>(packet + sizeof(ethernetHeader)),
        reinterpret_cast<void *>(packet), nBytes);

    // get the ethernet header pointer
    ethernetHeader *ethHeader = reinterpret_cast<ethernetHeader *>(packet);

    // copy in the data
    StationInfo me = pCard->getStationInfo();
    MemoryCopy(ethHeader->destMac, dest.getMac(), 6);
    MemoryCopy(ethHeader->sourceMac, me.mac, 6);
    ethHeader->type = HOST_TO_BIG16(type);

    // Check for filtering
    /// \todo need to indicate in/out direction in NetworkFilter
    if (!NetworkFilter::instance().filter(
            1, packet, nBytes + sizeof(ethernetHeader)))
    {
        pCard->droppedPacket();
        return;  // Drop the packet.
    }

    // send it over the network
    pCard->send(nBytes + sizeof(ethernetHeader), packet);

    // and dump it into any raw sockets (note the -1 for protocol - this means
    // WIRE level endpoints) RawManager::instance().receive(packAddr, newSize,
    // 0, -1, pCard);
}

void Ethernet::consume(uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8)
{
    NetworkStack::Packet *packet = reinterpret_cast<NetworkStack::Packet *>(p0);
    if (!packet)
    {
        ERROR("Null packet given to Ethernet::consume()");
        return;
    }

    uintptr_t packetBuffer = packet->getBuffer();
    size_t nBytes = packet->getLength();
    Network *pCard = packet->getCard();
    size_t offset = packet->getOffset();

    // grab the header
    ethernetHeader *ethHeader =
        reinterpret_cast<ethernetHeader *>(packetBuffer + offset);

    uint16_t type = BIG_TO_HOST16(ethHeader->type);

    if (type == ETH_IPV4)
    {
        Ipv4::instance().receive(
            nBytes, packetBuffer, pCard, sizeof(ethernetHeader));
    }
    else if (type == ETH_IPV6)
    {
        Ipv6::instance().receive(
            nBytes, packetBuffer, pCard, sizeof(ethernetHeader));
    }
    else
    {
        ERROR("Ethernet: unhandled packet type in consume()");
    }

    delete packet;
}

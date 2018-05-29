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

#ifndef MACHINE_NETWORK_H
#define MACHINE_NETWORK_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/network/IpAddress.h"
#include "pedigree/kernel/network/MacAddress.h"
#include "pedigree/kernel/processor/types.h"

class String;

/** Station information - basically information about this station, per NIC */
class EXPORTED_PUBLIC StationInfo
{
  public:
    // Broadcast address defaults to 255.255.255.255, as we may need to
    // broadcast without a known IPv4 address (and therefore no known network
    // or broadcast address).
    StationInfo();
    StationInfo(const StationInfo &info);
    virtual ~StationInfo();

    IpAddress ipv4;
    IpAddress *ipv6;  // Not compulsory
    size_t nIpv6Addresses;

    IpAddress subnetMask;
    IpAddress broadcast;  /// Automatically calculated?
    IpAddress gateway;
    IpAddress gatewayIpv6;

    IpAddress *dnsServers;  /// Can contain IPv6 addresses.
    size_t nDnsServers;

    MacAddress mac;  // MAC address

    size_t nPackets;  /// Number of packets passed through the interface
    size_t nDropped;  /// Number of packets dropped by the filter
    size_t nBad;      /// Number of packets dropped because they were invalid

    StationInfo &operator=(const StationInfo &info) = delete;
};

/**
 * A network device (sends/receives packets on a network)
 */
class EXPORTED_PUBLIC Network : public Device
{
  public:
    Network();
    Network(Network *pDev);
    virtual ~Network();

    virtual Type getType();

    virtual void getName(String &str);

    virtual void dump(String &str);

    /** Sends a given packet through the device.
     * \param nBytes The number of bytes to send.
     * \param buffer A buffer with the packet to send */
    virtual bool send(size_t nBytes, uintptr_t buffer) = 0;

    /** Sets station information (such as IP addresses)
     * \param info The information to set as the station info */
    virtual bool setStationInfo(const StationInfo &info);

    /** Gets station information (such as IP addresses) */
    virtual const StationInfo &getStationInfo();

    /** Is this device actually connected to a network? */
    virtual bool isConnected();

    /** Converts an IPv4 address into an integer */
    EXPORTED_PUBLIC static uint32_t
    convertToIpv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

    /** Converts an IPv6 address into an IpAddress object */
    EXPORTED_PUBLIC static IpAddress convertToIpv6(
        uint8_t a, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0, uint8_t e = 0,
        uint8_t f = 0, uint8_t g = 0, uint8_t h = 0, uint8_t i = 0,
        uint8_t j = 0, uint8_t k = 0, uint8_t l = 0, uint8_t m = 0,
        uint8_t n = 0, uint8_t o = 0, uint8_t p = 0);

    /** Calculates a checksum */
    EXPORTED_PUBLIC static uint16_t
    calculateChecksum(uintptr_t buffer, size_t nBytes);

    /** Packet statistics */

    /// Called when a packet is picked up by the system, regardless of if it's
    /// eventually bad or dropped
    virtual void gotPacket();

    /// Called when a packet is dropped by the system
    virtual void droppedPacket();

    /// Called when a packet is determined to be "bad" by the system (ie,
    /// invalid checksum).
    virtual void badPacket();

  protected:
    StationInfo m_StationInfo;
};

#endif

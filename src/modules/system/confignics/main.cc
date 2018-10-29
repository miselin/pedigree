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

#include "modules/Module.h"
#include "modules/system/network-stack/NetworkStack.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/machine/DeviceHashTree.h"
#include "pedigree/kernel/processor/Processor.h"

#include "modules/system/lwip/include/lwip/dhcp.h"
#include "modules/system/lwip/include/lwip/ip_addr.h"
#include "modules/system/lwip/include/lwip/netif.h"

static int configureInterfaces()
{
    // Fill out the device hash table (needed in RoutingTable)
    DeviceHashTree::instance().fill();

    // Build routing tables - try to find a default configuration that can
    // connect to the outside world
    IpAddress empty;
    Network *pDefaultCard = 0;
    for (size_t i = 0; i < NetworkStack::instance().getNumDevices(); i++)
    {
        /// \todo Perhaps try and ping a remote host?
        Network *card = NetworkStack::instance().getDevice(i);

        struct netif *iface = NetworkStack::instance().getInterface(card);
        if (!iface)
        {
            continue;
        }

        // Do the initial setup we need to get the interface up.
        ip4_addr_t ipaddr;
        ip4_addr_t netmask;
        ip4_addr_t gateway;
        ByteSet(&ipaddr, 0, sizeof(ipaddr));
        ByteSet(&netmask, 0, sizeof(netmask));
        ByteSet(&gateway, 0, sizeof(gateway));

        netif_set_addr(iface, &ipaddr, &netmask, &gateway);
        netif_set_ip6_autoconfig_enabled(iface, 1);
        netif_create_ip6_linklocal_address(iface, 1);
        netif_set_link_up(iface);
        netif_set_up(iface);
    }

    return 0;
}

static bool init()
{
    configureInterfaces();
    return false;  // unload after starting dhcp for all interfaces
}

static void destroy()
{
}

MODULE_INFO("confignics", &init, &destroy, "network-stack", "lwip");
MODULE_OPTIONAL_DEPENDS("nics", "pcap");

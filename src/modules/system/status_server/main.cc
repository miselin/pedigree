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
#include "modules/system/network-stack/ConnectionBasedEndpoint.h"
#include "modules/system/network-stack/Endpoint.h"
#include "modules/system/network-stack/NetworkStack.h"
#include "modules/system/network-stack/RoutingTable.h"
#include "modules/system/network-stack/TcpManager.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Version.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/Processor.h"

#include "modules/system/lwip/include/lwip/api.h"
#include "modules/system/lwip/include/lwip/ip_addr.h"
#include "modules/system/lwip/include/lwip/netif.h"

#define LISTEN_PORT 1234

static Tree<struct netconn *, Mutex *> g_Netconns;

static bool g_Running = false;
static Thread *g_pServerThread = nullptr;

static void netconnCallback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
    Mutex *mutex = g_Netconns.lookup(conn);
    if (mutex && (evt == NETCONN_EVT_RCVPLUS || evt == NETCONN_EVT_SENDPLUS || evt == NETCONN_EVT_ERROR))
    {
        // wake up waiter, positive event
        mutex->release();
    }
}

static int clientThread(void *p)
{
    if (!p)
        return 0;

    struct netconn *connection = reinterpret_cast<struct netconn *>(p);
    connection->callback = netconnCallback;

    bool stillOk = true;
    bool requestComplete = false;

    String httpRequest;
    String httpResponse;
    err_t err;
    while (!requestComplete)
    {
        struct netbuf *buf = nullptr;
        if ((err = netconn_recv(connection, &buf)) != ERR_OK)
        {
            if (err == ERR_RST || err == ERR_CLSD)
            {
                WARNING("Unexpected disconnection from remote client.");
                stillOk = false;
                break;
            }
            else
            {
                ERROR("error in recv: " << lwip_strerr(err));
            }
            continue;
        }

        do
        {
            void *data = nullptr;
            u16_t len = 0;
            netbuf_data(buf, &data, &len);

            if (stillOk && len)
            {
                httpRequest += String(reinterpret_cast<char *>(data), len);

                if (httpRequest.length() >= 4)
                {
                    if (!(httpRequest.startswith("GET") || httpRequest.startswith("HEAD")))
                    {
                        // We really don't want to deal with this.
                        httpResponse = "HTTP/1.1 400 Bad Request\r\nAllow: GET, HEAD\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nThe Pedigree built-in status server only accepts GET and HEAD requests.";
                        stillOk = false;
                    }
                }

                if (stillOk)
                {
                    if (StringContains(static_cast<const char *>(httpRequest), "\r\n\r\n"))
                    {
                        // no more data needed, we have the full request
                        requestComplete = true;
                    }
                }
            }
        }
        while (netbuf_next(buf) >= 0);

        netbuf_delete(buf);
    }

    // no longer needing to RX any data
    netconn_shutdown(connection, 1, 0);

    if (!stillOk)
    {
        if (httpResponse.length())
        {
            netconn_write(connection, static_cast<const char *>(httpResponse), httpResponse.length(), 0);
            netconn_shutdown(connection, 1, 1);
        }

        netconn_close(connection);
        netconn_delete(connection);
        return 0;
    }

    // Build the response.
    bool bHeadRequest = !httpRequest.startswith("GET");
    bool bNotFound = false;  /// \todo add path parsing

    // Got a heap of information now - prepare to return
    size_t code = bNotFound ? 404 : 200;
    NormalStaticString statusLine;
    statusLine = "HTTP/1.1 ";
    statusLine += code;
    statusLine += " ";
    statusLine += bNotFound ? "Not Found" : "OK";

    // Build up the reply.
    String responseContent;
    if (bNotFound)
    {
        responseContent += "Error 404: Page not found.";
    }
    else
    {
        responseContent += "<html><head><title>Pedigree - Live System Status "
                    "Report</title></head><body>";
        responseContent += "<h1>Pedigree Live Status Report</h1>";
        responseContent += "<p>This is a live status report from a running "
                    "Pedigree system.</p>";
        responseContent += "<h3>Current Build</h3><pre>";

        {
            HugeStaticString str;
            str += "Pedigree - revision ";
            str += g_pBuildRevision;
            str += "<br />===========================<br />Built at ";
            str += g_pBuildTime;
            str += " by ";
            str += g_pBuildUser;
            str += " on ";
            str += g_pBuildMachine;
            str += "<br />Build flags: ";
            str += g_pBuildFlags;
            str += "<br />";
            responseContent += str;
        }

        responseContent += "</pre>";

        responseContent += "<h3>Network Interfaces</h3>";
        responseContent += "<table border='1'><tr><th>Interface</th><th>IP "
                    "Addresses</th><th>Subnet "
                    "Mask</th><th>Gateway</th><th>Driver Name</th><th>MAC "
                    "address</th><th>Statistics</th></tr>";
        for (size_t i = 0; i < NetworkStack::instance().getNumDevices();
             i++)
        {
            /// \todo switch to using netif interface for all this
            Network *card = NetworkStack::instance().getDevice(i);
            StationInfo info = card->getStationInfo();

            struct netif *iface = NetworkStack::instance().getInterface(card);
            if (!iface)
            {
                continue;
            }

            // Interface number
            responseContent += "<tr><td>";
            NormalStaticString s;
            s.append(iface->name, 2);
            s.append(iface->num);
            responseContent += s;
            if (iface == netif_default)
            {
                responseContent += " <b>(default interface)</b>";
            }
            responseContent += "</td>";

            // IP address(es)
            responseContent += "<td>";
            const ip4_addr_t *ip4 = netif_ip4_addr(iface);
            responseContent += ip4addr_ntoa(ip4);
            for (size_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i)
            {
                const ip6_addr_t *ip6 = netif_ip6_addr(iface, i);
                if (ip6_addr_isany(ip6))
                {
                    continue;
                }
                responseContent += "<br />";
                responseContent += ip6addr_ntoa(ip6);
            }
            responseContent += "</td>";

            const ip4_addr_t *subnet4 = netif_ip4_netmask(iface);
            const ip4_addr_t *gw4 = netif_ip4_gw(iface);

            // Subnet mask
            responseContent += "<td>";
            responseContent += ip4addr_ntoa(subnet4);
            responseContent += "</td>";

            // Gateway
            responseContent += "<td>";
            responseContent += ip4addr_ntoa(gw4);
            responseContent += "</td>";

            // Driver name
            responseContent += "<td>";
            String cardName;
            card->getName(cardName);
            responseContent += cardName;
            responseContent += "</td>";

            // MAC
            responseContent += "<td>";
            responseContent += info.mac.toString();
            responseContent += "</td>";

            // Statistics
            responseContent += "<td>";
            s.clear();
            s += "Packets: ";
            s.append(info.nPackets);
            s += "<br />Dropped: ";
            s.append(info.nDropped);
            s += "<br />RX Errors: ";
            s.append(info.nBad);
            responseContent += s;
            responseContent += "</td>";

            responseContent += "</tr>";
        }
        responseContent += "</table>";

        responseContent += "<h3>VFS</h3>";
        responseContent += "<table border='1'><tr><th>VFS Alias</th><th>Disk</th></tr>";

        typedef List<String *> StringList;
        typedef Tree<Filesystem *, List<String *> *> VFSMountTree;

        VFSMountTree &mounts = VFS::instance().getMounts();

        for (VFSMountTree::Iterator it = mounts.begin(); it != mounts.end();
             it++)
        {
            Filesystem *pFs = it.key();
            StringList *pList = it.value();
            Disk *pDisk = pFs->getDisk();

            for (StringList::Iterator j = pList->begin(); j != pList->end();
                 j++)
            {
                String mount = **j;
                String diskInfo, temp;

                if (pDisk)
                {
                    pDisk->getName(temp);
                    pDisk->getParent()->getName(diskInfo);

                    diskInfo += " -- ";
                    diskInfo += temp;
                }
                else
                    diskInfo = "(no disk)";

                responseContent += "<tr><td>";
                responseContent += mount;
                responseContent += "</td><td>";
                responseContent += diskInfo;
                responseContent += "</td></tr>";
            }
        }

        responseContent += "</table>";

#ifdef X86_COMMON
        responseContent += "<h3>Memory Usage (KiB)</h3>";
        responseContent += "<table "
                    "border='1'><tr><th>Heap</th><th>Used</th><th>Free</"
                    "th></tr>";
        {
            extern size_t g_FreePages;
            extern size_t g_AllocedPages;

            NormalStaticString str;
            str += "<tr><td>";
            str += SlamAllocator::instance().heapPageCount() * 4;
            str += "</td><td>";
            str += (g_AllocedPages * 4096) / 1024;
            str += "</td><td>";
            str += (g_FreePages * 4096) / 1024;
            str += "</td></tr>";
            responseContent += str;
        }
        responseContent += "</table>";
#endif

        responseContent += "<h3>Processes</h3>";
        responseContent += "<table "
                    "border='1'><tr><th>PID</th><th>Description</"
                    "th><th>Virtual Memory (KiB)</th><th>Physical Memory "
                    "(KiB)</th><th>Shared Memory (KiB)</th>";
        for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i)
        {
            responseContent += "<tr>";
            Process *pProcess = Scheduler::instance().getProcess(i);
            HugeStaticString str;

            ssize_t virtK =
                (pProcess->getVirtualPageCount() * 0x1000) / 1024;
            ssize_t physK =
                (pProcess->getPhysicalPageCount() * 0x1000) / 1024;
            ssize_t shrK = (pProcess->getSharedPageCount() * 0x1000) / 1024;

            /// \todo add timing
            str.append("<td>");
            str.append(pProcess->getId());
            str.append("</td><td>");
            str.append(pProcess->description());
            str.append("</td><td>");
            str.append(virtK, 10);
            str.append("</td><td>");
            str.append(physK, 10);
            str.append("</td><td>");
            str.append(shrK, 10);
            str.append("</td>");

            responseContent += str;
            responseContent += "</tr>";
        }
        responseContent += "</table>";

        responseContent += "</body></html>";
    }

    String contentLength;
    contentLength.Format("\r\nContent-Length: %d", responseContent.length());

    httpResponse = statusLine;
    httpResponse += contentLength;
    httpResponse += "\r\nContent-type: text/html; charset=utf-8";
    httpResponse += "\r\nConnection: close";
    httpResponse += "\r\n\r\n";
    httpResponse += responseContent;

    Mutex *mutex = new Mutex(true);

    g_Netconns.insert(connection, mutex);

    /// \todo error handling
    netconn_write(connection, static_cast<const char *>(httpResponse), httpResponse.length(), 0);
    netconn_close(connection);

    while (!mutex->acquire())
        ;

    g_Netconns.remove(connection);

    // Connection closed cleanly, delete our netconn now.
    netconn_delete(connection);

    return 0;
}

static int mainThread(void *)
{
    struct netconn *server = netconn_new(NETCONN_TCP);

    ip_addr_t ipaddr;
    ByteSet(&ipaddr, 0, sizeof(ipaddr));

    netconn_bind(server, &ipaddr, LISTEN_PORT);

    netconn_listen(server);

    g_Running = true;
    while (g_Running)
    {
        /// \todo need to abort accept() somehow to cancel this thread
        struct netconn *connection;
        if (netconn_accept(server, &connection) == ERR_OK)
        {
            Thread *pThread = new Thread(
                Processor::information().getCurrentThread()->getParent(),
                clientThread, connection);
            pThread->detach();
        }
    }

    netconn_close(server);
    netconn_delete(server);

    return 0;
}

static bool init()
{
    g_pServerThread = new Thread(
        Processor::information().getCurrentThread()->getParent(), mainThread,
        nullptr);
    return true;
}

static void destroy()
{
    /// \todo need to stop the listen thread's accept() call somehow
    g_Running = false;
    if (g_pServerThread)
    {
        g_pServerThread->join();
    }
}

MODULE_INFO("Status Server", &init, &destroy, "config", "lwip");
MODULE_OPTIONAL_DEPENDS("confignics");

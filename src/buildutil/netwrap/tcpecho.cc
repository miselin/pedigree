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

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <getopt.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "TunWrapper.h"
#include "config-shim.h"

#include "modules/system/network-stack/NetworkStack.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/DeviceHashTree.h"
#include "pedigree/kernel/utilities/pocketknife.h"

#include "modules/system/lwip/include/lwip/api.h"
#include "modules/system/lwip/include/lwip/init.h"
#include "modules/system/lwip/include/lwip/ip_addr.h"
#include "modules/system/lwip/include/lwip/netif.h"
#include "modules/system/lwip/include/lwip/tcp.h"
#include "modules/system/lwip/include/lwip/tcpip.h"

static jmp_buf g_jb;

class StreamingStderrLogger : public Log::LogCallback
{
  public:
    void callback(const char *str)
    {
        std::cerr << str;
    }
};

static void sigint(int signo)
{
    siglongjmp(g_jb, 1);
}

static err_t
echo_server_rx(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct pbuf *q = p;
    bool needDisconnect = false;
    size_t totalBytes = 0;
    while (q != nullptr)
    {
        u16_t sndbufSize = tcp_sndbuf(pcb);
        NOTICE("send buffer size: " << sndbufSize);

        tcp_recved(pcb, totalBytes);

        /// \todo check for errors
        err_t e = tcp_write(
            pcb, q->payload, q->len, q->next ? TCP_WRITE_FLAG_MORE : 0);
        if (e != ERR_OK)
        {
            ERROR("failed to send some data!");
        }

        totalBytes += q->len;

        char *buf = reinterpret_cast<char *>(q->payload);
        for (size_t i = 0; i < q->len; ++i)
        {
            if (buf[i] == '\x01')
            {
                NOTICE("TCPECHO Client Complete.");
                needDisconnect = true;
                break;
            }
        }

        q = q->next;
    }

    // yep, handled this data fine
    tcp_recved(pcb, totalBytes);

    if (needDisconnect)
    {
        tcp_close(pcb);
    }

    return ERR_OK;
}

static int echo_server_conn(void *arg)
{
    struct netconn *connection = reinterpret_cast<struct netconn *>(arg);

    err_t err = 0;

    bool running = true;
    while (running)
    {
        struct netbuf *buf = nullptr;
        if ((err = netconn_recv(connection, &buf)) != ERR_OK)
        {
            if (err == ERR_RST || err == ERR_CLSD)
            {
                WARNING("Unexpected disconnection from remote client.");
                running = false;
            }
            else
            {
                ERROR("error in recv: " << lwip_strerr(err));
            }
            continue;
        }

        do
        {
            // echo all bytes we receive back
            void *data = nullptr;
            u16_t len = 0;
            netbuf_data(buf, &data, &len);

            if (running)
            {
                // check for a possible end of data
                for (u16_t i = 0; i < len; ++i)
                {
                    if (reinterpret_cast<char *>(data)[i] == '\x01')
                    {
                        running = false;
                        break;
                    }
                }
            }

            netconn_write(connection, data, len, NETCONN_COPY);
        } while (netbuf_next(buf) >= 0);

        netbuf_delete(buf);
    }

    // all finished
    netconn_close(connection);
    netconn_delete(connection);

    return ERR_OK;
}

static int echo_server(void *p)
{
    struct netconn *server = netconn_new(NETCONN_TCP);

    ip_addr_t ipaddr;
    ByteSet(&ipaddr, 0, sizeof(ipaddr));

    netconn_bind(server, &ipaddr, 8080);

    netconn_listen(server);

    while (1)
    {
        NOTICE("waiting for a connection");
        struct netconn *connection;
        if (netconn_accept(server, &connection) == ERR_OK)
        {
            NOTICE("accepting connection!");
            // pocketknife::runConcurrently(echo_server_conn,
            // reinterpret_cast<void *>(connection));
            echo_server_conn(connection);
        }
        else
        {
            NOTICE("accept() failed");
        }
    }

    return 0;
}

static int open_tun(const char *interface)
{
    struct ifreq ifr;

    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open /dev/net/tun");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;  // No need for packet info
    strncpy(ifr.ifr_name, interface, IFNAMSIZ);

    int err = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (err < 0)
    {
        perror("Failed to select tun device");
        close(fd);
        return -1;
    }

    return fd;
}

static Mutex tcpipInitPending(false);

static void tcpipInitComplete(void *)
{
    tcpipInitPending.release();
}

static void mainloop(int fd)
{
    struct in_addr in;
    int e = inet_pton(AF_INET, "192.168.15.2", &in);
    if (e < 0)
    {
        perror("Cannot set up IP address");
        return;
    }

    tcpipInitPending.acquire();

    // make sure the multi threaded lwIP implementation is ready to go
    tcpip_init(tcpipInitComplete, nullptr);

    tcpipInitPending.acquire();

    NetworkStack *stack = new NetworkStack();

    // StationInfo for our static configuration.
    StationInfo info;
    info.ipv4.setIp(in.s_addr);
    info.ipv6 = nullptr;
    info.nIpv6Addresses = 0;
    info.subnetMask.setIp(0xffffff);
    info.broadcast.setIp(0xff000000 | in.s_addr);
    info.dnsServers = nullptr;
    info.nDnsServers = 0;
    uint8_t mac[6] = {0, 0xab, 0xcd, 0, 0, 0x1};
    for (size_t i = 0; i < 6; ++i)
    {
        info.mac.setMac(mac[i], i);
    }

    TunWrapper *wrapper = new TunWrapper();
    wrapper->setStationInfo(info);
    NetworkStack::instance().registerDevice(wrapper);

    struct netif *iface = NetworkStack::instance().getInterface(wrapper);

    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ByteSet(&gateway, 0, sizeof(gateway));

    ipaddr.addr = in.s_addr;
    netmask.addr = info.subnetMask.getIp();

    netif_set_addr(iface, &ipaddr, &netmask, &gateway);
    netif_set_default(iface);
    netif_set_link_up(iface);
    netif_set_up(iface);

    DeviceHashTree::instance().fill(wrapper);

    if (sigsetjmp(g_jb, 0))
    {
        // Signal handled, we need to quit.
        /// \todo probably should clean up network stack
        std::cerr << "Shutting down, received interrupt." << std::endl;
        return;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint;

    e = sigaction(SIGINT, &sa, nullptr);
    if (e < 0)
    {
        perror("Cannot setup SIGINT handler");
        return;
    }

    pocketknife::runConcurrently(echo_server, nullptr);

    // Good to go - run the card!
    wrapper->run(fd);
}

static void usage()
{
    std::cerr << "Usage: tcpecho [options]" << std::endl;
    std::cerr << "Run an instance of the Pedigree network stack on a tun/tap "
                 "interface, with a TCP echo server running on port 8080."
              << std::endl;
    std::cerr << std::endl;
    std::cerr << "  --version, -[vV] Print version and exit successfully."
              << std::endl;
    std::cerr << "  --help,          Print this help and exit successfully."
              << std::endl;
    std::cerr << "  --tap, -t        Device name to open (e.g. tun0)."
              << std::endl;
    std::cerr << "  --quiet, -q      Don't print logs to stderr." << std::endl;
    std::cerr << std::endl;
}

static void version()
{
    std::cerr << "netwrap v1.0, Copyright (C) 2014, Pedigree Developers"
              << std::endl;
}

int main(int argc, char *argv[])
{
    const char *interface = nullptr;
    bool quiet = false;
    const struct option long_options[] = {
        {"tap", required_argument, 0, 't'},
        {"version", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {"quiet", no_argument, 0, 'q'},
        {0, 0, 0, 0},
    };

    opterr = 1;
    while (1)
    {
        int c = getopt_long(argc, argv, "t:vVhq", long_options, NULL);
        if (c < 0)
        {
            break;
        }

        switch (c)
        {
            case 't':
                interface = optarg;
                break;

            case 'v':
            case 'V':
                version();
                return 0;

            case ':':
                std::cerr << "At least one required option was missing."
                          << std::endl;
            case 'h':
                usage();
                return 0;

            case 'q':
                quiet = true;
                break;

            default:
                usage();
                return 1;
        }
    }

    argc -= optind;
    argv += optind;

    if (!interface)
    {
        usage();
        return 1;
    }

    StreamingStderrLogger logger;
    if (!quiet)
    {
        Log::instance().installCallback(&logger, true);
    }

    int result = initialize_config();
    if (result < 0)
    {
        std::cerr << "Failed to open configuration database." << std::endl;
        return 1;
    }

    int fd = open_tun(interface);
    if (fd < 0)
    {
        std::cerr << "Failed to open interface '" << interface << "'."
                  << std::endl;
        return 1;
    }

    mainloop(fd);

    close(fd);
    destroy_config();

    if (!quiet)
    {
        Log::instance().removeCallback(&logger);
    }

    return 0;
}

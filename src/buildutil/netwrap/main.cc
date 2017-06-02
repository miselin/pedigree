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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/DeviceHashTree.h"
#include <network-stack/NetworkStack.h>
#include <network-stack/RoutingTable.h>
#include <network-stack/TcpManager.h>

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

static void mainloop(int fd)
{
    struct in_addr in;
    int e = inet_pton(AF_INET, "192.168.15.2", &in);
    if (e < 0)
    {
        perror("Cannot set up IP address");
        return;
    }

    NetworkStack *stack = new NetworkStack();
    TcpManager *tcpManager = new TcpManager();

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

    DeviceHashTree::instance().fill(wrapper);

    IpAddress empty;
    RoutingTable::instance().initialise();
    RoutingTable::instance().Add(
        RoutingTable::Named, empty, empty, String("default"), wrapper);

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

    // Good to go - run the card!
    wrapper->run(fd);
}

static void usage()
{
    std::cerr << "Usage: netwrap [options]" << std::endl;
    std::cerr << "Run an instance of the Pedigree network stack on a tun/tap "
                 "interface."
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

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

#include "TunWrapper.h"

#include <cstdio>
#include <iostream>

#include <poll.h>
#include <unistd.h>
#include <errno.h>

#include <network-stack/NetworkStack.h>
#include <utilities/pocketknife.h>

TunWrapper::TunWrapper() : m_StationInfo(), m_Fd(-1)
{
    m_SpecificType = "Pedigree TUN/TAP Device Wrapper";
}

TunWrapper::TunWrapper(Network *pDev) : Network(pDev), m_StationInfo()
{
}

TunWrapper::~TunWrapper()
{
}

Device::Type TunWrapper::getType()
{
    return Device::Network;
}

void TunWrapper::getName(String &str)
{
    str = "Pedigree TUN/TAP Wrapper";
}

void TunWrapper::dump(String &str)
{
    str = "Pedigree TUN/TAP Wrapper";
}

bool TunWrapper::send(size_t nBytes, uintptr_t buffer)
{
    if (m_Fd < 0)
    {
        return false;
    }

    return write(m_Fd, reinterpret_cast<void *>(buffer), nBytes) == nBytes;
}

bool TunWrapper::setStationInfo(StationInfo info)
{
    m_StationInfo.ipv4 = info.ipv4;
    NOTICE("TUNTAP: Setting ipv4, " << info.ipv4.toString());

    m_StationInfo.subnetMask = info.subnetMask;
    NOTICE("TUNTAP: Setting subnetMask, " << info.subnetMask.toString());

    m_StationInfo.gateway = info.gateway;
    NOTICE("TUNTAP: Setting gateway, " << info.gateway.toString());

    m_StationInfo.mac = info.mac;
    NOTICE("TUNTAP: Setting mac, " << info.mac.toString());

    return true;
}

StationInfo TunWrapper::getStationInfo()
{
    return m_StationInfo;
}

void TunWrapper::run(int fd)
{
    pocketknife::runConcurrently(packetPusherThread, this);

    m_Fd = fd;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR;

    while (1)
    {
        int ready = poll(&pfd, 1, -1);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            else
            {
                perror("Failed to poll");
                return;
            }
        }

        if ((pfd.revents & POLLIN) == POLLIN)
        {
            packet *p = new packet;
            memset(p, 0, sizeof(*p));

            ssize_t bytes = read(fd, p->buffer, sizeof p->buffer);
            p->bytes = bytes;

            if (bytes >= 0)
            {
                lock.acquire();
                m_Packets.pushBack(p);
                cond.signal();
                lock.release();
            }
        }
        else if ((pfd.revents & POLLERR) == POLLERR)
        {
            /// \todo how to handle this?
        }
    }
}

int TunWrapper::packetPusherThread(void *param)
{
    TunWrapper *wrapper = reinterpret_cast<TunWrapper *>(param);
    wrapper->packetPusher();
    return 0;
}

void TunWrapper::packetPusher()
{
    lock.acquire();
    while (true)
    {
        if (!m_Packets.count())
        {
            cond.wait(lock);
            continue;
        }

        packet *p = m_Packets.popFront();
        NetworkStack::instance().receive(p->bytes, reinterpret_cast<uintptr_t>(p->buffer), this, 0);
        delete p;
    }
    lock.release();
}

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

#include "Endpoint.h"
#include "ConnectionlessEndpoint.h"
#include "ConnectionBasedEndpoint.h"

Endpoint::Endpoint() :
    m_Sockets(), m_LocalPort(0), m_RemotePort(0), m_LocalIp(), m_RemoteIp(),
    m_Manager(0), m_Error(Error::NoError), m_bConnection(false)
{
}

Endpoint::Endpoint(uint16_t local, uint16_t remote) :
    m_Sockets(), m_LocalPort(local), m_RemotePort(remote), m_LocalIp(), m_RemoteIp(),
    m_Manager(0), m_Error(Error::NoError), m_bConnection(false)
{
}

Endpoint::Endpoint(IpAddress remoteIp, uint16_t local, uint16_t remote) :
    m_Sockets(), m_LocalPort(local), m_RemotePort(remote), m_LocalIp(), m_RemoteIp(remoteIp),
    m_Manager(0), m_Error(Error::NoError), m_bConnection(false)
{
}

Endpoint::~Endpoint()
{
}

uint16_t Endpoint::getLocalPort() const
{
    return m_LocalPort;
}

uint16_t Endpoint::getRemotePort() const
{
    return m_RemotePort;
}

const IpAddress &Endpoint::getLocalIp() const
{
    return m_LocalIp;
}

const IpAddress &Endpoint::getRemoteIp() const
{
    return m_RemoteIp;
}

void Endpoint::setLocalPort(uint16_t port)
{
    m_LocalPort = port;
}

void Endpoint::setRemotePort(uint16_t port)
{
    m_RemotePort = port;
}

void Endpoint::setLocalIp(const IpAddress &local)
{
    m_LocalIp = local;
}

void Endpoint::setRemoteIp(const IpAddress &remote)
{
    m_RemoteIp = remote;
}

bool Endpoint::dataReady(bool block, uint32_t timeout)
{
    return false;
}

size_t Endpoint::depositPayload(size_t nBytes, uintptr_t payload, RemoteEndpoint remoteHost)
{
    return 0;
}

void Endpoint::setCard(Network* pCard)
{
}

ProtocolManager *Endpoint::getManager() const
{
    return m_Manager;
}

void Endpoint::setManager(ProtocolManager *man)
{
    m_Manager = man;
}

bool Endpoint::isConnectionless() const
{
    return !m_bConnection;
}

void Endpoint::AddSocket(Socket *s)
{
    m_Sockets.pushBack(s);
}

void Endpoint::RemoveSocket(Socket *s)
{
    for(List<Socket*>::Iterator it = m_Sockets.begin(); it != m_Sockets.end(); ++it)
    {
        if(*it == s)
        {
            m_Sockets.erase(it);
            return;
        }
    }
}

Error::PosixError Endpoint::getError() const
{
    return m_Error;
}

void Endpoint::resetError()
{
    m_Error = Error::NoError;
}

void Endpoint::reportError(Error::PosixError e)
{
    m_Error = e;
}

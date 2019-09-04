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

#ifndef CDI_CPP_NET_H
#define CDI_CPP_NET_H

#include <stdbool.h>
#include "cdi.h"
#include "cdi/net.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

/** CDI NIC Device */
class CdiNet : public Network
{
    public:
        CdiNet(struct cdi_net_device* device);
        CdiNet(Network* pDev, struct cdi_net_device* device);
        ~CdiNet();

        virtual void getName(String &str)
        {
            if((!m_Device) || (!m_Device->dev.name))
                str.assign("cdi-net", 8);
            else
            {
                str.assign(m_Device->dev.name);
            }
        }

        virtual bool send(size_t nBytes, uintptr_t buffer);

        virtual bool setStationInfo(const StationInfo &info);
        virtual const StationInfo &getStationInfo();

        const struct cdi_net_device *getCdiDevice() const
        {
            return m_Device;
        }

    private:
        CdiNet(const CdiNet&);
        const CdiNet & operator = (const CdiNet&);

        struct cdi_net_device* m_Device;
};

#endif

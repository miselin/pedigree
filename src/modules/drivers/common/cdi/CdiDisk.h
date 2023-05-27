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

#ifndef CDI_CPP_DISK_H
#define CDI_CPP_DISK_H

#include <stdbool.h>
#include "cdi.h"
#include "cdi/storage.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/String.h"

/** CDI Disk Device */
class CdiDisk : public Disk
{
    public:
        CdiDisk(struct cdi_storage_device *device);
        CdiDisk(Disk* pDev, struct cdi_storage_device* device);
        virtual ~CdiDisk();

        virtual void getName(String &str)
        {
            if((!m_Device) || (!m_Device->dev.name))
                str.assign("cdi-disk", 8);
            else
            {
                str.assign(m_Device->dev.name);
            }
        }

        /** Tries to detect if this device is present.
         * \return True if the device is present and was successfully initialised. */
        bool initialise();

        // These are the functions that others call - they add a request to the parent controller's queue.
        virtual uintptr_t read(uint64_t location);
        virtual void write(uint64_t location);

        /// Assume CDI-provided disks are never read-only.
        virtual bool cacheIsCritical()
        {
            return false;
        }

        /// CDI disks do not yet do any form of caching.
        /// \todo Fix that.
        virtual void flush(uint64_t location)
        {
            return;
        }

    private:
        CdiDisk(const CdiDisk&);
        const CdiDisk & operator = (const CdiDisk&);

        struct cdi_storage_device* m_Device;
        Cache m_Cache;
};

#endif

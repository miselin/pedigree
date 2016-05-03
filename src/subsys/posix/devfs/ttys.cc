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

#include <console/TextIO.h>
#include <console/Console.h>
#include <utilities/assert.h>

#include <DevFs.h>

void createPtyNodes(Filesystem *fs, DevFsDirectory *root, size_t &baseInode)
{
    // Create nodes for psuedoterminals.
    const char *ptyletters = "pqrstuvwxyzabcde";
    for(size_t i = 0; i < 16; ++i)
    {
        for(const char *ptyc = ptyletters; *ptyc != 0; ++ptyc)
        {
            const char c = *ptyc;
            char a = 'a' + (i % 10);
            if(i <= 9)
                a = '0' + i;
            char master[] = {'p', 't', 'y', c, a, 0};
            char slave[] = {'t', 't', 'y', c, a, 0};

            String masterName(master), slaveName(slave);

            File *pMaster = ConsoleManager::instance().getConsole(masterName);
            File *pSlave = ConsoleManager::instance().getConsole(slaveName);
            assert(pMaster && pSlave);

            root->addEntry(masterName, pMaster);
            root->addEntry(slaveName, pSlave);
        }
    }

    // Create /dev/textui for the text-only UI device.
    TextIO *pTty = new TextIO(String("textui"), ++baseInode, fs, root);
    if(pTty->initialise(false))
    {
        root->addEntry(pTty->getName(), pTty);
    }
    else
    {
        WARNING("POSIX: no /dev/textui - TextIO failed to initialise.");
        --baseInode;
        delete pTty;
    }
}

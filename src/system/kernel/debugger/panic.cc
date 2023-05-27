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

#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/debugger/DebuggerIO.h"
#include "pedigree/kernel/debugger/LocalIO.h"
#include "pedigree/kernel/debugger/SerialIO.h"
#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

static size_t newlineCount(const char *pString)
{
    size_t nNewlines = 0;
    while (*pString != '\0')
        if (*pString++ == '\n')
            ++nNewlines;

    return nNewlines;
}

// TODO: We might want a separate parameter for a stacktrace/register dump
static void _panic(const char *msg, DebuggerIO *pScreen)
{
    static HugeStaticString panic_output;
    panic_output.clear();

    panic_output.append("PANIC: ");
    panic_output.append(msg);

    // write the final string to the screen
    pScreen->drawString(panic_output, 0, 0, DebuggerIO::Red, DebuggerIO::Black);

    size_t nLines = newlineCount(panic_output) + 2;

    Log &log = Log::instance();
    Log::SeverityLevel level;
    static NormalStaticString Line;

    size_t iEntry = 0, iUsedEntries = 0;
    if ((pScreen->getHeight() - nLines) <
        (log.getStaticEntryCount() + log.getDynamicEntryCount()))
        iEntry = log.getStaticEntryCount() + log.getDynamicEntryCount() -
                 (pScreen->getHeight() - nLines) + 1;
    bool bPrintThisLine = false;
    for (; iEntry < (log.getStaticEntryCount() + log.getDynamicEntryCount());
         iEntry++)
    {
        if (iEntry < log.getStaticEntryCount())
        {
            const Log::StaticLogEntry &entry = log.getStaticEntry(iEntry);

            // level = entry.severity;
            // if( level == Log::Fatal || level == Log::Error )
            // {
            Line.clear();
            Line.append("[");
            Line.append(entry.timestamp, 10, 8, '0');
            Line.append("] ");
            Line.append(entry.str);
            Line.append("\n");

            bPrintThisLine = true;
            // }
        }
        else
        {
            const Log::DynamicLogEntry &entry = log.getDynamicEntry(iEntry);
            level = entry.severity;

            //      if( level == Log::Fatal || level == Log::Error )
            //      {
            Line.clear();
            Line.append("[");
            Line.append(entry.timestamp, 10, 8, '0');
            Line.append("] ");
            Line.append(entry.str);
            Line.append("\n");

            bPrintThisLine = true;
            //      }
        }

        // print the line
        if (bPrintThisLine == true)
        {
            ++iUsedEntries;
            pScreen->drawString(
                Line, nLines + iUsedEntries, 0, DebuggerIO::White,
                DebuggerIO::Black);
            bPrintThisLine = false;
        }
    }
}

void panic(const char *msg)
{
    static String graphicsService("graphics");

    Processor::setInterrupts(false);

    // Drop out of whatever graphics mode we were in
    GraphicsService::GraphicsProvider provider;
    ByteSet(&provider, 0, sizeof(provider));
    provider.bTextModes = true;

    ServiceFeatures *pFeatures =
        ServiceManager::instance().enumerateOperations(graphicsService);
    Service *pService = ServiceManager::instance().getService(graphicsService);
    bool bSuccess = false;
    if (pFeatures && pFeatures->provides(ServiceFeatures::probe))
        if (pService)
            bSuccess = pService->serve(
                ServiceFeatures::probe, reinterpret_cast<void *>(&provider),
                sizeof(provider));

    if (bSuccess && !provider.bTextModes)
        provider.pDisplay->setScreenMode(0);

#if MULTIPROCESSOR
    Machine::instance().stopAllOtherProcessors();
#endif

    /*
     * I/O implementations.
     */
    SerialIO serialIO(Machine::instance().getSerial(0));

    DebuggerIO *pInterfaces[2] = {0};

    int nInterfaces = 0;
    if (Machine::instance()
            .getNumVga())  // Not all machines have "VGA", so handle that
    {
        static LocalIO localIO(
            Machine::instance().getVga(0), Machine::instance().getKeyboard());
#if DONT_LOG_TO_SERIAL
        pInterfaces[0] = &localIO;
        nInterfaces = 1;
#else
        pInterfaces[0] = &localIO;
        pInterfaces[1] = &serialIO;
        nInterfaces = 2;
#endif
    }
#if !DONT_LOG_TO_SERIAL
    else
    {
        pInterfaces[0] = &serialIO;
        nInterfaces = 1;
    }
#endif

    for (int nIFace = 0; nIFace < nInterfaces; nIFace++)
        _panic(msg, pInterfaces[nIFace]);

    // Halt the processor
    while (1)
        Processor::halt();
}

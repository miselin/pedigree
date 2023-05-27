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
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"
#include "sqlite3/sqlite3.h"

extern BootstrapStruct_t *g_pBootstrapInfo;

sqlite3 *g_pSqlite = 0;

static uint8_t *g_pFile = 0;
static size_t g_FileSz = 0;

extern "C" {
void log_(unsigned long a);
int atoi(const char *str);
}

extern "C" void log_(unsigned long a)
{
    NOTICE("Int: " << a);
}

extern "C" int atoi(const char *str)
{
    return StringToUnsignedLong(str, 0, 10);
}

extern "C" struct tm *gmtime(struct tm *timep)
{
    return 0;
}

extern "C" size_t
strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
    return 0;
}

static int xClose(sqlite3_file *file)
{
    return 0;
}

static int xRead(sqlite3_file *file, void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    int ret = 0;
    if ((static_cast<size_t>(iOfst + iAmt)) >= g_FileSz)
    {
        if (static_cast<size_t>(iAmt) > g_FileSz)
            iAmt = g_FileSz;
        ByteSet(ptr, 0, iAmt);
        iAmt = g_FileSz - iOfst;
        ret = SQLITE_IOERR_SHORT_READ;
    }
    MemoryCopy(ptr, &g_pFile[iOfst], iAmt);
    return ret;
}

static int
xReadFail(sqlite3_file *file, void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    ByteSet(ptr, 0, iAmt);
    return SQLITE_IOERR_SHORT_READ;
}

static int
xWrite(sqlite3_file *file, const void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    // Write past the end of the file?
    if (static_cast<size_t>(iOfst + iAmt) >= g_FileSz)
    {
        /// \todo figure out some sort of error to return?
        return SQLITE_IOERR_WRITE;
    }

    MemoryCopy(&g_pFile[iOfst], ptr, iAmt);
    return 0;
}

static int
xWriteFail(sqlite3_file *file, const void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    return 0;
}

static int xTruncate(sqlite3_file *file, sqlite3_int64 size)
{
    return 0;
}

static int xSync(sqlite3_file *file, int flags)
{
    return 0;
}

static int xFileSize(sqlite3_file *file, sqlite3_int64 *pSize)
{
    *pSize = g_FileSz;
    return 0;
}

static int xLock(sqlite3_file *file, int a)
{
    return 0;
}

static int xUnlock(sqlite3_file *file, int a)
{
    return 0;
}

static int xCheckReservedLock(sqlite3_file *file, int *pResOut)
{
    *pResOut = 0;
    return 0;
}

static int xFileControl(sqlite3_file *file, int op, void *pArg)
{
    return 0;
}

static int xSectorSize(sqlite3_file *file)
{
    return 1;
}

static int xDeviceCharacteristics(sqlite3_file *file)
{
    return 0;
}

static struct sqlite3_io_methods theio = {1,
                                          &xClose,
                                          &xRead,
                                          &xWrite,
                                          &xTruncate,
                                          &xSync,
                                          &xFileSize,
                                          &xLock,
                                          &xUnlock,
                                          &xCheckReservedLock,
                                          &xFileControl,
                                          &xSectorSize,
                                          &xDeviceCharacteristics,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0};

static struct sqlite3_io_methods theio_fail = {1,
                                               &xClose,
                                               &xReadFail,
                                               &xWriteFail,
                                               &xTruncate,
                                               &xSync,
                                               &xFileSize,
                                               &xLock,
                                               &xUnlock,
                                               &xCheckReservedLock,
                                               &xFileControl,
                                               &xSectorSize,
                                               &xDeviceCharacteristics,
                                               0,
                                               0,
                                               0,
                                               0,
                                               0,
                                               0};

static int xOpen(
    sqlite3_vfs *vfs, const char *zName, sqlite3_file *file, int flags,
    int *pOutFlags)
{
    if (StringCompare(zName, "root»/.pedigree-root"))
    {
        // Assume journal file, return failure functions.
        file->pMethods = &theio_fail;
        return 0;
    }

    if (!g_pBootstrapInfo->isDatabaseLoaded())
    {
        FATAL("Config database not loaded!");
    }

    file->pMethods = &theio;
    return 0;
}

static int xDelete(sqlite3_vfs *vfs, const char *zName, int syncDir)
{
    return 0;
}

static int xAccess(sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut)
{
    return 0;
}

static int
xFullPathname(sqlite3_vfs *vfs, const char *zName, int nOut, char *zOut)
{
    StringCopyN(zOut, zName, nOut);
    return 0;
}

static void *xDlOpen(sqlite3_vfs *vfs, const char *zFilename)
{
    return 0;
}

static void xDlError(sqlite3_vfs *vfs, int nByte, char *zErrMsg)
{
}

static void (*xDlSym(sqlite3_vfs *vfs, void *p, const char *zSymbol))(void)
{
    return 0;
}

static void xDlClose(sqlite3_vfs *vfs, void *v)
{
}

static int xRandomness(sqlite3_vfs *vfs, int nByte, char *zOut)
{
    return 0;
}

static int xSleep(sqlite3_vfs *vfs, int microseconds)
{
    return 0;
}

static int xCurrentTime(sqlite3_vfs *vfs, sqlite3_int64 *)
{
    return 0;
}

static int xGetLastError(sqlite3_vfs *vfs, int i, char *c)
{
    return 0;
}

static struct sqlite3_vfs thevfs = {1,
                                    sizeof(void *),
                                    32,
                                    0,
                                    "no-vfs",
                                    0,
                                    &xOpen,
                                    &xDelete,
                                    &xAccess,
                                    &xFullPathname,
                                    &xDlOpen,
                                    &xDlError,
                                    &xDlSym,
                                    &xDlClose,
                                    &xRandomness,
                                    &xSleep,
                                    &xCurrentTime,
                                    &xGetLastError,
                                    0,
                                    0,
                                    0,
                                    0};

int sqlite3_os_init()
{
    sqlite3_vfs_register(&thevfs, 1);
    return 0;
}

int sqlite3_os_end()
{
    return 0;
}

static void xCallback0(sqlite3_context *context, int n, sqlite3_value **values)
{
    const unsigned char *text = sqlite3_value_text(values[0]);

    if (!text)
        return;

    uintptr_t x;
    if (text[0] == '0')
    {
        x = StringToUnsignedLong(reinterpret_cast<const char *>(text), 0, 16);
    }
    else
    {
        x = KernelElf::instance().lookupSymbol(
            reinterpret_cast<const char *>(text));
        if (!x)
        {
            ERROR(
                "Couldn't trigger callback `"
                << reinterpret_cast<const char *>(text)
                << "': symbol not found.");
            return;
        }
    }

    void (*func)(void) = reinterpret_cast<void (*)(void)>(x);
    func();
    sqlite3_result_int(context, 0);
}

static void xCallback1(sqlite3_context *context, int n, sqlite3_value **values)
{
    const char *text =
        reinterpret_cast<const char *>(sqlite3_value_text(values[0]));

    if (!text)
        return;

    uintptr_t x;
    if (text[0] == '0')
    {
        x = StringToUnsignedLong(text, 0, 16);
    }
    else
    {
        x = KernelElf::instance().lookupSymbol(text);
        if (!x)
        {
            ERROR(
                "Couldn't trigger callback `" << text
                                              << "': symbol not found.");
            return;
        }
    }

    void (*func)(const char *) = reinterpret_cast<void (*)(const char *)>(x);
    func(reinterpret_cast<const char *>(sqlite3_value_text(values[1])));
    sqlite3_result_int(context, 0);
}

static void xCallback2(sqlite3_context *context, int n, sqlite3_value **values)
{
    const char *text =
        reinterpret_cast<const char *>(sqlite3_value_text(values[0]));

    if (!text)
        return;

    uintptr_t x;
    if (text[0] == '0')
    {
        x = StringToUnsignedLong(text, 0, 16);
    }
    else
    {
        x = KernelElf::instance().lookupSymbol(text);
        if (!x)
        {
            ERROR(
                "Couldn't trigger callback `" << text
                                              << "': symbol not found.");
            return;
        }
    }

    void (*func)(const char *, const char *) =
        reinterpret_cast<void (*)(const char *, const char *)>(x);
    func(
        reinterpret_cast<const char *>(sqlite3_value_text(values[1])),
        reinterpret_cast<const char *>(sqlite3_value_text(values[2])));
    sqlite3_result_int(context, 0);
}

#if STATIC_DRIVERS
#include "config_database.h"
#else
static uint8_t file[0] = {};
#endif

// Memory region containing the config database. Not used if static drivers are
// being used, but used in all other cases.
MemoryRegion region("Config");

// Entry point for --gc-sections.
static bool init()
{
    EMIT_IF(!STATIC_DRIVERS)
    {
        if (!g_pBootstrapInfo->isDatabaseLoaded())
            FATAL("Database not loaded, cannot continue.");

        uint8_t *pPhys = g_pBootstrapInfo->getDatabaseAddress();
        size_t sSize = g_pBootstrapInfo->getDatabaseSize();

        if ((reinterpret_cast<physical_uintptr_t>(pPhys) &
             (PhysicalMemoryManager::getPageSize() - 1)) != 0)
            panic("Config: Alignment issues");

        EMIT_IF(HOSTED)
        {
            g_pFile = new uint8_t[sSize];
            MemoryCopy(g_pFile, pPhys, sSize);
            g_FileSz = sSize;
        }
        else
        {
            if (PhysicalMemoryManager::instance().allocateRegion(
                    region,
                    (sSize + PhysicalMemoryManager::getPageSize() - 1) /
                        PhysicalMemoryManager::getPageSize(),
                    PhysicalMemoryManager::continuous, VirtualAddressSpace::KernelMode,
                    reinterpret_cast<physical_uintptr_t>(pPhys)) == false)
            {
                ERROR("Config: allocateRegion failed.");
                return false;
            }

            g_pFile = reinterpret_cast<uint8_t *>(region.virtualAddress());
            g_FileSz = sSize;
        }
    }
    else
    {
        g_pFile = file;
        g_FileSz = sizeof file;
    }

    sqlite3_initialize();
    int ret = sqlite3_open("root»/.pedigree-root", &g_pSqlite);
    if (ret)
    {
        FATAL("sqlite3 error: " << sqlite3_errmsg(g_pSqlite));
    }

    sqlite3_create_function(
        g_pSqlite, "pedigree_callback", 1, SQLITE_ANY, 0, &xCallback0, 0, 0);
    sqlite3_create_function(
        g_pSqlite, "pedigree_callback", 2, SQLITE_ANY, 0, &xCallback1, 0, 0);
    sqlite3_create_function(
        g_pSqlite, "pedigree_callback", 3, SQLITE_ANY, 0, &xCallback2, 0, 0);

    return true;
}

static void destroy()
{
    // Shut down sqlite, cleaning up the opened file along the way.
    sqlite3_close(g_pSqlite);
    sqlite3_shutdown();

    region.free();
}

MODULE_INFO("config", &init, &destroy);

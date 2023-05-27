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

#ifndef DEVFS_H
#define DEVFS_H

#include "pedigree/kernel/Log.h"

#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"

#include "modules/system/console/TextIO.h"

#include "pedigree/kernel/graphics/Graphics.h"
#include "pedigree/kernel/graphics/GraphicsService.h"

#include "modules/subsys/posix/PsAuxFile.h"
#include "modules/subsys/posix/VirtualTerminal.h"

#define DEVFS_NUMTTYS 7

class DevFs;
class DevFsDirectory;

extern DevFs *g_pDevFs;

class RandomFile : public File
{
  public:
    RandomFile(String str, size_t inode, Filesystem *pParentFS, File *pParent)
        : File(str, 0, 0, 0, inode, pParentFS, 0, pParent)
    {
        setPermissionsOnly(
            FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
        setUidOnly(0);
        setGidOnly(0);
    }
    ~RandomFile()
    {
    }

    virtual uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

  private:
    virtual bool isBytewise() const
    {
        return true;
    }
};

class NullFile : public File
{
  public:
    NullFile(String str, size_t inode, Filesystem *pParentFS, File *pParentNode)
        : File(str, 0, 0, 0, inode, pParentFS, 0, pParentNode)
    {
        setPermissionsOnly(
            FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
        setUidOnly(0);
        setGidOnly(0);
    }
    ~NullFile()
    {
    }

    virtual uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

  private:
    virtual bool isBytewise() const
    {
        return true;
    }
};

class ZeroFile : public File
{
  public:
    ZeroFile(String str, size_t inode, Filesystem *pParentFS, File *pParentNode)
        : File(str, 0, 0, 0, inode, pParentFS, 0, pParentNode)
    {
        setPermissionsOnly(
            FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
        setUidOnly(0);
        setGidOnly(0);
    }
    ~ZeroFile()
    {
    }

    virtual uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

  private:
    virtual bool isBytewise() const
    {
        return true;
    }
};

class PtmxFile : public File
{
  public:
    PtmxFile(
        String str, size_t inode, Filesystem *pParentFS, File *pParent,
        DevFsDirectory *m_pPtsDirectory);
    ~PtmxFile();

    virtual uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    // override open() to correctly handle returning a master and creating
    // the associated slave.
    virtual File *open();

  private:
    ExtensibleBitmap m_Terminals;
    DevFsDirectory *m_pPtsDirectory;

    virtual bool isBytewise() const
    {
        return true;
    }
};

class RtcFile : public File
{
  public:
    RtcFile(size_t inode, Filesystem *pParentFS, File *pParentNode)
        : File(String("rtc"), 0, 0, 0, inode, pParentFS, 0, pParentNode)
    {
        setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR);
        setUidOnly(0);
        setGidOnly(0);
    }
    ~RtcFile()
    {
    }

    virtual uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual bool supports(const size_t command) const;
    virtual int command(const size_t command, void *buffer);

  private:
    virtual bool isBytewise() const
    {
        return true;
    }
};

class FramebufferFile : public File
{
  public:
    FramebufferFile(
        String str, size_t inode, Filesystem *pParentFS, File *pParentNode);
    ~FramebufferFile();

    bool initialise();

    virtual uintptr_t readBlock(uint64_t location);

    virtual bool supports(const size_t command) const;
    virtual int command(const size_t command, void *buffer);

    /// \todo pinBlock/unpinBlock should pin/unpin physical pages!

  private:
    GraphicsService::GraphicsParameters *m_pGraphicsParameters;

    bool m_bTextMode;
    size_t m_nDepth;
};

class Tty0File : public File
{
  public:
    Tty0File(
        String str, size_t inode, Filesystem *pParentFS, File *pParent,
        DevFs *devfs);
    ~Tty0File();

    virtual uint64_t readBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t writeBytewise(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    // override open() to correctly handle returning a master and creating
    // the associated slave.
    virtual File *open();

  private:
    DevFs *m_pDevFs;

    virtual bool isBytewise() const
    {
        return true;
    }
};

class MemFile : public File
{
  public:
    MemFile(String name, size_t inode, Filesystem *pParentFS, File *pParentNode)
        : File(name, 0, 0, 0, inode, pParentFS, 0, pParentNode)
    {
        setPermissionsOnly(
            FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
        setUidOnly(0);
        setGidOnly(0);
    }
    ~MemFile()
    {
    }

    virtual physical_uintptr_t getPhysicalPage(size_t offset);
    virtual void returnPhysicalPage(size_t offset);
};

/** This class provides slightly more flexibility for adding files to a
 * directory. */
class DevFsDirectory : public Directory
{
  public:
    DevFsDirectory(
        String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
        Time::Timestamp creationTime, uintptr_t inode, class Filesystem *pFs,
        size_t size, File *pParent)
        : Directory(
              name, accessedTime, modifiedTime, creationTime, inode, pFs, size,
              pParent)
    {
    }

    virtual ~DevFsDirectory();

    void addEntry(String name, File *pFile)
    {
        addDirectoryEntry(name, pFile);
    }
};

/** This class provides /dev */
class DevFs : public Filesystem
{
  public:
    DevFs() : m_pRoot(0), m_pTty(0), m_VtManager(0), m_pPsAuxFile(0)
    {
    }

    virtual ~DevFs();

    virtual bool initialise(Disk *pDisk);

    virtual File *getRoot() const
    {
        return m_pRoot;
    }
    virtual const String &getVolumeLabel() const
    {
        static String devfsLabel("dev");
        return devfsLabel;
    }

    virtual size_t getNextInode();
    virtual void revertInode();

    void handleInput(InputManager::InputNotification &in);

    VirtualTerminalManager &getTerminalManager()
    {
        return *m_VtManager;
    }

  protected:
    virtual bool createFile(File *parent, const String &filename, uint32_t mask)
    {
        return false;
    }
    virtual bool
    createDirectory(File *parent, const String &filename, uint32_t mask)
    {
        return false;
    }
    virtual bool
    createSymlink(File *parent, const String &filename, const String &value)
    {
        return false;
    }
    virtual bool remove(File *parent, File *file)
    {
        return false;
    }

  private:
    DevFs(const DevFs &);
    DevFs &operator=(const DevFs &);

    DevFsDirectory *m_pRoot;

    TextIO *m_pTty;

    size_t m_NextInode;

    TextIO *m_pTtys[DEVFS_NUMTTYS];
    File *m_pTtyFiles[DEVFS_NUMTTYS];
    size_t m_CurrentTty;

    VirtualTerminalManager *m_VtManager;

    PsAuxFile *m_pPsAuxFile;
};

#endif

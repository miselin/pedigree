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

#include <Log.h>

#include <machine/InputManager.h>
#include <utilities/ExtensibleBitmap.h>
#include <vfs/Directory.h>
#include <vfs/File.h>
#include <vfs/Filesystem.h>

#include <console/TextIO.h>

#include <graphics/Graphics.h>
#include <graphics/GraphicsService.h>

class DevFs;
class DevFsDirectory;

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

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
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

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
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

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
};

class PtmxFile : public File
{
  public:
    PtmxFile(
        String str, size_t inode, Filesystem *pParentFS, File *pParent,
        DevFsDirectory *m_pPtsDirectory);
    ~PtmxFile();

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    // override open() to correctly handle returning a master and creating
    // the associated slave.
    virtual File *open();

  private:
    ExtensibleBitmap m_Terminals;
    DevFsDirectory *m_pPtsDirectory;
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

    virtual uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual bool supports(const int command);
    virtual int command(const int command, void *buffer);
};

class FramebufferFile : public File
{
  public:
    FramebufferFile(
        String str, size_t inode, Filesystem *pParentFS, File *pParentNode);
    ~FramebufferFile();

    bool initialise();

    virtual uintptr_t readBlock(uint64_t location);

    virtual bool supports(const int command);
    virtual int command(const int command, void *buffer);

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

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    // override open() to correctly handle returning a master and creating
    // the associated slave.
    virtual File *open();

  private:
    DevFs *m_pDevFs;
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

    virtual ~DevFsDirectory()
    {
    }

    void addEntry(String name, File *pFile)
    {
        addDirectoryEntry(name, pFile);
    }
};

/** This class provides /dev */
class DevFs : public Filesystem
{
  public:
    DevFs() : m_pRoot(0), m_pTty(0)
    {
    }

    virtual ~DevFs();

    virtual bool initialise(Disk *pDisk);

    virtual File *getRoot()
    {
        return m_pRoot;
    }
    virtual String getVolumeLabel()
    {
        return String("dev");
    }

    virtual size_t getNextInode();
    virtual void revertInode();

    void handleInput(InputManager::InputNotification &in);

    TextIO *getCurrentTty() const;
    File *getCurrentTtyFile() const;

  protected:
    virtual bool createFile(File *parent, String filename, uint32_t mask)
    {
        return false;
    }
    virtual bool createDirectory(File *parent, String filename, uint32_t mask)
    {
        return false;
    }
    virtual bool createSymlink(File *parent, String filename, String value)
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

    TextIO *m_pTtys[7];
    File *m_pTtyFiles[7];
    size_t m_CurrentTty;
};

#endif

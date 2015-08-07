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

#include <syscallError.h>
#include <processor/types.h>
#include <processor/Processor.h>
#include <processor/MemoryRegion.h>
#include <processor/PhysicalMemoryManager.h>
#include <processor/VirtualAddressSpace.h>
#include <process/Process.h>
#include <utilities/Tree.h>
#include <vfs/File.h>
#include <vfs/LockedFile.h>
#include <vfs/MemoryMappedFile.h>
#include <vfs/Symlink.h>
#include <vfs/Directory.h>
#include <vfs/VFS.h>
#include <console/Console.h>
#include <network-stack/NetManager.h>
#include <network-stack/Tcp.h>
#include <utilities/utility.h>
#include <machine/Disk.h>

#include <Subsystem.h>
#include <PosixSubsystem.h>

#include "file-syscalls.h"
#include "console-syscalls.h"
#include "pipe-syscalls.h"
#include "net-syscalls.h"

extern int posix_getpid();

//
// Syscalls pertaining to files.
//

#define GET_CWD() (Processor::information().getCurrentThread()->getParent()->getCwd())

inline File *traverseSymlink(File *file)
{
    if(!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return 0;
    }

    Tree<File*, File*> loopDetect;
    while(file->isSymlink())
    {
        file = Symlink::fromFile(file)->followLink();
        if(!file)
        {
            SYSCALL_ERROR(DoesNotExist);
            return 0;
        }

        if(loopDetect.lookup(file))
        {
            SYSCALL_ERROR(LoopExists);
            return 0;
        }
        else
            loopDetect.insert(file, file);
    }

    return file;
}

inline String normalisePath(const char *name, bool *onDevFs = 0)
{
    // Rebase /dev onto the devfs. /dev/tty is special.
    String nameToOpen;
    if (!strcmp(name, "/dev/tty"))
    {
        // Get controlling console, unless we have none.
        Process *pProcess = Processor::information().getCurrentThread()->getParent();
        if (!pProcess->getCtty())
        {
            nameToOpen = name;
            if (onDevFs)
                *onDevFs = true;
        }
        else
        {
            nameToOpen = name;
        }
    }
    else if (!strncmp(name, "/dev", strlen("/dev")))
    {
        nameToOpen = "dev»";
        nameToOpen += (name + strlen("/dev"));
        if (onDevFs)
            *onDevFs = true;
    }
    else if (!strncmp(name, "/bin", strlen("/bin")))
    {
        nameToOpen = "/applications";
        nameToOpen += (name + strlen("/bin"));
    }
    else if (!strncmp(name, "/etc", strlen("/etc")))
    {
        nameToOpen = "/config";
        nameToOpen += (name + strlen("/etc"));
    }
    else if (!strncmp(name, "/tmp", strlen("/tmp")))
    {
        nameToOpen = "scratch»";
        nameToOpen += (name + strlen("/tmp"));
    }
    else if (!strncmp(name, "/var/run", strlen("/var/run")))
    {
        nameToOpen = "runtime»";
        nameToOpen += (name + strlen("/var/run"));
    }
    else
    {
        nameToOpen = name;
    }

    return nameToOpen;
}

int posix_close(int fd)
{
    F_NOTICE("close(" << fd << ")");
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    // If this was a master psuedoterminal, we should unlock it now.
    if(ConsoleManager::instance().isConsole(pFd->file))
    {
        if(ConsoleManager::instance().isMasterConsole(pFd->file))
        {
            ConsoleManager::instance().unlockConsole(pFd->file);
        }
    }

    pSubsystem->freeFd(fd);
    return 0;
}

int posix_open(const char *name, int flags, int mode)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), PATH_MAX, PosixSubsystem::SafeRead))
    {
        F_NOTICE("open -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("open(" << name << ", " << ((flags & O_RDWR) ? "O_RDWR" : "") << ((flags & O_RDONLY) ? "O_RDONLY" : "") << ((flags & O_WRONLY) ? "O_WRONLY" : "") << ")");
    F_NOTICE("  -> actual flags " << flags);
    
    // One of these three must be specified
    /// \bug Breaks /dev/tty open in crt0
#if 0
    if(!(flags & (O_RDONLY | O_WRONLY | O_RDWR)))
    {
        F_NOTICE("One of O_RDONLY, O_WRONLY, or O_RDWR must be passed.");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }
#endif

    // verify the filename - don't try to open a dud file
    if (name[0] == 0)
    {
        F_NOTICE("File does not exist (null path).");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    size_t fd = pSubsystem->getFd();

    File* file = 0;

    bool onDevFs = false;
    String nameToOpen = normalisePath(name, &onDevFs);
    if (nameToOpen == "/dev/tty")
    {
        file = pProcess->getCtty();
        if(!file)
        {
            F_NOTICE(" -> returning -1, no controlling tty");
            return -1;
        }
        else if(ConsoleManager::instance().isMasterConsole(file))
        {
            // If we happened to somehow open a master console, get its slave.
            F_NOTICE(" -> controlling terminal was not a slave");
            file = ConsoleManager::instance().getOther(file);
        }
    }

    F_NOTICE("  -> actual filename to open is '" << nameToOpen << "'");

    if (!file)
    {
        // Find file.
        file = VFS::instance().find(nameToOpen, GET_CWD());
    }

    bool bCreated = false;
    if (!file)
    {
        if ((flags & O_CREAT) && !onDevFs)
        {
            F_NOTICE("  {O_CREAT}");
            bool worked = VFS::instance().createFile(nameToOpen, 0777, GET_CWD());
            if (!worked)
            {
                F_NOTICE("File does not exist (createFile failed)");
                SYSCALL_ERROR(DoesNotExist);
                pSubsystem->freeFd(fd);
                return -1;
            }

            file = VFS::instance().find(nameToOpen, GET_CWD());
            if (!file)
            {
                F_NOTICE("File does not exist (O_CREAT failed)");
                SYSCALL_ERROR(DoesNotExist);
                pSubsystem->freeFd(fd);
                return -1;
            }

            bCreated = true;
        }
        else
        {
            F_NOTICE("Does not exist.");
            // Error - not found.
            SYSCALL_ERROR(DoesNotExist);
            pSubsystem->freeFd(fd);
            return -1;
        }
    }

    if(!file)
    {
      F_NOTICE("File does not exist.");
      SYSCALL_ERROR(DoesNotExist);
      pSubsystem->freeFd(fd);
      return -1;
    }

    file = traverseSymlink(file);

    if(!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        pSubsystem->freeFd(fd);
        return -1;
    }

    if (file->isDirectory() && (flags & (O_WRONLY | O_RDWR)))
    {
        // Error - is directory.
        F_NOTICE("Is a directory, and O_WRONLY or O_RDWR was specified.");
        SYSCALL_ERROR(IsADirectory);
        pSubsystem->freeFd(fd);
        return -1;
    }

    if ((flags & O_CREAT) && (flags & O_EXCL) && !bCreated)
    {
        // file exists with O_CREAT and O_EXCL
        F_NOTICE("File exists");
        SYSCALL_ERROR(FileExists);
        pSubsystem->freeFd(fd);
        return -1;
    }

    // Check for console (as we have special handling needed here)
    if (ConsoleManager::instance().isConsole(file))
    {
        // If a master console, attempt to lock.
        if(ConsoleManager::instance().isMasterConsole(file))
        {
            // Lock the master, we now own it.
            // Or, we don't - if someone else has it open for example.
            if(!ConsoleManager::instance().lockConsole(file))
            {
                F_NOTICE("Couldn't lock pseudoterminal master");
                SYSCALL_ERROR(DeviceBusy);
                pSubsystem->freeFd(fd);
                return -1;
            }
        }
    }

    if ((flags & O_TRUNC) && ((flags & O_CREAT) || (flags & O_WRONLY) || (flags & O_RDWR)))
    {
        F_NOTICE("  {O_TRUNC}");
        // truncate the file
        file->truncate();
    }

    FileDescriptor *f = new FileDescriptor(file, (flags & O_APPEND) ? file->getSize() : 0, fd, 0, flags);
    if(f)
        pSubsystem->addFileDescriptor(fd, f);

    F_NOTICE("    -> " << fd);

    return static_cast<int> (fd);
}

int posix_read(int fd, char *ptr, int len)
{
    F_NOTICE("read(" << Dec << fd << Hex << ", " << reinterpret_cast<uintptr_t>(ptr) << ", " << len << ")");
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ptr), len, PosixSubsystem::SafeWrite))
    {
        F_NOTICE("  -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    
    if(pFd->file->isDirectory())
    {
        SYSCALL_ERROR(IsADirectory);
        return -1;
    }

    // Are we allowed to block?
    bool canBlock = !((pFd->flflags & O_NONBLOCK) == O_NONBLOCK);

    // Handle async descriptor that is not ready for reading.
    // File::read has no mechanism for presenting such an error, other than
    // returning 0. However, a read() returning 0 is an EOF condition.
    if(!canBlock)
    {
        if(!pFd->file->select(false, 0))
        {
            SYSCALL_ERROR(NoMoreProcesses);
            return -1;
        }
    }

    // Prepare to handle EINTR.
    Thread *pThread = Processor::information().getCurrentThread();

    uint64_t nRead = 0;
    if (ptr && len)
    {
        /// \todo Sanitise input and check it's mapped etc so we don't segfault the kernel
        pThread->setInterrupted(false);
        nRead = pFd->file->read(pFd->offset, len, len > 0x500000 ? 0 : reinterpret_cast<uintptr_t>(ptr), canBlock);
        if((!nRead) && (pThread->wasInterrupted()))
        {
            SYSCALL_ERROR(Interrupted);
            return -1;
        }
        pFd->offset += nRead;
    }

    F_NOTICE("    -> " << Dec << nRead << Hex);

    return static_cast<int>(nRead);
}

int posix_write(int fd, char *ptr, int len, bool nocheck)
{
    F_NOTICE("write(" << fd << ", " << reinterpret_cast<uintptr_t>(ptr) << ", " << len << ")");
    if(!nocheck && !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ptr), len, PosixSubsystem::SafeRead))
    {
        F_NOTICE("  -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if(ptr)
        F_NOTICE("write(" << fd << ", " << ptr << ", " << len << ")");

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    // Copy to kernel.
    uint64_t nWritten = 0;
    if (ptr && len)
    {
        /// \todo Sanitise input and check it's mapped etc so we don't segfault the kernel
        nWritten = pFd->file->write(pFd->offset, len, reinterpret_cast<uintptr_t>(ptr));
        pFd->offset += nWritten;
    }

    return static_cast<int>(nWritten);
}

off_t posix_lseek(int file, off_t ptr, int dir)
{
    F_NOTICE("lseek(" << file << ", " << ptr << ", " << dir << ")");

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(file);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    size_t fileSize = pFd->file->getSize();
    switch (dir)
    {
    case SEEK_SET:
        pFd->offset = ptr;
        break;
    case SEEK_CUR:
        pFd->offset += ptr;
        break;
    case SEEK_END:
        pFd->offset = fileSize + ptr;
        break;
    }

    return static_cast<int>(pFd->offset);
}

int posix_link(char *old, char *_new)
{
    /// \note To make nethack work, you either have to implement this, or return 0 and pretend
    ///       it worked (ie, make the files in the tree - which I've already done -- Matt)
    NOTICE("posix_link(" << old << ", " << _new << ")");
    SYSCALL_ERROR(Unimplemented);
    return 0;
}

int posix_readlink(const char* path, char* buf, unsigned int bufsize)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(path), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), bufsize, PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("readlink -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("readlink(" << path << ", " << reinterpret_cast<uintptr_t>(buf) << ", " << bufsize << ")");

    String realPath = normalisePath(path);

    File* f = VFS::instance().find(realPath, GET_CWD());
    if (!f)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if (!f->isSymlink())
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if (buf == 0)
        return -1;

    HugeStaticString str;
    HugeStaticString tmp;
    str.clear();
    tmp.clear();

    return Symlink::fromFile(f)->followLink(buf, bufsize);
}

int posix_realpath(const char *path, char *buf, size_t bufsize)
{
    F_NOTICE("realpath");

    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(path), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), bufsize, PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("realpath -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    String realPath = normalisePath(path);
    F_NOTICE("  -> traversing " << realPath);
    File* f = VFS::instance().find(realPath, GET_CWD());
    if (!f)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    f = traverseSymlink(f);
    if(!f)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if(!f->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }

    String actualPath = f->getFullPath();
    if(actualPath.length() > (bufsize - 1))
    {
        SYSCALL_ERROR(NameTooLong);
        return -1;
    }

    // File is good, copy it now.
    F_NOTICE("  -> returning " << actualPath);
    strncpy(buf, static_cast<const char *>(actualPath), bufsize);

    return 0;
}

int posix_unlink(char *name)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), PATH_MAX, PosixSubsystem::SafeRead))
    {
        F_NOTICE("unlink -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("unlink(" << name << ")");

    /// \todo Check permissions, perhaps!?

    String realPath = normalisePath(name);

    if (VFS::instance().remove(realPath, GET_CWD()))
        return 0;
    else
        return -1; /// \todo SYSCALL_ERROR of some sort
}

int posix_symlink(char *target, char *link)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(target), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(link), PATH_MAX, PosixSubsystem::SafeRead)))
    {
        F_NOTICE("symlink -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("symlink(" << target << ", " << link << ")");

    bool worked = VFS::instance().createSymlink(String(link), String(target), GET_CWD());
    if (worked)
        return 0;
    else
        ERROR("Symlink failed for `" << link << "' -> `" << target << "'");
    return -1;
}

int posix_rename(const char* source, const char* dst)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(source), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(dst), PATH_MAX, PosixSubsystem::SafeRead)))
    {
        F_NOTICE("rename -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("rename(" << source << ", " << dst << ")");

    String realSource = normalisePath(source);
    String realDestination = normalisePath(dst);

    File* src = VFS::instance().find(realSource, GET_CWD());
    File* dest = VFS::instance().find(realDestination, GET_CWD());

    if (!src)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // traverse symlink
    src = traverseSymlink(src);
    if(!src)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if (dest)
    {
        // traverse symlink
        dest = traverseSymlink(dest);
        if(!dest)
        {
            SYSCALL_ERROR(DoesNotExist);
            return -1;
        }

        if (dest->isDirectory() && !src->isDirectory())
        {
            SYSCALL_ERROR(FileExists);
            return -1;
        }
        else if (!dest->isDirectory() && src->isDirectory())
        {
            SYSCALL_ERROR(NotADirectory);
            return -1;
        }
    }
    else
    {
        VFS::instance().createFile(realDestination, 0777, GET_CWD());
        dest = VFS::instance().find(realDestination, GET_CWD());
        if (!dest)
        {
            // Failed to create the file?
            return -1;
        }
    }

    // Gay algorithm.
    uint8_t* buf = new uint8_t[src->getSize()];
    src->read(0, src->getSize(), reinterpret_cast<uintptr_t>(buf));
    dest->truncate();
    dest->write(0, src->getSize(), reinterpret_cast<uintptr_t>(buf));
    VFS::instance().remove(String(source), GET_CWD());
    delete [] buf;

    return 0;
}

char* posix_getcwd(char* buf, size_t maxlen)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), maxlen, PosixSubsystem::SafeWrite))
    {
        F_NOTICE("getcwd -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return 0;
    }

    F_NOTICE("getcwd(" << maxlen << ")");

    File* curr = GET_CWD();
    String str = curr->getFullPath(false);

    size_t maxLength = str.length();
    if(maxLength > maxlen)
        maxLength = maxlen;
    strncpy(buf, static_cast<const char*>(str), maxlen);

    return buf;
}

int posix_stat(const char *name, struct stat *st)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(st), sizeof(struct stat), PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("stat -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("stat(" << name << ")");

    // verify the filename - don't try to open a dud file (otherwise we'll open the cwd)
    if (name[0] == 0)
    {
        F_NOTICE("    -> Doesn't exist");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if(!st)
    {
        F_NOTICE("    -> Invalid argument");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    String realPath = normalisePath(name);

    File* file = VFS::instance().find(realPath, GET_CWD());
    if (!file)
    {
        F_NOTICE("    -> Not found by VFS");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }
    
    file = traverseSymlink(file);

    if(!file)
    {
        F_NOTICE("    -> Symlink traversal failed");
        return -1;
    }

    int mode = 0;
    if (ConsoleManager::instance().isConsole(file) || !strcmp(name, "/dev/null"))
    {
        mode = S_IFCHR;
    }
    else if (file->isDirectory())
    {
        mode = S_IFDIR;
    }
    else
    {
        mode = S_IFREG;
    }

    uint32_t permissions = file->getPermissions();
    if (permissions & FILE_UR) mode |= S_IRUSR;
    if (permissions & FILE_UW) mode |= S_IWUSR;
    if (permissions & FILE_UX) mode |= S_IXUSR;
    if (permissions & FILE_GR) mode |= S_IRGRP;
    if (permissions & FILE_GW) mode |= S_IWGRP;
    if (permissions & FILE_GX) mode |= S_IXGRP;
    if (permissions & FILE_OR) mode |= S_IROTH;
    if (permissions & FILE_OW) mode |= S_IWOTH;
    if (permissions & FILE_OX) mode |= S_IXOTH;

    st->st_dev   = static_cast<short>(reinterpret_cast<uintptr_t>(file->getFilesystem()));
    st->st_ino   = static_cast<short>(file->getInode());
    st->st_mode  = mode;
    st->st_nlink = 1;
    st->st_uid   = file->getUid();
    st->st_gid   = file->getGid();
    st->st_rdev  = 0;
    st->st_size  = static_cast<int>(file->getSize());
    st->st_atime = static_cast<int>(file->getAccessedTime());
    st->st_mtime = static_cast<int>(file->getModifiedTime());
    st->st_ctime = static_cast<int>(file->getCreationTime());
    st->st_blksize = static_cast<int>(file->getBlockSize());
    st->st_blocks = (st->st_size / st->st_blksize) + ((st->st_size % st->st_blksize) ? 1 : 0);

    F_NOTICE("    -> Success");
    return 0;
}

int posix_fstat(int fd, struct stat *st)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(st), sizeof(struct stat), PosixSubsystem::SafeWrite))
    {
        F_NOTICE("fstat -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("fstat(" << Dec << fd << Hex << ")");
    if(!st)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        ERROR("Error, no such FD!");
        // Error - no such file descriptor.
        return -1;
    }

    int mode = 0;
    if (ConsoleManager::instance().isConsole(pFd->file))
    {
        F_NOTICE("    -> S_IFCHR");
        mode = S_IFCHR;
    }
    else if (pFd->file->isDirectory())
    {
        F_NOTICE("    -> S_IFDIR");
        mode = S_IFDIR;
    }
    else
    {
        F_NOTICE("    -> S_IFREG");
        mode = S_IFREG;
    }

    uint32_t permissions = pFd->file->getPermissions();
    if (permissions & FILE_UR) mode |= S_IRUSR;
    if (permissions & FILE_UW) mode |= S_IWUSR;
    if (permissions & FILE_UX) mode |= S_IXUSR;
    if (permissions & FILE_GR) mode |= S_IRGRP;
    if (permissions & FILE_GW) mode |= S_IWGRP;
    if (permissions & FILE_GX) mode |= S_IXGRP;
    if (permissions & FILE_OR) mode |= S_IROTH;
    if (permissions & FILE_OW) mode |= S_IWOTH;
    if (permissions & FILE_OX) mode |= S_IXOTH;
    F_NOTICE("    -> " << mode);

    st->st_dev   = static_cast<short>(reinterpret_cast<uintptr_t>(pFd->file->getFilesystem()));
    F_NOTICE("    -> " << st->st_dev);
    st->st_ino   = static_cast<short>(pFd->file->getInode());
    F_NOTICE("    -> " << st->st_ino);
    st->st_mode  = mode;
    st->st_nlink = 1;
    st->st_uid   = pFd->file->getUid();
    st->st_gid   = pFd->file->getGid();
    st->st_rdev  = 0;
    st->st_size  = static_cast<int>(pFd->file->getSize());
    st->st_atime = static_cast<int>(pFd->file->getAccessedTime());
    st->st_mtime = static_cast<int>(pFd->file->getModifiedTime());
    st->st_ctime = static_cast<int>(pFd->file->getCreationTime());
    st->st_blksize = static_cast<int>(pFd->file->getBlockSize());
    st->st_blocks = (st->st_size / st->st_blksize) + ((st->st_size % st->st_blksize) ? 1 : 0);

    F_NOTICE("Success");
    return 0;
}

int posix_lstat(char *name, struct stat *st)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(st), sizeof(struct stat), PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("lstat -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("lstat(" << name << ")");
    if(!st)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    String realPath = normalisePath(name);

    File *file = VFS::instance().find(realPath, GET_CWD());

    int mode = 0;
    if (!file)
    {
        // Error - not found.
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }
    if (file->isSymlink())
    {
        mode = S_IFLNK;
    }
    else
    {
        if (ConsoleManager::instance().isConsole(file))
        {
            mode = S_IFCHR;
        }
        else if (file->isDirectory())
        {
            mode = S_IFDIR;
        }
        else
        {
            mode = S_IFREG;
        }
    }

    uint32_t permissions = file->getPermissions();
    if (permissions & FILE_UR) mode |= S_IRUSR;
    if (permissions & FILE_UW) mode |= S_IWUSR;
    if (permissions & FILE_UX) mode |= S_IXUSR;
    if (permissions & FILE_GR) mode |= S_IRGRP;
    if (permissions & FILE_GW) mode |= S_IWGRP;
    if (permissions & FILE_GX) mode |= S_IXGRP;
    if (permissions & FILE_OR) mode |= S_IROTH;
    if (permissions & FILE_OW) mode |= S_IWOTH;
    if (permissions & FILE_OX) mode |= S_IXOTH;

    st->st_dev   = static_cast<short>(reinterpret_cast<uintptr_t>(file->getFilesystem()));
    st->st_ino   = static_cast<short>(file->getInode());
    st->st_mode  = mode;
    st->st_nlink = 1;
    st->st_uid   = file->getGid();
    st->st_gid   = file->getGid();
    st->st_rdev  = 0;
    st->st_size  = static_cast<int>(file->getSize());
    st->st_atime = static_cast<int>(file->getAccessedTime());
    st->st_mtime = static_cast<int>(file->getModifiedTime());
    st->st_ctime = static_cast<int>(file->getCreationTime());
    st->st_blksize = static_cast<int>(file->getBlockSize());
    st->st_blocks = (st->st_size / st->st_blksize) + ((st->st_size % st->st_blksize) ? 1 : 0);

    return 0;
}

int posix_opendir(const char *dir, dirent *ent)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(dir), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ent), sizeof(dirent), PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("opendir -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("opendir(" << dir << ")");

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    size_t fd = pSubsystem->getFd();

    String realPath = normalisePath(dir);

    File* file = VFS::instance().find(realPath, GET_CWD());
    if (!file)
    {
        // Error - not found.
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }
    
    file = traverseSymlink(file);

    if(!file)
        return -1;

    if (!file->isDirectory())
    {
        // Error - not a directory.
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }

    FileDescriptor *f = new FileDescriptor;
    f->file = file;
    f->offset = 0;
    f->fd = fd;

    file = Directory::fromFile(file)->getChild(0);
    if (file)
    {
        ent->d_ino = file->getInode();

        // Some applications consider a null inode to mean "bad file" which is
        // a horrible assumption for them to make. Because the presence of a file
        // is indicated by more effective means (ie, successful return from
        // readdir) this just appeases the applications which aren't portably
        // written.
        if(ent->d_ino == 0)
            ent->d_ino = 0x7fff; // Signed, don't want this to turn negative

        // Copy the filename across
        strncpy(ent->d_name, static_cast<const char*>(file->getName()), MAXNAMLEN);
    }
    else
    {
        // No file here.
        memset(ent, 0, sizeof(*ent));
    }

    pSubsystem->addFileDescriptor(fd, f);

    return static_cast<int>(fd);
}

int posix_readdir(int fd, dirent *ent)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ent), sizeof(dirent), PosixSubsystem::SafeWrite))
    {
        F_NOTICE("readdir -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("readdir(" << fd << ")");

    if (fd == -1)
        return -1;

    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd || !pFd->file)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    if(!pFd->file->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }
    File* file = Directory::fromFile(pFd->file)->getChild(pFd->offset);
    if (!file)
    {
        // Normal EOF condition.
        SYSCALL_ERROR(NoError);
        return -1;
    }

    ent->d_ino = static_cast<short>(file->getInode());
    String tmp = file->getName();
    strcpy(ent->d_name, static_cast<const char*>(tmp));
    ent->d_name[strlen(static_cast<const char*>(tmp))] = '\0';
    if(file->isSymlink())
        ent->d_type = DT_LNK;
    else
        ent->d_type = file->isDirectory() ? DT_DIR : DT_REG;
    pFd->offset ++;

    return 0;
}

void posix_rewinddir(int fd, dirent *ent)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(ent), sizeof(dirent), PosixSubsystem::SafeWrite))
    {
        F_NOTICE("rewinddir -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return;
    }

    if (fd == -1)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return;
    }

    F_NOTICE("rewinddir(" << fd << ")");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return;
    }
    FileDescriptor *f = pSubsystem->getFileDescriptor(fd);
    f->offset = 0;
    posix_readdir(fd, ent);
}

int posix_closedir(int fd)
{
    if (fd == -1)
        return -1;

    F_NOTICE("closedir(" << fd << ")");

    /// \todo Race here - fix.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    pSubsystem->freeFd(fd);

    return 0;
}

int posix_ioctl(int fd, int command, void *buf)
{
    F_NOTICE("ioctl(" << Dec << fd << ", " << Hex << command << ", " << reinterpret_cast<uintptr_t>(buf) << ")");

    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(fd);
    if (!f)
    {
        // Error - no such FD.
        return -1;
    }

    /// \todo Sanitise buf, if it has meaning for the command.

    if (f->file->supports(command))
    {
        return f->file->command(command, buf);
    }

    switch (command)
    {
        case TIOCGWINSZ:
        {
            return console_getwinsize(f->file, reinterpret_cast<winsize_t*>(buf));
        }

        case TIOCSWINSZ:
        {
            const winsize_t *ws = reinterpret_cast<const winsize_t*>(buf);
            F_NOTICE(" -> TIOCSWINSZ " << Dec << ws->ws_col << "x" << ws->ws_row << Hex);
            return console_setwinsize(f->file, ws);
        }

        case TIOCFLUSH:
        {
            return console_flush(f->file, buf);
        }

        case TIOCSCTTY:
        {
            F_NOTICE(" -> TIOCSCTTY");
            return console_setctty(fd, reinterpret_cast<uintptr_t>(buf) == 1);
        }

        case FIONBIO:
        {
            // set/unset non-blocking
            if (buf)
            {
                int a = *reinterpret_cast<int *>(buf);
                if (a)
                {
                    F_NOTICE("  -> set non-blocking");
                    f->flflags |= O_NONBLOCK;
                }
                else
                {
                    F_NOTICE("  -> set blocking");
                    f->flflags &= ~O_NONBLOCK;
                }
            }
            else
                f->flflags &= ~O_NONBLOCK;

            return 0;
        }
        default:
        {
            // Error - no such ioctl.
            SYSCALL_ERROR(InvalidArgument);
            return -1;
        }
    }
}

int posix_chdir(const char *path)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(path), PATH_MAX, PosixSubsystem::SafeRead))
    {
        F_NOTICE("chdir -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("chdir(" << path << ")");

    String realPath = normalisePath(path);

    File *dir = VFS::instance().find(realPath, GET_CWD());
    File *target = 0;
    if (dir->isSymlink())
        target = traverseSymlink(dir);
    if (dir && (dir->isDirectory() || (dir->isSymlink() && target->isDirectory())))
    {
        Processor::information().getCurrentThread()->getParent()->setCwd(dir);
    }
    else if(dir && !dir->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }
    else
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    return 0;
}

int posix_dup(int fd)
{
    F_NOTICE("dup(" << fd << ")");

    // grab the file descriptor pointer for the passed descriptor
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(fd);
    if (!f)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    size_t newFd = pSubsystem->getFd();

    // Copy the descriptor
    FileDescriptor* f2 = new FileDescriptor(*f);
    pSubsystem->addFileDescriptor(newFd, f2);

    return static_cast<int>(newFd);
}

int posix_dup2(int fd1, int fd2)
{
    F_NOTICE("dup2(" << fd1 << ", " << fd2 << ")");

    if (fd2 < 0)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1; // EBADF
    }

    if (fd1 == fd2)
        return fd2;

    // grab the file descriptor pointer for the passed descriptor
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor* f = pSubsystem->getFileDescriptor(fd1);
    if (!f)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    // Copy the descriptor.
    //
    // This will also increase the refcount *before* we close the original, else we
    // might accidentally trigger an EOF condition on a pipe! (if the write refcount
    // drops to zero)...
    FileDescriptor* f2 = new FileDescriptor(*f);
    pSubsystem->addFileDescriptor(fd2, f2);

    // According to the spec, CLOEXEC is cleared on DUP.
    f2->fdflags &= ~FD_CLOEXEC;

    return static_cast<int>(fd2);
}

int posix_mkdir(const char* name, int mode)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), PATH_MAX, PosixSubsystem::SafeRead))
    {
        F_NOTICE("mkdir -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("mkdir(" << name << ")");

    String realPath = normalisePath(name);

    bool worked = VFS::instance().createDirectory(realPath, GET_CWD());
    return worked ? 0 : -1;
}

int posix_isatty(int fd)
{
    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        ERROR("isatty: no such file descriptor (" << Dec << fd << ")");
        return 0;
    }

    int result = ConsoleManager::instance().isConsole(pFd->file) ? 1 : 0;
    NOTICE("isatty(" << fd << ") -> " << result);
    return result;
}

int posix_fcntl(int fd, int cmd, int num, int* args)
{
    /// \todo Same as ioctl, figure out how best to sanitise input addresses
    if (num)
        F_NOTICE("fcntl(" << fd << ", " << cmd << ", " << num << ", " << args[0] << ")");
    /// \note Added braces. Compiler warned about ambiguity if F_NOTICE isn't enabled. It seems to be able
    ///       to figure out what we meant to do, but making it explicit never hurt anyone.
    else
    {
        F_NOTICE("fcntl(" << fd << ", " << cmd << ")");
    }

    // grab the file descriptor pointer for the passed descriptor
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor* f = pSubsystem->getFileDescriptor(fd);
    if(!f)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    switch (cmd)
    {
        case F_DUPFD:

            if (num)
            {
                // if there is an argument and it's valid, map fd to the passed descriptor
                if (args[0] >= 0)
                {
                    size_t fd2 = static_cast<size_t>(args[0]);

                    // Copy the descriptor (addFileDescriptor automatically frees the old one, if needed)
                    FileDescriptor* f2 = new FileDescriptor(*f);
                    pSubsystem->addFileDescriptor(fd2, f2);

                    // According to the spec, CLOEXEC is cleared on DUP.
                    f2->fdflags &= ~FD_CLOEXEC;

                    return static_cast<int>(fd2);
                }
            }
            else
            {
                size_t fd2 = pSubsystem->getFd();

                // copy the descriptor
                FileDescriptor* f2 = new FileDescriptor(*f);
                pSubsystem->addFileDescriptor(fd2, f2);

                // According to the spec, CLOEXEC is cleared on DUP.
                f2->fdflags &= ~FD_CLOEXEC;

                return static_cast<int>(fd2);
            }
            return 0;
            break;

        case F_GETFD:
            return f->fdflags;
        case F_SETFD:
            f->fdflags = args[0];
            return 0;
        case F_GETFL:
            F_NOTICE("  -> get flags " << f->flflags);
            return f->flflags;
        case F_SETFL:
            F_NOTICE("  -> set flags " << args[0]);
            f->flflags = args[0] & (O_APPEND | O_NONBLOCK);
            F_NOTICE("  -> new flags " << f->flflags);
            return 0;
        case F_GETLK: // Get record-locking information
        case F_SETLK: // Set or clear a record lock (without blocking
        case F_SETLKW: // Set or clear a record lock (with blocking)

            // Grab the lock information structure
            struct flock *lock = reinterpret_cast<struct flock*>(args[0]);
            if(!lock)
            {
                SYSCALL_ERROR(InvalidArgument);
                return -1;
            }

            // Lock the LockedFile map
            // LockGuard<Mutex> lockFileGuard(g_PosixLockedFileMutex);

            // Can only take exclusive locks...
            if(cmd == F_GETLK)
            {
                if(f->lockedFile)
                {
                    lock->l_type = F_WRLCK;
                    lock->l_whence = SEEK_SET;
                    lock->l_start = lock->l_len = 0;
                    lock->l_pid = f->lockedFile->getLocker();
                }
                else
                    lock->l_type = F_UNLCK;

                return 0;
            }

            // Trying to set an exclusive lock?
            if(lock->l_type == F_WRLCK)
            {
                // Already got a LockedFile instance?
                if(f->lockedFile)
                {
                    if(cmd == F_SETLK)
                    {
                        return f->lockedFile->lock(false) ? 0 : -1;
                    }
                    else
                    {
                        // Lock the file, blocking
                        f->lockedFile->lock(true);
                        return 0;
                    }
                }

                // Not already locked!
                LockedFile *lf = new LockedFile(f->file);
                if(!lf)
                {
                    SYSCALL_ERROR(OutOfMemory);
                    return -1;
                }

                // Insert
                g_PosixGlobalLockedFiles.insert(f->file->getFullPath(), lf);
                f->lockedFile = lf;

                // The file is now locked
                return 0;
            }

            // Trying to unlock?
            if(lock->l_type == F_UNLCK)
            {
                // No locked file? Fail
                if(!f->lockedFile)
                    return -1;

                // Only need to unlock the file - it'll be locked again when needed
                f->lockedFile->unlock();
                return 0;
            }

            // Success, none of the above, no reason to be unlockable
            return 0;
    }

    SYSCALL_ERROR(Unimplemented);
    return -1;
}

struct _mmap_tmp
{
    void *addr;
    size_t len;
    int prot;
    int flags;
    int fildes;
    off_t off;
};

void *posix_mmap(void *p)
{
    F_NOTICE("mmap");

    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(p), sizeof(_mmap_tmp), PosixSubsystem::SafeRead))
    {
        F_NOTICE("mmap -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return MAP_FAILED;
    }

    // Grab the parameter list
    _mmap_tmp *map_info = reinterpret_cast<_mmap_tmp*>(p);

    // Get real variables from the parameters
    void *addr = map_info->addr;
    size_t len = map_info->len;
    int prot = map_info->prot;
    int flags = map_info->flags;
    int fd = map_info->fildes;
    off_t off = map_info->off;

    F_NOTICE("  -> addr=" << reinterpret_cast<uintptr_t>(addr) << ", len=" << len << ", prot=" << prot << ", flags=" << flags << ", fildes=" << fd << ", off=" << off << ".");

    // Get the File object to map
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return MAP_FAILED;
    }

    // The return address
    void *finalAddress = 0;

    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    size_t pageSz = PhysicalMemoryManager::getPageSize();

    // Sanitise input.
    uintptr_t sanityAddress = reinterpret_cast<uintptr_t>(addr);
    if(sanityAddress)
    {
        if((sanityAddress < va.getUserStart()) ||
            (sanityAddress >= va.getKernelStart()))
        {
            if(flags & MAP_FIXED)
            {
                // Invalid input and MAP_FIXED, this is an error.
                SYSCALL_ERROR(InvalidArgument);
                F_NOTICE("  -> mmap given invalid fixed address");
                return MAP_FAILED;
            }
            else
            {
                // Invalid input - but not MAP_FIXED, so we can ignore addr.
                sanityAddress = 0;
            }
        }
    }
    addr = reinterpret_cast<void *>(sanityAddress);

    // Verify the passed length
    if(!len || (sanityAddress & (pageSz-1)))
    {
        SYSCALL_ERROR(InvalidArgument);
        return MAP_FAILED;
    }

    // Create permission set.
    MemoryMappedObject::Permissions perms;
    if(prot & PROT_NONE)
    {
        perms = MemoryMappedObject::None;
    }
    else
    {
        // Everything implies a readable memory region.
        perms = MemoryMappedObject::Read;
        if(prot & PROT_WRITE)
            perms |= MemoryMappedObject::Write;
        if(prot & PROT_EXEC)
            perms |= MemoryMappedObject::Exec;
    }

    if(flags & MAP_ANON)
    {
        if(flags & MAP_SHARED)
        {
            F_NOTICE("  -> failed (MAP_SHARED cannot be used with MAP_ANONYMOUS)");
            SYSCALL_ERROR(InvalidArgument);
            return MAP_FAILED;
        }

        MemoryMappedObject *pObject = MemoryMapManager::instance().mapAnon(sanityAddress, len, perms);
        if(!pObject)
        {
            /// \todo Better error?
            SYSCALL_ERROR(OutOfMemory);
            F_NOTICE("  -> failed (mapAnon)!");
            return MAP_FAILED;
        }

        F_NOTICE("  -> " << sanityAddress);

        finalAddress = reinterpret_cast<void*>(sanityAddress);
    }
    else
    {
        // Valid file passed?
        FileDescriptor* f = pSubsystem->getFileDescriptor(fd);
        if(!f)
        {
            SYSCALL_ERROR(BadFileDescriptor);
            return MAP_FAILED;
        }

        // Grab the file to map in
        File *fileToMap = f->file;
        
        F_NOTICE("mmap: file name is " << fileToMap->getFullPath());

        // Grab the MemoryMappedFile for it. This will automagically handle
        // MAP_FIXED mappings too
        bool bCopyOnWrite = (flags & MAP_SHARED) == 0;
        MemoryMappedObject *pFile = MemoryMapManager::instance().mapFile(fileToMap, sanityAddress, len, perms, off, bCopyOnWrite);
        if(!pFile)
        {
            /// \todo Better error?
            SYSCALL_ERROR(OutOfMemory);
            F_NOTICE("  -> failed (mapFile)!");
            return MAP_FAILED;
        }

        F_NOTICE("  -> " << sanityAddress);

        finalAddress = reinterpret_cast<void*>(sanityAddress);
    }

    // Complete
    return finalAddress;
}

int posix_msync(void *p, size_t len, int flags) {
    F_NOTICE("msync");

    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    size_t pageSz = PhysicalMemoryManager::getPageSize();

    // Verify the passed length
    if(!len || (addr & (pageSz-1)))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if((flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Make sure there's at least one object we'll touch.
    if(!MemoryMapManager::instance().contains(addr, len))
    {
        SYSCALL_ERROR(OutOfMemory);
        return -1;
    }

    if(flags & MS_INVALIDATE)
    {
        MemoryMapManager::instance().invalidate(addr, len);
    }
    else
    {
        MemoryMapManager::instance().sync(addr, len, flags & MS_ASYNC);
    }

    return 0;
}

int posix_mprotect(void *p, size_t len, int prot)
{
    F_NOTICE("mprotect");

    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    size_t pageSz = PhysicalMemoryManager::getPageSize();

    // Verify the passed length
    if(!len || (addr & (pageSz-1)))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Make sure there's at least one object we'll touch.
    if(!MemoryMapManager::instance().contains(addr, len))
    {
        SYSCALL_ERROR(OutOfMemory);
        return -1;
    }

    // Create permission set.
    MemoryMappedObject::Permissions perms;
    if(prot & PROT_NONE)
    {
        perms = MemoryMappedObject::None;
    }
    else
    {
        // Everything implies a readable memory region.
        perms = MemoryMappedObject::Read;
        if(prot & PROT_WRITE)
            perms |= MemoryMappedObject::Write;
        if(prot & PROT_EXEC)
            perms |= MemoryMappedObject::Exec;
    }

    /// \todo EACCESS

    MemoryMapManager::instance().setPermissions(addr, len, perms);

    return 0;
}

int posix_munmap(void *addr, size_t len)
{
    F_NOTICE("munmap(" << reinterpret_cast<uintptr_t>(addr) << ", " << len << ")");

    if(!len)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    MemoryMapManager::instance().remove(reinterpret_cast<uintptr_t>(addr), len);

    return 0;
}

int posix_access(const char *name, int amode)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(name), PATH_MAX, PosixSubsystem::SafeRead))
    {
        F_NOTICE("access -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("access(" << (name ? name : "n/a") << ", " << Dec << amode << Hex << ")");
    if(!name)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    String realPath = normalisePath(name);

    // Grab the file
    File *file = VFS::instance().find(realPath, GET_CWD());
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    /// \todo Proper permission checks. For now, the file exists, and you can do what you want with it.
    F_NOTICE("  -> ok");
    return 0;
}

int posix_ftruncate(int a, off_t b)
{
	F_NOTICE("ftruncate(" << a << ", " << b << ")");

    // Grab the File pointer for this file
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(a);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    File *pFile = pFd->file;

    // If we are to simply truncate, do so
    if(b == 0)
    {
        pFile->truncate();
        return 0;
    }
    else if(static_cast<size_t>(b) == pFile->getSize())
        return 0;
    // If we need to reduce the file size, do so
    else if(static_cast<size_t>(b) < pFile->getSize())
    {
        pFile->setSize(b);
        return 0;
    }
    // Otherwise, extend the file
    else
    {
        size_t currSize = pFile->getSize();
        size_t numExtraBytes = b - currSize;
        NOTICE("Extending by " << numExtraBytes << " bytes");
        uint8_t *nullBuffer = new uint8_t[numExtraBytes];
        NOTICE("Got the buffer");
        memset(nullBuffer, 0, numExtraBytes);
        NOTICE("Zeroed the buffer");
        pFile->write(currSize, numExtraBytes, reinterpret_cast<uintptr_t>(nullBuffer));
        NOTICE("Deleting the buffer");
        delete [] nullBuffer;
        NOTICE("Complete");
        return 0;
    }

    // Can't get here
	return -1;
}

int posix_fsync(int fd)
{
    F_NOTICE("fsync(" << fd << ")");

    // Grab the File pointer for this file
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    File *pFile = pFd->file;
    pFile->sync();

    return 0;
}

int pedigree_get_mount(char* mount_buf, char* info_buf, size_t n)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(mount_buf), PATH_MAX, PosixSubsystem::SafeWrite) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(info_buf), PATH_MAX, PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("pedigree_get_mount -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    NOTICE("pedigree_get_mount(" << Dec << n << Hex << ")");
    
    typedef List<String*> StringList;
    typedef Tree<Filesystem *, List<String*>* > VFSMountTree;
    VFSMountTree &mounts = VFS::instance().getMounts();
    
    size_t i = 0;
    for(VFSMountTree::Iterator it = mounts.begin();
        it != mounts.end();
        it++)
    {
        Filesystem *pFs = it.key();
        StringList *pList = it.value();
        Disk *pDisk = pFs->getDisk();
        
        for(StringList::Iterator it2 = pList->begin();
            it2 != pList->end();
            it2++, i++)
        {
            String mount = **it2;
            
            if(i == n)
            {
                String info, s;
                if(pDisk)
                {
                    pDisk->getName(s);
                    pDisk->getParent()->getName(info);
                    info += " // ";
                    info += s;
                }
                else
                    info = "no disk";
                
                strcpy(mount_buf, static_cast<const char *>(mount));
                strcpy(info_buf, static_cast<const char *>(info));
                
                return 0;
            }
        }
    }
    
    return -1;
}

int posix_chmod(const char *path, mode_t mode)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(path), PATH_MAX, PosixSubsystem::SafeRead))
    {
        F_NOTICE("chmod -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("chmod(" << String(path) << ", " << Oct << mode << Hex << ")");
    
    /// \todo EACCESS, EPERM
    
    if((mode == static_cast<mode_t>(-1)) || (mode > 0777))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    bool onDevFs = false;
    String realPath = normalisePath(path, &onDevFs);

    if(onDevFs)
    {
        // Silently ignore.
        return 0;
    }

    File* file = VFS::instance().find(realPath, GET_CWD());
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }
    
    // Read-only filesystem?
    if(file->getFilesystem()->isReadOnly())
    {
        SYSCALL_ERROR(ReadOnlyFilesystem);
        return -1;
    }

    // Symlink traversal
    file = traverseSymlink(file);
    if(!file)
        return -1;
    
    /// \todo Might want to change permissions on open file descriptors?
    uint32_t permissions = 0;
    if (mode & S_IRUSR) permissions |= FILE_UR;
    if (mode & S_IWUSR) permissions |= FILE_UW;
    if (mode & S_IXUSR) permissions |= FILE_UX;
    if (mode & S_IRGRP) permissions |= FILE_GR;
    if (mode & S_IWGRP) permissions |= FILE_GW;
    if (mode & S_IXGRP) permissions |= FILE_GX;
    if (mode & S_IROTH) permissions |= FILE_OR;
    if (mode & S_IWOTH) permissions |= FILE_OW;
    if (mode & S_IXOTH) permissions |= FILE_OX;
    file->setPermissions(permissions);
    
    return 0;
}

int posix_chown(const char *path, uid_t owner, gid_t group)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(path), PATH_MAX, PosixSubsystem::SafeRead))
    {
        F_NOTICE("chown -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("chown(" << String(path) << ", " << owner << ", " << group << ")");
    
    /// \todo EACCESS, EPERM
    
    // Is there any need to change?
    if((owner == group) && (owner == static_cast<uid_t>(-1)))
        return 0;

    bool onDevFs = false;
    String realPath = normalisePath(path, &onDevFs);

    if(onDevFs)
    {
        // Silently ignore.
        return 0;
    }

    File* file = VFS::instance().find(realPath, GET_CWD());
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }
    
    // Read-only filesystem?
    if(file->getFilesystem()->isReadOnly())
    {
        SYSCALL_ERROR(ReadOnlyFilesystem);
        return -1;
    }

    // Symlink traversal
    file = traverseSymlink(file);
    if(!file)
        return -1;
    
    // Set the UID and GID
    if(owner != static_cast<uid_t>(-1))
        file->setUid(owner);
    if(group != static_cast<gid_t>(-1))
        file->setGid(group);
    
    return 0;
}

int posix_fchmod(int fd, mode_t mode)
{
    F_NOTICE("fchmod(" << fd << ", " << Oct << mode << Hex << ")");
    
    /// \todo EACCESS, EPERM
    
    if((mode == static_cast<mode_t>(-1)) || (mode > 0777))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }
    
    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    
    File *file = pFd->file;
    
    // Read-only filesystem?
    if(file->getFilesystem()->isReadOnly())
    {
        SYSCALL_ERROR(ReadOnlyFilesystem);
        return -1;
    }
    
    /// \todo Might want to change permissions on open file descriptors?
    uint32_t permissions = 0;
    if (mode & S_IRUSR) permissions |= FILE_UR;
    if (mode & S_IWUSR) permissions |= FILE_UW;
    if (mode & S_IXUSR) permissions |= FILE_UX;
    if (mode & S_IRGRP) permissions |= FILE_GR;
    if (mode & S_IWGRP) permissions |= FILE_GW;
    if (mode & S_IXGRP) permissions |= FILE_GX;
    if (mode & S_IROTH) permissions |= FILE_OR;
    if (mode & S_IWOTH) permissions |= FILE_OW;
    if (mode & S_IXOTH) permissions |= FILE_OX;
    file->setPermissions(permissions);
    
    return 0;
}

int posix_fchown(int fd, uid_t owner, gid_t group)
{
    F_NOTICE("fchown(" << fd << ", " << owner << ", " << group << ")");
    
    /// \todo EACCESS, EPERM
    
    // Is there any need to change?
    if((owner == group) && (owner == static_cast<uid_t>(-1)))
        return 0;
    
    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    
    File *file = pFd->file;
    
    // Read-only filesystem?
    if(file->getFilesystem()->isReadOnly())
    {
        SYSCALL_ERROR(ReadOnlyFilesystem);
        return -1;
    }
    
    // Set the UID and GID
    if(owner != static_cast<uid_t>(-1))
        file->setUid(owner);
    if(group != static_cast<gid_t>(-1))
        file->setGid(group);
    
    return 0;
}

int posix_fchdir(int fd)
{
    F_NOTICE("fchdir(" << fd << ")");
    
    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    
    File *file = pFd->file;
    if(!file->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }
    
    Processor::information().getCurrentThread()->getParent()->setCwd(file);
    return 0;
}

int statvfs_doer(Filesystem *pFs, struct statvfs *buf)
{
    if(!pFs)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }
    
    /// \todo Get all this data from the Filesystem object
    buf->f_bsize = 4096;
    buf->f_frsize = 512;
    buf->f_blocks = static_cast<fsblkcnt_t>(-1);
    buf->f_bfree = static_cast<fsblkcnt_t>(-1);
    buf->f_bavail = static_cast<fsblkcnt_t>(-1);
    buf->f_files = 0;
    buf->f_ffree = static_cast<fsfilcnt_t>(-1);
    buf->f_favail = static_cast<fsfilcnt_t>(-1);
    buf->f_fsid = 0;
    buf->f_flag = (pFs->isReadOnly() ? ST_RDONLY : 0) | ST_NOSUID; // No suid in pedigree yet.
    buf->f_namemax = VFS_MNAMELEN;
    
    // FS type
    strcpy(buf->f_fstypename, "ext2");
    
    // "From" point
    /// \todo Disk device hash + path (on raw filesystem maybe?)
    strcpy(buf->f_mntfromname, "from");
    
    // "To" point
    /// \todo What to put here?
    strcpy(buf->f_mntfromname, "to");
    
    return 0;
}

int posix_fstatvfs(int fd, struct statvfs *buf)
{
    if(!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), sizeof(struct statvfs), PosixSubsystem::SafeWrite))
    {
        F_NOTICE("fstatvfs -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("fstatvfs(" << fd << ")");
    
    // Lookup this process.
    Process *pProcess = Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem = reinterpret_cast<PosixSubsystem*>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }
    
    File *file = pFd->file;

    return statvfs_doer(file->getFilesystem(), buf);
}

int posix_statvfs(const char *path, struct statvfs *buf)
{
    if(!(PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(path), PATH_MAX, PosixSubsystem::SafeRead) &&
        PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), sizeof(struct statvfs), PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("statvfs -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("statvfs(" << path << ")");

    String realPath = normalisePath(path);

    File* file = VFS::instance().find(realPath, GET_CWD());
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // Symlink traversal
    file = traverseSymlink(file);
    if(!file)
        return -1;
    
    return statvfs_doer(file->getFilesystem(), buf);
}

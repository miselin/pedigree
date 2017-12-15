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

#include "modules/system/console/Console.h"
#include "modules/system/network-stack/NetManager.h"
#include "modules/system/network-stack/Tcp.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/users/UserManager.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/LockedFile.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Pipe.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/utility.h"

#include "pedigree/kernel/Subsystem.h"
#include <PosixProcess.h>
#include <PosixSubsystem.h>
#include <FileDescriptor.h>

#include "console-syscalls.h"
#include "file-syscalls.h"
#include "net-syscalls.h"
#include "pipe-syscalls.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <utime.h>

// Emits a lot of logs in normalisePath to help debug remaps.
#define ENABLE_VERBOSE_NORMALISATION 0

extern int posix_getpid();

// For getdents() (getdents64 uses a compatible struct dirent).
struct linux_dirent
{
    long d_ino;
    off_t d_off;
    unsigned short d_reclen;
    char d_name[];
};

extern DevFs *g_pDevFs;

//
// Syscalls pertaining to files.
//

#define CHECK_FLAG(a, b) (((a) & (b)) == (b))

#define GET_CWD() \
    (Processor::information().getCurrentThread()->getParent()->getCwd())

static PosixProcess *getPosixProcess()
{
    Process *pStockProcess =
        Processor::information().getCurrentThread()->getParent();
    if (pStockProcess->getType() != Process::Posix)
    {
        return 0;
    }

    PosixProcess *pProcess = static_cast<PosixProcess *>(pStockProcess);
    return pProcess;
}

File *findFileWithAbiFallbacks(String name, File *cwd)
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    if (cwd == nullptr)
    {
        cwd = pProcess->getCwd();
    }

    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    bool mountAwareAbi = pSubsystem->getAbi() != PosixSubsystem::LinuxAbi;

    File *target = VFS::instance().find(name, cwd);

    if (mountAwareAbi)
    {
        // no fall back for mount-aware ABIs (e.g. Pedigree's ABI)
        return target;
    }

    // for non-mount-aware ABIs, we need to fall back if the path is absolute
    // this means we can be on dev»/ and still run things like /bin/ls because
    // the lookup for dev»/bin/ls fails and falls back to root»/bin/ls
    if (name[0] != '/')
    {
        return target;
    }

    if (!target)
    {
        // fall back to root filesystem
        Filesystem *pRootFs = VFS::instance().lookupFilesystem(String("root"));
        if (pRootFs)
        {
            target = VFS::instance().find(name, pRootFs->getRoot());
        }
    }

    return target;
}

static File *traverseSymlink(File *file)
{
    /// \todo detect inability to access at each intermediate step.
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return 0;
    }

    Tree<File *, File *> loopDetect;
    while (file->isSymlink())
    {
        file = Symlink::fromFile(file)->followLink();
        if (!file)
        {
            SYSCALL_ERROR(DoesNotExist);
            return 0;
        }

        if (loopDetect.lookup(file))
        {
            SYSCALL_ERROR(LoopExists);
            return 0;
        }
        else
            loopDetect.insert(file, file);
    }

    return file;
}

static bool doChdir(File *dir)
{
    File *target = 0;
    if (dir->isSymlink())
    {
        target = traverseSymlink(dir);
        if (!target)
        {
            F_NOTICE("Symlink traversal failed.");
            SYSCALL_ERROR(DoesNotExist);
            return false;
        }
    }

    if (dir &&
        (dir->isDirectory() || (dir->isSymlink() && target->isDirectory())))
    {
        File *pRealFile = dir;
        if (dir->isSymlink())
        {
            pRealFile = target;
        }

        // Only need execute permissions to enter a directory.
        if (!VFS::checkAccess(pRealFile, false, false, true))
        {
            return false;
        }

        Processor::information().getCurrentThread()->getParent()->setCwd(dir);
    }
    else if (dir && !dir->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return false;
    }
    else
    {
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }

    return true;
}

static bool
doStat(const char *name, File *pFile, struct stat *st, bool traverse = true)
{
    if (traverse)
    {
        pFile = traverseSymlink(pFile);
        if (!pFile)
        {
            F_NOTICE("    -> Symlink traversal failed");
            return -1;
        }
    }

    int mode = 0;
    /// \todo files really should be able to expose their "type"...
    if (ConsoleManager::instance().isConsole(pFile) ||
        (name && !StringCompare(name, "/dev/null")) ||
        (pFile && pFile->getName() == "null"))
    {
        F_NOTICE("    -> S_IFCHR");
        mode = S_IFCHR;
    }
    else if (pFile->isDirectory())
    {
        F_NOTICE("    -> S_IFDIR");
        mode = S_IFDIR;
    }
    else if (pFile->isSymlink() || pFile->isPipe())
    {
        F_NOTICE("    -> S_IFLNK");
        mode = S_IFLNK;
    }
    else if (pFile->isFifo())
    {
        F_NOTICE("    -> S_FIFO");
        mode = S_IFIFO;
    }
    else if (pFile->isSocket())
    {
        F_NOTICE("    -> S_SOCK");
        mode = S_IFSOCK;
    }
    else
    {
        F_NOTICE("    -> S_IFREG");
        mode = S_IFREG;
    }

    // Clear any cruft in the stat structure before we fill it.
    ByteSet(st, 0, sizeof(*st));

    uint32_t permissions = pFile->getPermissions();
    if (permissions & FILE_UR)
        mode |= S_IRUSR;
    if (permissions & FILE_UW)
        mode |= S_IWUSR;
    if (permissions & FILE_UX)
        mode |= S_IXUSR;
    if (permissions & FILE_GR)
        mode |= S_IRGRP;
    if (permissions & FILE_GW)
        mode |= S_IWGRP;
    if (permissions & FILE_GX)
        mode |= S_IXGRP;
    if (permissions & FILE_OR)
        mode |= S_IROTH;
    if (permissions & FILE_OW)
        mode |= S_IWOTH;
    if (permissions & FILE_OX)
        mode |= S_IXOTH;
    if (permissions & FILE_STICKY)
        mode |= S_ISVTX;
    F_NOTICE("    -> " << Oct << mode);

    Filesystem *pFs = pFile->getFilesystem();

    /// \todo expose number of links and number of blocks from Files
    st->st_dev =
        static_cast<short>(reinterpret_cast<uintptr_t>(pFile->getFilesystem()));
    F_NOTICE("    -> " << st->st_dev);
    st->st_ino = static_cast<short>(pFile->getInode());
    F_NOTICE("    -> " << st->st_ino);
    st->st_mode = mode;
    st->st_nlink = 1;
    st->st_uid = pFile->getUid();
    st->st_gid = pFile->getGid();
    st->st_rdev = 0;
    st->st_size = static_cast<int>(pFile->getSize());
    F_NOTICE("    -> " << st->st_size);
    st->st_atime = pFile->getAccessedTime();
    st->st_mtime = pFile->getModifiedTime();
    st->st_ctime = pFile->getCreationTime();
    st->st_blksize = static_cast<int>(pFile->getBlockSize());
    st->st_blocks = (st->st_size / st->st_blksize) +
                    ((st->st_size % st->st_blksize) ? 1 : 0);

    // Special fixups
    if (pFs == g_pDevFs)
    {
        if ((name && !StringCompare(name, "/dev/null")) ||
            (pFile->getName() == "null"))
        {
            NOTICE("/dev/null, fixing st_rdev");
            // major/minor device numbers
            st->st_rdev = 0x0103;
        }
        else if (ConsoleManager::instance().isConsole(pFile))
        {
            /// \todo assumption here
            ConsoleFile *pConsole = static_cast<ConsoleFile *>(pFile);
            st->st_rdev = 0x8800 | pConsole->getConsoleNumber();
        }
    }

    return true;
}

static bool doChmod(File *pFile, mode_t mode)
{
    // Are we the owner of the file?
    User *pCurrentUser =
        Processor::information().getCurrentThread()->getParent()->getUser();

    size_t uid = pCurrentUser->getId();
    if (!(uid == pFile->getUid() || uid == 0))
    {
        F_NOTICE(" -> EPERM");
        // Not allowed - EPERM.
        // User must own the file or be superuser.
        SYSCALL_ERROR(NotEnoughPermissions);
        return false;
    }

    /// \todo Might want to change permissions on open file descriptors?
    uint32_t permissions = 0;
    if (mode & S_IRUSR)
        permissions |= FILE_UR;
    if (mode & S_IWUSR)
        permissions |= FILE_UW;
    if (mode & S_IXUSR)
        permissions |= FILE_UX;
    if (mode & S_IRGRP)
        permissions |= FILE_GR;
    if (mode & S_IWGRP)
        permissions |= FILE_GW;
    if (mode & S_IXGRP)
        permissions |= FILE_GX;
    if (mode & S_IROTH)
        permissions |= FILE_OR;
    if (mode & S_IWOTH)
        permissions |= FILE_OW;
    if (mode & S_IXOTH)
        permissions |= FILE_OX;
    if (mode & S_ISVTX)
        permissions |= FILE_STICKY;
    pFile->setPermissions(permissions);

    return true;
}

static bool doChown(File *pFile, uid_t owner, gid_t group)
{
    // If we're root, changing is fine.
    size_t newOwner = pFile->getUid();
    size_t newGroup = pFile->getGid();
    if (owner != static_cast<uid_t>(-1))
    {
        newOwner = owner;
    }
    if (group != static_cast<gid_t>(-1))
    {
        newGroup = group;
    }

    // We can only chown the user if we're root.
    if (pFile->getUid() != newOwner)
    {
        User *pCurrentUser =
            Processor::information().getCurrentThread()->getParent()->getUser();
        if (pCurrentUser->getId())
        {
            SYSCALL_ERROR(NotEnoughPermissions);
            return false;
        }
    }

    // We can change the group to anything if we're root, but otherwise only
    // to a group we're a member of.
    if (pFile->getGid() != newGroup)
    {
        User *pCurrentUser =
            Processor::information().getCurrentThread()->getParent()->getUser();
        if (pCurrentUser->getId())
        {
            Group *pTargetGroup = UserManager::instance().getGroup(newGroup);
            if (!pTargetGroup->isMember(pCurrentUser))
            {
                SYSCALL_ERROR(NotEnoughPermissions);
                return false;
            }
        }
    }

    // Update the file's uid/gid now that we've checked we're allowed to.
    if (pFile->getUid() != newOwner)
    {
        pFile->setUid(newOwner);
    }

    if (pFile->getGid() != newGroup)
    {
        pFile->setGid(newGroup);
    }

    return true;
}

// NON-special-case remappings.
static struct Remapping
{
    // from must match either completely, or be followed by a "/"
    const char *from;
    const char *to;
    const char *fsname;  // certain remaps are to be reported as custom FS's to
                         // some ABIs
    bool all_abis;       // certain ABIs shouldn't normalise certain paths
    bool on_devfs;       // certain callers care about the result being on devfs
} g_Remappings[] = {
    {"/dev", "dev»", nullptr, true, true},
    {"/proc", "proc»", "proc", true, false},
    {"/bin", "/applications", nullptr, false, false},
    {"/usr/bin", "/applications", nullptr, false, false},
    {"/lib", "/libraries", nullptr, false, false},
    {"/etc", "/config", nullptr, false, false},
    {"/tmp", "scratch»", "tmpfs", true, false},
    {"/var/run", "posix-runtime»", "tmpfs", true, false},
    {nullptr, nullptr, nullptr, false, false},
};

bool normalisePath(String &nameToOpen, const char *name, bool *onDevFs)
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    bool fixFilesystemPaths = pSubsystem->getAbi() != PosixSubsystem::LinuxAbi;

    // Rebase /dev onto the devfs. /dev/tty is special.
    // Note: in all these we may need to accept the raw directory but nothing
    // more (e.g. /libfoo should not become /libraries, but /lib DOES become
    // /libraries because it has no further characters)
    if (!StringCompare(name, "/dev/tty"))
    {
        // Get controlling console, unless we have none.
        Process *pProcess =
            Processor::information().getCurrentThread()->getParent();
        if (!pProcess->getCtty())
        {
            if (onDevFs)
                *onDevFs = true;
        }

        nameToOpen = name;
        return true;
    }
    else if (!StringCompareN(name, "/@/", StringLength("/@/")))
    {
        // Absolute UNIX paths for POSIX stupidity.
        // /@/path/to/foo = /path/to/foo
        // /@/root»/applications = root»/applications
        const char *newName = name + StringLength("/@/");
        if (*newName == '/')
            ++newName;
        nameToOpen = newName;
        return true;
    }
    else
    {
        // try the remappings
        struct Remapping *remap = g_Remappings;
#if ENABLE_VERBOSE_NORMALISATION
        F_NOTICE("performing remap for '" << name << "'...");
#endif
        bool ok = false;
        while (remap->from != nullptr)
        {
            if (!(fixFilesystemPaths || remap->all_abis))
            {
#if ENABLE_VERBOSE_NORMALISATION
                F_NOTICE(
                    " -> ignoring " << remap->from
                                    << " as it is not for the current ABI");
#endif
                ++remap;
                continue;
            }

#if ENABLE_VERBOSE_NORMALISATION
            F_NOTICE(" -> check against " << remap->from);
#endif
            if (!StringCompare(name, remap->from))
            {
#if ENABLE_VERBOSE_NORMALISATION
                F_NOTICE(" -> direct remap to " << remap->to);
#endif
                nameToOpen = remap->to;
                ok = true;
                break;
            }

            // does not match directly, so we need to check for a partial match
            if (!StringCompareN(name, remap->from, StringLength(remap->from)))
            {
#if ENABLE_VERBOSE_NORMALISATION
                F_NOTICE(" -> possibly partial remap");
#endif

                // we have a partial match, but this only OK if the following
                // character is '/' to avoid incorrectly rewriting paths
                if (*(name + StringLength(remap->from)) == '/')
                {
                    // good
                    nameToOpen = remap->to;
                    nameToOpen += (name + StringLength(remap->from));
#if ENABLE_VERBOSE_NORMALISATION
                    F_NOTICE(
                        " -> indirect remap to create path '" << nameToOpen
                                                              << "'...");
#endif
                    ok = true;
                    break;
                }

// no good
#if ENABLE_VERBOSE_NORMALISATION
                NOTICE(" -> cannot use this remap as it is not actually "
                       "matching a path segment");
#endif
            }

            ++remap;
        }

        if (onDevFs && remap)
        {
            *onDevFs = remap->on_devfs;
        }

        if (!ok)
        {
            nameToOpen = name;
            return false;
        }

        return true;
    }
}

int posix_close(int fd)
{
    F_NOTICE("close(" << fd << ")");
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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
    if (ConsoleManager::instance().isConsole(pFd->file))
    {
        if (ConsoleManager::instance().isMasterConsole(pFd->file))
        {
            ConsoleManager::instance().unlockConsole(pFd->file);
        }
    }

    pSubsystem->freeFd(fd);
    return 0;
}

int posix_open(const char *name, int flags, int mode)
{
    return posix_openat(AT_FDCWD, name, flags, mode);
}

int posix_read(int fd, char *ptr, int len)
{
    F_NOTICE(
        "read(" << Dec << fd << Hex << ", " << reinterpret_cast<uintptr_t>(ptr)
                << ", " << len << ")");
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(ptr), len, PosixSubsystem::SafeWrite))
    {
        F_NOTICE("  -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Lookup this process.
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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

    if (pFd->networkImpl)
    {
        // Need to redirect to socket implementation.
        return posix_recv(fd, ptr, len, 0);
    }

    if (pFd->file->isDirectory())
    {
        SYSCALL_ERROR(IsADirectory);
        return -1;
    }

    // Are we allowed to block?
    bool canBlock = !((pFd->flflags & O_NONBLOCK) == O_NONBLOCK);

    // Handle async descriptor that is not ready for reading.
    // File::read has no mechanism for presenting such an error, other than
    // returning 0. However, a read() returning 0 is an EOF condition.
    if (!canBlock)
    {
        if (!pFd->file->select(false, 0))
        {
            SYSCALL_ERROR(NoMoreProcesses);
            F_NOTICE(" -> async and nothing available to read");
            return -1;
        }
    }

    // Prepare to handle EINTR.
    uint64_t nRead = 0;
    if (ptr && len)
    {
        pThread->setInterrupted(false);
        nRead = pFd->file->read(
            pFd->offset, len, reinterpret_cast<uintptr_t>(ptr), canBlock);
        if ((!nRead) && (pThread->wasInterrupted()))
        {
            SYSCALL_ERROR(Interrupted);
            return -1;
        }
        pFd->offset += nRead;
    }

    if (ptr && nRead)
    {
        // Need to use unsafe for String::assign so StringLength doesn't get
        // called, as this does not always end up zero-terminated.
        String debug;
        debug.assign(ptr, nRead, true);
        F_NOTICE(" -> read: '" << debug << "'");
    }

    F_NOTICE("    -> " << Dec << nRead << Hex);

    return static_cast<int>(nRead);
}

int posix_write(int fd, char *ptr, int len, bool nocheck)
{
    F_NOTICE(
        "write(" << fd << ", " << reinterpret_cast<uintptr_t>(ptr) << ", "
                 << len << ")");
    if (!nocheck &&
        !PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(ptr), len, PosixSubsystem::SafeRead))
    {
        F_NOTICE("  -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if (ptr && len > 0)
    {
        // Need to use unsafe for String::assign so StringLength doesn't get
        // called, as this does not always end up zero-terminated.
        String debug;
        debug.assign(ptr, len - 1, true);
        F_NOTICE(
            "write(" << fd << ", " << debug << ", " << len << ")");
    }

    // Lookup this process.
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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

    if (pFd->networkImpl)
    {
        // Need to redirect to socket implementation.
        return posix_send(fd, ptr, len, 0);
    }

    // Copy to kernel.
    uint64_t nWritten = 0;
    if (ptr && len)
    {
        nWritten = pFd->file->write(
            pFd->offset, len, reinterpret_cast<uintptr_t>(ptr));
        pFd->offset += nWritten;
    }

    F_NOTICE("  -> write returns " << nWritten);

    // Handle broken pipe (write of zero bytes to a pipe).
    // Note: don't send SIGPIPE if we actually tried a zero-length write.
    if (pFd->file->isPipe() && (nWritten == 0 && len > 0))
    {
        F_NOTICE("  -> write to a broken pipe");
        SYSCALL_ERROR(BrokenPipe);
        pSubsystem->threadException(pThread, Subsystem::Pipe);
        return -1;
    }

    return static_cast<int>(nWritten);
}

int posix_writev(int fd, const struct iovec *iov, int iovcnt)
{
    F_NOTICE("writev(" << fd << ", <iov>, " << iovcnt << ")");

    /// \todo check iov

    if (iovcnt <= 0)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    int totalWritten = 0;
    for (int i = 0; i < iovcnt; ++i)
    {
        F_NOTICE(
            "writev: iov[" << i << "] is @ " << iov[i].iov_base << ", "
                           << iov[i].iov_len << " bytes.");

        if (!iov[i].iov_len)
            continue;

        int r = posix_write(
            fd, reinterpret_cast<char *>(iov[i].iov_base), iov[i].iov_len,
            false);
        if (r < 0)
        {
            /// \todo fd should not be seeked any further, even if past writes
            /// succeeded
            return r;
        }

        totalWritten += r;
    }

    return totalWritten;
}

int posix_readv(int fd, const struct iovec *iov, int iovcnt)
{
    F_NOTICE("readv(" << fd << ", <iov>, " << iovcnt << ")");

    /// \todo check iov

    if (iovcnt <= 0)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    int totalRead = 0;
    for (int i = 0; i < iovcnt; ++i)
    {
        F_NOTICE(
            "readv: iov[" << i << "] is @ " << iov[i].iov_base << ", "
                          << iov[i].iov_len << " bytes.");

        if (!iov[i].iov_len)
            continue;

        int r = posix_read(
            fd, reinterpret_cast<char *>(iov[i].iov_base), iov[i].iov_len);
        if (r < 0)
        {
            /// \todo fd should not be seeked any further, even if past writes
            /// succeeded
            return r;
        }

        totalRead += r;
    }

    return totalRead;
}

off_t posix_lseek(int file, off_t ptr, int dir)
{
    F_NOTICE("lseek(" << file << ", " << ptr << ", " << dir << ")");

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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

int posix_link(char *target, char *link)
{
    return posix_linkat(AT_FDCWD, target, AT_FDCWD, link, AT_SYMLINK_FOLLOW);
}

int posix_readlink(const char *path, char *buf, unsigned int bufsize)
{
    return posix_readlinkat(AT_FDCWD, path, buf, bufsize);
}

int posix_realpath(const char *path, char *buf, size_t bufsize)
{
    F_NOTICE("realpath");

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(path), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(buf), bufsize,
              PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("realpath -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    String realPath;
    normalisePath(realPath, path);
    F_NOTICE("  -> traversing " << realPath);
    File *f = findFileWithAbiFallbacks(realPath, GET_CWD());
    if (!f)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    f = traverseSymlink(f);
    if (!f)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if (!f->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }

    String actualPath("/@/");
    actualPath += f->getFullPath(true);
    if (actualPath.length() > (bufsize - 1))
    {
        SYSCALL_ERROR(NameTooLong);
        return -1;
    }

    // File is good, copy it now.
    F_NOTICE("  -> returning " << actualPath);
    StringCopyN(buf, static_cast<const char *>(actualPath), bufsize);

    return 0;
}

int posix_unlink(char *name)
{
    return posix_unlinkat(AT_FDCWD, name, 0);
}

int posix_symlink(char *target, char *link)
{
    return posix_symlinkat(target, AT_FDCWD, link);
}

int posix_rename(const char *source, const char *dst)
{
    return posix_renameat(AT_FDCWD, source, AT_FDCWD, dst);
}

int posix_getcwd(char *buf, size_t maxlen)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(buf), maxlen,
            PosixSubsystem::SafeWrite))
    {
        F_NOTICE("getcwd -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("getcwd(" << maxlen << ")");

    File *curr = GET_CWD();

    // Absolute path syntax.
    String str("/@/");
    str += curr->getFullPath(true);

    size_t maxLength = str.length();
    if (maxLength > maxlen)
    {
        // Too long.
        SYSCALL_ERROR(BadRange);
        return -1;
    }
    StringCopyN(buf, static_cast<const char *>(str), maxLength);

    F_NOTICE(" -> " << str);

    return maxLength + 1;  // include null terminator
}

int posix_stat(const char *name, struct stat *st)
{
    F_NOTICE("stat(" << name << ") => fstatat");
    return posix_fstatat(AT_FDCWD, name, st, 0);
}

int posix_fstat(int fd, struct stat *st)
{
    F_NOTICE("fstat(" << fd << ") => fstatat");
    return posix_fstatat(fd, 0, st, AT_EMPTY_PATH);
}

int posix_lstat(char *name, struct stat *st)
{
    F_NOTICE("lstat(" << name << ") => fstatat");
    return posix_fstatat(AT_FDCWD, name, st, AT_SYMLINK_NOFOLLOW);
}

static int getdents_common(
    int fd, size_t (*set_dent)(File *, void *, size_t, size_t), void *buffer,
    int count)
{
    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd || !pFd->file)
    {
        // Error - no such file descriptor.
        F_NOTICE(" -> bad file");
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    if (!pFd->file->isDirectory())
    {
        F_NOTICE(" -> not a directory");
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }

    if (!count)
    {
        F_NOTICE(" -> count is zero");
        return 0;
    }

    // Navigate the directory tree.
    Directory *pDirectory = Directory::fromFile(pFd->file);
    size_t truePosition = pFd->offset;
    int offset = 0;
    for (; truePosition < pDirectory->getNumChildren() && offset < count;
         ++truePosition)
    {
        File *pFile = pDirectory->getChild(truePosition);
        if (!pFile)
            break;

        F_NOTICE(" -> " << pFile->getName());
        size_t reclen =
            set_dent(pFile, buffer, count - offset, truePosition + 1);
        if (reclen == 0)
        {
            // no more room
            break;
        }

        buffer = adjust_pointer(buffer, reclen);
        offset += reclen;
    }

    size_t totalCount = truePosition - pFd->offset;
    pFd->offset = truePosition;

    F_NOTICE(" -> " << offset);
    return offset;
}

static size_t
getdents_helper(File *file, void *buffer, size_t avail, size_t next_pos)
{
    struct linux_dirent *entry =
        reinterpret_cast<struct linux_dirent *>(buffer);
    char *char_buffer = reinterpret_cast<char *>(buffer);

    size_t filenameLength = file->getName().length();
    // dirent struct, filename, null terminator, and d_type
    size_t reclen = sizeof(struct linux_dirent) + filenameLength + 2;
    // do we have room for this record?
    if (avail < reclen)
    {
        // need to call again with more space available
        return 0;
    }

    entry->d_reclen = reclen;
    entry->d_off = next_pos;  /// \todo not quite correct

    entry->d_ino = file->getInode();
    if (!entry->d_ino)
    {
        entry->d_ino = ~0U;
    }

    StringCopy(entry->d_name, static_cast<const char *>(file->getName()));
    char_buffer[reclen - 2] = 0;

    char d_type = DT_UNKNOWN;
    if (file->isSymlink() || file->isPipe())
    {
        d_type = DT_LNK;
    }
    else if (file->isDirectory())
    {
        d_type = DT_DIR;
    }
    else
    {
        /// \todo also need to consider character devices
        d_type = DT_REG;
    }
    char_buffer[reclen - 1] = d_type;

    return reclen;
}

static size_t
getdents64_helper(File *file, void *buffer, size_t avail, size_t next_pos)
{
    struct dirent *entry = reinterpret_cast<struct dirent *>(buffer);

    size_t filenameLength = file->getName().length();
    size_t reclen = offsetof(struct dirent, d_name) + filenameLength +
                    1;  // needs null terminator
    // do we have room for this record?
    if (avail < reclen)
    {
        // need to call again with more space available
        return 0;
    }

    entry->d_reclen = reclen;
    entry->d_off = next_pos;

    entry->d_ino = file->getInode();
    if (!entry->d_ino)
    {
        entry->d_ino = ~0U;
    }

    StringCopy(entry->d_name, static_cast<const char *>(file->getName()));
    entry->d_name[filenameLength] = 0;

    if (file->isSymlink() || file->isPipe())
    {
        entry->d_type = DT_LNK;
    }
    else if (file->isDirectory())
    {
        entry->d_type = DT_DIR;
    }
    else
    {
        /// \todo also need to consider character devices
        entry->d_type = DT_REG;
    }

    return reclen;
}

int posix_getdents(int fd, struct linux_dirent *ents, int count)
{
    F_NOTICE("getdents(" << fd << ")");
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(ents), count,
            PosixSubsystem::SafeWrite))
    {
        F_NOTICE("getdents -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    return getdents_common(fd, getdents_helper, ents, count);
}

int posix_getdents64(int fd, struct dirent *ents, int count)
{
    F_NOTICE("getdents64(" << fd << ")");
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(ents), count,
            PosixSubsystem::SafeWrite))
    {
        F_NOTICE("getdents64 -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    return getdents_common(fd, getdents64_helper, ents, count);
}

int posix_ioctl(int fd, int command, void *buf)
{
    F_NOTICE(
        "ioctl(" << Dec << fd << ", " << Hex << command << ", "
                 << reinterpret_cast<uintptr_t>(buf) << ")");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(fd);
    if (!f)
    {
        // Error - no such FD.
        F_NOTICE("  -> ioctl for a file that doesn't exist");
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    if (!f->file)
    {
        F_NOTICE("  -> fd " << fd << " is not supposed to be ioctl'd");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    /// \todo Sanitise buf, if it has meaning for the command.

    if (f->file->supports(command))
    {
        return f->file->command(command, buf);
    }

    switch (command)
    {
        // KDGETLED
        case 0x4B31:
            F_NOTICE(" -> KDGETLED (stubbed), arg=" << buf);
            return 0;

        // KDSETLED
        case 0x4B32:
            F_NOTICE(" -> KDSETLED (stubbed), arg=" << buf);
            return 0;

        case 0x4B33:  // KDGKBTYPE
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                // US 101
                *reinterpret_cast<int *>(buf) = 0x02;
                return 0;
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        // KDSETMODE
        case 0x4b3a:
            /// \todo what do we do when switching to graphics mode?
            F_NOTICE(" -> KDSETMODE (stubbed), arg=" << buf);
            return 0;

        // KDSKBMODE
        case 0x4B45:
            F_NOTICE(" -> KDSKBMODE (stubbed), arg=" << buf);
            return 0;

        // KDKBDREP
        case 0x4B52:
            F_NOTICE(" -> KDKBDREP (stubbed), arg=" << buf);
            return 0;

        case TCGETS:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                return posix_tcgetattr(
                    fd, reinterpret_cast<struct termios *>(buf));
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TCSETS:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                return posix_tcsetattr(
                    fd, TCSANOW, reinterpret_cast<struct termios *>(buf));
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TCSETSW:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                return posix_tcsetattr(
                    fd, TCSADRAIN, reinterpret_cast<struct termios *>(buf));
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TCSETSF:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                return posix_tcsetattr(
                    fd, TCSAFLUSH, reinterpret_cast<struct termios *>(buf));
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TIOCGPGRP:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                pid_t pgrp = posix_tcgetpgrp(fd);
                *reinterpret_cast<pid_t *>(buf) = pgrp;
                return 0;
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TIOCSPGRP:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                return posix_tcsetpgrp(fd, *reinterpret_cast<pid_t *>(buf));
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TCFLSH:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                return console_flush(f->file, 0);
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TIOCGWINSZ:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                F_NOTICE(" -> TIOCGWINSZ");
                return console_getwinsize(
                    f->file, reinterpret_cast<struct winsize *>(buf));
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TIOCSWINSZ:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                const struct winsize *ws =
                    reinterpret_cast<const struct winsize *>(buf);
                F_NOTICE(
                    " -> TIOCSWINSZ " << Dec << ws->ws_col << "x" << ws->ws_row
                                      << Hex);
                return console_setwinsize(f->file, ws);
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TIOCSCTTY:
        {
            if (ConsoleManager::instance().isConsole(f->file))
            {
                F_NOTICE(" -> TIOCSCTTY");
                return console_setctty(
                    fd, reinterpret_cast<uintptr_t>(buf) == 1);
            }
            else
            {
                SYSCALL_ERROR(NotAConsole);
                return -1;
            }
        }

        case TIOCGPTN:
        {
            F_NOTICE(" -> TIOCGPTN");
            unsigned int *out = reinterpret_cast<unsigned int *>(buf);
            unsigned int result = console_getptn(fd);
            if (result < ~0U)
            {
                F_NOTICE(" -> ok, returning " << result);
                *out = result;
                return 0;
            }
            else
            {
                // console_getptn will set the syscall error
                F_NOTICE(" -> failed!");
                return -1;
            }
            break;
        }

        case FIONBIO:
        {
            F_NOTICE(" -> FIONBIO");
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

        /// \todo move this into ConsoleFile or something
        // VT_OPENQRY
        case 0x5600:
        {
            F_NOTICE(" -> VT_OPENQRY (stubbed)");

            int *ibuf = reinterpret_cast<int *>(buf);
            *ibuf = 2;  // tty2 is free (maybe)

            return 0;
        }

        // VT_GETMODE
        case 0x5601:
            {
                F_NOTICE(" -> VT_GETMODE (stubbed)");
                struct vt_mode *mode = reinterpret_cast<struct vt_mode *>(buf);
                mode->mode = VT_AUTO;
                mode->waitv = 0;
                mode->relsig = 0;
                mode->acqsig = 0;
                mode->frsig = 0;
            }
            return 0;

        // VT_SETMODE
        case 0x5602:
            F_NOTICE(" -> VT_SETMODE (stubbed)");
            return 0;

        // VT_GETSTATE
        case 0x5603:
            F_NOTICE(" -> VT_GETSTATE (stubbed)");
            return 0;

        // VT_ACTIVATE
        case 0x5606:
            /// \todo same thing as meta+F1, meta+F2 etc (switch terminal)
            F_NOTICE(" -> VT_ACTIVATE (stubbed)");
            return 0;

        // VT_WAITACTIVE
        case 0x5607:
            // no-op on Pedigree so far
            return 0;
    }

    F_NOTICE(
        "  -> invalid combination of fd " << fd << " and ioctl " << Hex
                                          << command);
    SYSCALL_ERROR(InvalidArgument);
    return -1;
}

int posix_chdir(const char *path)
{
    F_NOTICE("chdir");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(path), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE("chdir -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("chdir(" << path << ")");

    String realPath;
    normalisePath(realPath, path);

    File *dir = findFileWithAbiFallbacks(realPath, GET_CWD());
    if (!dir)
    {
        F_NOTICE("Does not exist.");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    return doChdir(dir) ? 0 : -1;
}

int posix_dup(int fd)
{
    F_NOTICE("dup(" << fd << ")");

    // grab the file descriptor pointer for the passed descriptor
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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
    FileDescriptor *f2 = new FileDescriptor(*f);
    pSubsystem->addFileDescriptor(newFd, f2);

    return static_cast<int>(newFd);
}

int posix_dup2(int fd1, int fd2)
{
    F_NOTICE("dup2(" << fd1 << ", " << fd2 << ")");

    if (fd2 < 0)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;  // EBADF
    }

    if (fd1 == fd2)
        return fd2;

    // grab the file descriptor pointer for the passed descriptor
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *f = pSubsystem->getFileDescriptor(fd1);
    if (!f)
    {
        SYSCALL_ERROR(BadFileDescriptor);
        return -1;
    }

    // Copy the descriptor.
    //
    // This will also increase the refcount *before* we close the original, else
    // we might accidentally trigger an EOF condition on a pipe! (if the write
    // refcount drops to zero)...
    FileDescriptor *f2 = new FileDescriptor(*f);
    pSubsystem->addFileDescriptor(fd2, f2);

    // According to the spec, CLOEXEC is cleared on DUP.
    f2->fdflags &= ~FD_CLOEXEC;

    return fd2;
}

int posix_mkdir(const char *name, int mode)
{
    return posix_mkdirat(AT_FDCWD, name, mode);
}

int posix_rmdir(const char *path)
{
    return posix_unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

int posix_isatty(int fd)
{
    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    FileDescriptor *pFd = pSubsystem->getFileDescriptor(fd);
    if (!pFd)
    {
        // Error - no such file descriptor.
        ERROR("isatty: no such file descriptor (" << Dec << fd << Hex << ")");
        return 0;
    }

    int result = ConsoleManager::instance().isConsole(pFd->file) ? 1 : 0;
    NOTICE("isatty(" << fd << ") -> " << result);
    return result;
}

int posix_fcntl(int fd, int cmd, void *arg)
{
    /// \todo Same as ioctl, figure out how best to sanitise input addresses
    F_NOTICE("fcntl(" << fd << ", " << cmd << ", " << arg << ")");

    // grab the file descriptor pointer for the passed descriptor
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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

    switch (cmd)
    {
        case F_DUPFD:

            if (arg)
            {
                size_t fd2 = reinterpret_cast<size_t>(arg);

                // Copy the descriptor (addFileDescriptor automatically frees
                // the old one, if needed)
                FileDescriptor *f2 = new FileDescriptor(*f);
                pSubsystem->addFileDescriptor(fd2, f2);

                // According to the spec, CLOEXEC is cleared on DUP.
                f2->fdflags &= ~FD_CLOEXEC;

                return static_cast<int>(fd2);
            }
            else
            {
                size_t fd2 = pSubsystem->getFd();

                // copy the descriptor
                FileDescriptor *f2 = new FileDescriptor(*f);
                pSubsystem->addFileDescriptor(fd2, f2);

                // According to the spec, CLOEXEC is cleared on DUP.
                f2->fdflags &= ~FD_CLOEXEC;

                return static_cast<int>(fd2);
            }
            break;

        case F_GETFD:
            F_NOTICE("  -> get fd flags");
            return f->fdflags;
        case F_SETFD:
            F_NOTICE("  -> set fd flags: " << arg);
            f->fdflags = reinterpret_cast<size_t>(arg);
            return 0;
        case F_GETFL:
            F_NOTICE("  -> get flags " << f->flflags);
            return f->flflags;
        case F_SETFL:
            F_NOTICE("  -> set flags " << arg);
            f->flflags =
                reinterpret_cast<size_t>(arg) & (O_APPEND | O_NONBLOCK | O_CLOEXEC);
            F_NOTICE("  -> new flags " << f->flflags);
            return 0;
        case F_GETLK:   // Get record-locking information
        case F_SETLK:   // Set or clear a record lock (without blocking
        case F_SETLKW:  // Set or clear a record lock (with blocking)
            F_NOTICE("  -> fcntl locks (stubbed)");
            /// \note advisory locking disabled for now
            return 0;

        default:
            WARNING("fcntl: unknown control " << cmd << " on fd " << fd);
    }

    SYSCALL_ERROR(Unimplemented);
    return -1;
}

void *posix_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    F_NOTICE("mmap");
    F_NOTICE(
        "  -> addr=" << reinterpret_cast<uintptr_t>(addr) << ", len=" << len
                     << ", prot=" << prot << ", flags=" << flags
                     << ", fildes=" << fd << ", off=" << off << ".");

    // Get the File object to map
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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
    if (sanityAddress)
    {
        if ((sanityAddress < va.getUserStart()) ||
            (sanityAddress >= va.getKernelStart()))
        {
            if (flags & MAP_FIXED)
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

    // Verify the passed length
    if (!len || (sanityAddress & (pageSz - 1)))
    {
        SYSCALL_ERROR(InvalidArgument);
        return MAP_FAILED;
    }

    // Create permission set.
    MemoryMappedObject::Permissions perms;
    if (prot & PROT_NONE)
    {
        perms = MemoryMappedObject::None;
    }
    else
    {
        // Everything implies a readable memory region.
        perms = MemoryMappedObject::Read;
        if (prot & PROT_WRITE)
            perms |= MemoryMappedObject::Write;
        if (prot & PROT_EXEC)
            perms |= MemoryMappedObject::Exec;
    }

    if (flags & MAP_ANON)
    {
        if (flags & MAP_SHARED)
        {
            F_NOTICE(
                "  -> failed (MAP_SHARED cannot be used with MAP_ANONYMOUS)");
            SYSCALL_ERROR(InvalidArgument);
            return MAP_FAILED;
        }

        MemoryMappedObject *pObject =
            MemoryMapManager::instance().mapAnon(sanityAddress, len, perms);
        if (!pObject)
        {
            /// \todo Better error?
            SYSCALL_ERROR(OutOfMemory);
            F_NOTICE("  -> failed (mapAnon)!");
            return MAP_FAILED;
        }

        F_NOTICE("  -> " << sanityAddress);

        finalAddress = reinterpret_cast<void *>(sanityAddress);
    }
    else
    {
        // Valid file passed?
        FileDescriptor *f = pSubsystem->getFileDescriptor(fd);
        if (!f)
        {
            SYSCALL_ERROR(BadFileDescriptor);
            return MAP_FAILED;
        }

        /// \todo check flags on the file descriptor (e.g. O_RDONLY shouldn't be
        /// opened writeable)

        // Grab the file to map in
        File *fileToMap = f->file;

        // Check general file permissions, open file mode aside.
        // Note: PROT_WRITE is OK for private mappings, as the backing file
        // doesn't get updated for those maps.
        if (!VFS::checkAccess(
                fileToMap, prot & PROT_READ,
                (prot & PROT_WRITE) && (flags & MAP_SHARED), prot & PROT_EXEC))
        {
            F_NOTICE(
                "  -> mmap on " << fileToMap->getFullPath()
                                << " failed due to permissions.");
            return MAP_FAILED;
        }

        F_NOTICE("mmap: file name is " << fileToMap->getFullPath());

        // Grab the MemoryMappedFile for it. This will automagically handle
        // MAP_FIXED mappings too
        bool bCopyOnWrite = (flags & MAP_SHARED) == 0;
        MemoryMappedObject *pFile = MemoryMapManager::instance().mapFile(
            fileToMap, sanityAddress, len, perms, off, bCopyOnWrite);
        if (!pFile)
        {
            /// \todo Better error?
            SYSCALL_ERROR(OutOfMemory);
            F_NOTICE("  -> failed (mapFile)!");
            return MAP_FAILED;
        }

        F_NOTICE("  -> " << sanityAddress);

        finalAddress = reinterpret_cast<void *>(sanityAddress);
    }

    // Complete
    return finalAddress;
}

int posix_msync(void *p, size_t len, int flags)
{
    F_NOTICE("msync");
    F_NOTICE(
        "  -> addr=" << p << ", len=" << len << ", flags=" << Hex << flags);

    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    size_t pageSz = PhysicalMemoryManager::getPageSize();

    // Verify the passed length
    if (!len || (addr & (pageSz - 1)))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if ((flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Make sure there's at least one object we'll touch.
    if (!MemoryMapManager::instance().contains(addr, len))
    {
        SYSCALL_ERROR(OutOfMemory);
        return -1;
    }

    if (flags & MS_INVALIDATE)
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
    F_NOTICE("  -> addr=" << p << ", len=" << len << ", prot=" << Hex << prot);

    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    size_t pageSz = PhysicalMemoryManager::getPageSize();

    // Verify the passed length
    if (!len || (addr & (pageSz - 1)))
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Make sure there's at least one object we'll touch.
    if (!MemoryMapManager::instance().contains(addr, len))
    {
        SYSCALL_ERROR(OutOfMemory);
        return -1;
    }

    // Create permission set.
    MemoryMappedObject::Permissions perms;
    if (prot & PROT_NONE)
    {
        perms = MemoryMappedObject::None;
    }
    else
    {
        // Everything implies a readable memory region.
        perms = MemoryMappedObject::Read;
        if (prot & PROT_WRITE)
            perms |= MemoryMappedObject::Write;
        if (prot & PROT_EXEC)
            perms |= MemoryMappedObject::Exec;
    }

    /// \todo EACCESS, which needs us to be able to get the File for a given
    ///       mapping (if one exists).

    MemoryMapManager::instance().setPermissions(addr, len, perms);

    return 0;
}

int posix_munmap(void *addr, size_t len)
{
    F_NOTICE(
        "munmap(" << reinterpret_cast<uintptr_t>(addr) << ", " << len << ")");

    if (!len)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    MemoryMapManager::instance().remove(reinterpret_cast<uintptr_t>(addr), len);

    return 0;
}

int posix_access(const char *name, int amode)
{
    return posix_faccessat(AT_FDCWD, name, amode, 0);
}

int posix_ftruncate(int a, off_t b)
{
    F_NOTICE("ftruncate(" << a << ", " << b << ")");

    // Grab the File pointer for this file
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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
    if (b == 0)
    {
        pFile->truncate();
        return 0;
    }
    else if (static_cast<size_t>(b) == pFile->getSize())
        return 0;
    // If we need to reduce the file size, do so
    else if (static_cast<size_t>(b) < pFile->getSize())
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
        ByteSet(nullBuffer, 0, numExtraBytes);
        NOTICE("Zeroed the buffer");
        pFile->write(
            currSize, numExtraBytes, reinterpret_cast<uintptr_t>(nullBuffer));
        NOTICE("Deleting the buffer");
        delete[] nullBuffer;
        NOTICE("Complete");
        return 0;
    }
}

int posix_fsync(int fd)
{
    F_NOTICE("fsync(" << fd << ")");

    // Grab the File pointer for this file
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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

int pedigree_get_mount(char *mount_buf, char *info_buf, size_t n)
{
    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(mount_buf), PATH_MAX,
              PosixSubsystem::SafeWrite) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(info_buf), PATH_MAX,
              PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("pedigree_get_mount -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    NOTICE("pedigree_get_mount(" << Dec << n << Hex << ")");

    typedef List<String *> StringList;
    typedef Tree<Filesystem *, List<String *> *> VFSMountTree;
    VFSMountTree &mounts = VFS::instance().getMounts();

    size_t i = 0;
    for (VFSMountTree::Iterator it = mounts.begin(); it != mounts.end(); it++)
    {
        Filesystem *pFs = it.key();
        StringList *pList = it.value();
        Disk *pDisk = pFs->getDisk();

        for (StringList::Iterator it2 = pList->begin(); it2 != pList->end();
             it2++, i++)
        {
            String mount = **it2;

            if (i == n)
            {
                String info, s;
                if (pDisk)
                {
                    pDisk->getName(s);
                    pDisk->getParent()->getName(info);
                    info += " // ";
                    info += s;
                }
                else
                    info = "no disk";

                StringCopy(mount_buf, static_cast<const char *>(mount));
                StringCopy(info_buf, static_cast<const char *>(info));

                return 0;
            }
        }
    }

    return -1;
}

int posix_chmod(const char *path, mode_t mode)
{
    return posix_fchmodat(AT_FDCWD, path, mode, 0);
}

int posix_chown(const char *path, uid_t owner, gid_t group)
{
    return posix_fchownat(AT_FDCWD, path, owner, group, 0);
}

int posix_fchmod(int fd, mode_t mode)
{
    return posix_fchmodat(fd, nullptr, mode, AT_EMPTY_PATH);
}

int posix_fchown(int fd, uid_t owner, gid_t group)
{
    return posix_fchownat(fd, nullptr, owner, group, AT_EMPTY_PATH);
}

int posix_fchdir(int fd)
{
    F_NOTICE("fchdir(" << fd << ")");

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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
    return doChdir(file) ? 0 : -1;
}

static int statvfs_doer(Filesystem *pFs, struct statvfs *buf)
{
    if (!pFs)
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
    buf->f_flag = (pFs->isReadOnly() ? ST_RDONLY : 0) |
                  ST_NOSUID;  // No suid in pedigree yet.
    buf->f_namemax = 0;

    return 0;
}

int posix_fstatvfs(int fd, struct statvfs *buf)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(buf), sizeof(struct statvfs),
            PosixSubsystem::SafeWrite))
    {
        F_NOTICE("fstatvfs -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("fstatvfs(" << fd << ")");

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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
    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(path), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(buf), sizeof(struct statvfs),
              PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("statvfs -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("statvfs(" << path << ")");

    String realPath;
    normalisePath(realPath, path);

    File *file = findFileWithAbiFallbacks(realPath, GET_CWD());
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // Symlink traversal
    file = traverseSymlink(file);
    if (!file)
        return -1;

    return statvfs_doer(file->getFilesystem(), buf);
}

int posix_utime(const char *path, const struct utimbuf *times)
{
    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(path), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          ((!times) || PosixSubsystem::checkAddress(
                           reinterpret_cast<uintptr_t>(times),
                           sizeof(struct utimbuf), PosixSubsystem::SafeRead))))
    {
        F_NOTICE("utimes -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("utime(" << path << ")");

    String realPath;
    normalisePath(realPath, path);

    File *file = findFileWithAbiFallbacks(realPath, GET_CWD());
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // Symlink traversal
    file = traverseSymlink(file);
    if (!file)
        return -1;

    if (!VFS::checkAccess(file, false, true, false))
    {
        // checkAccess does a SYSCALL_ERROR for us.
        return -1;
    }

    Time::Timestamp accessTime;
    Time::Timestamp modifyTime;
    if (times)
    {
        accessTime = times->actime * Time::Multiplier::SECOND;
        modifyTime = times->modtime * Time::Multiplier::SECOND;
    }
    else
    {
        accessTime = modifyTime = Time::getTime();
    }

    file->setAccessedTime(accessTime);
    file->setModifiedTime(modifyTime);

    return 0;
}

int posix_utimes(const char *path, const struct timeval *times)
{
    return posix_futimesat(AT_FDCWD, path, times);
}

int posix_chroot(const char *path)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(path), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE("chroot -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("chroot(" << path << ")");

    String realPath;
    normalisePath(realPath, path);

    File *file = findFileWithAbiFallbacks(realPath, GET_CWD());
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // Symlink traversal
    file = traverseSymlink(file);
    if (!file)
        return -1;

    // chroot must be a directory.
    if (!file->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    pProcess->setRootFile(file);

    return 0;
}

int posix_flock(int fd, int operation)
{
    F_NOTICE("flock(" << fd << ", " << operation << ")");
    F_NOTICE(" -> flock is a no-op stub");
    return 0;
}

static File *check_dirfd(int dirfd, int flags = 0)
{
    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        F_NOTICE("  -> No subsystem for this process!");
        return 0;
    }

    File *cwd = GET_CWD();
    if (dirfd != AT_FDCWD)
    {
        FileDescriptor *pFd = pSubsystem->getFileDescriptor(dirfd);
        if (!pFd)
        {
            F_NOTICE("  -> dirfd is a bad fd");
            SYSCALL_ERROR(BadFileDescriptor);
            return 0;
        }

        File *file = pFd->file;
        if ((flags & AT_EMPTY_PATH) == 0)
        {
            if (!file->isDirectory())
            {
                F_NOTICE("  -> dirfd is not a directory");
                SYSCALL_ERROR(NotADirectory);
                return 0;
            }
        }

        cwd = file;
    }

    return cwd;
}

int posix_openat(int dirfd, const char *pathname, int flags, mode_t mode)
{
    F_NOTICE("openat");

    File *cwd = check_dirfd(dirfd);
    if (!cwd)
    {
        return -1;
    }

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE("open -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "openat(" << dirfd << ", " << pathname << ", " << flags << ", " << Oct
                  << mode << ")");

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        F_NOTICE("  -> No subsystem for this process!");
        return -1;
    }

    // One of these three must be specified.
    if (!(CHECK_FLAG(flags, O_RDONLY) || CHECK_FLAG(flags, O_RDWR) ||
          CHECK_FLAG(flags, O_WRONLY)))
    {
        F_NOTICE("One of O_RDONLY, O_WRONLY, or O_RDWR must be passed.");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // verify the filename - don't try to open a dud file
    if (pathname[0] == 0)
    {
        F_NOTICE("  -> File does not exist (null path).");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    PosixProcess *pPosixProcess = getPosixProcess();
    if (pPosixProcess)
    {
        mode &= ~pPosixProcess->getMask();
    }

    size_t fd = pSubsystem->getFd();

    File *file = 0;

    bool onDevFs = false;
    bool openingCtty = false;
    String nameToOpen;
    normalisePath(nameToOpen, pathname, &onDevFs);
    if (nameToOpen == "/dev/tty")
    {
        openingCtty = true;

        file = pProcess->getCtty();
        if (!file)
        {
            F_NOTICE("  -> returning -1, no controlling tty");
            return -1;
        }
        else if (ConsoleManager::instance().isMasterConsole(file))
        {
            // If we happened to somehow open a master console, get its slave.
            F_NOTICE("  -> controlling terminal was not a slave");
            file = ConsoleManager::instance().getOther(file);
        }
    }

    F_NOTICE("  -> actual filename to open is '" << nameToOpen << "'");

    if (!file)
    {
        // Find file.
        file = findFileWithAbiFallbacks(nameToOpen, cwd);
    }

    bool bCreated = false;
    if (!file)
    {
        if ((flags & O_CREAT) && !onDevFs)
        {
            F_NOTICE("  {O_CREAT}");
            bool worked = VFS::instance().createFile(nameToOpen, mode, cwd);
            if (!worked)
            {
                // createFile should set the error if it fails.
                F_NOTICE("  -> File does not exist (createFile failed)");
                pSubsystem->freeFd(fd);
                return -1;
            }

            file = findFileWithAbiFallbacks(nameToOpen, cwd);
            if (!file)
            {
                F_NOTICE("  -> File does not exist (O_CREAT failed)");
                SYSCALL_ERROR(DoesNotExist);
                pSubsystem->freeFd(fd);
                return -1;
            }

            bCreated = true;
        }
        else
        {
            F_NOTICE("  -> Does not exist.");
            // Error - not found.
            SYSCALL_ERROR(DoesNotExist);
            pSubsystem->freeFd(fd);
            return -1;
        }
    }

    if (!file)
    {
        F_NOTICE("  -> File does not exist.");
        SYSCALL_ERROR(DoesNotExist);
        pSubsystem->freeFd(fd);
        return -1;
    }

    file = traverseSymlink(file);

    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        pSubsystem->freeFd(fd);
        return -1;
    }

    if (file->isDirectory() && (flags & (O_WRONLY | O_RDWR)))
    {
        // Error - is directory.
        F_NOTICE("  -> Is a directory, and O_WRONLY or O_RDWR was specified.");
        SYSCALL_ERROR(IsADirectory);
        pSubsystem->freeFd(fd);
        return -1;
    }

    if ((flags & O_CREAT) && (flags & O_EXCL) && !bCreated)
    {
        // file exists with O_CREAT and O_EXCL
        F_NOTICE("  -> File exists");
        SYSCALL_ERROR(FileExists);
        pSubsystem->freeFd(fd);
        return -1;
    }

    // O_RDONLY is zero.
    bool checkRead = (flags == O_RDONLY) || (flags & O_RDWR);

    // Handle side effects.
    File *newFile = file->open();

    // Check for the desired permissions.
    // Note: we are permitted to create a file that we cannot open for writing
    // again. It will be open for the original mode requested if it was
    // created.
    if (!bCreated)
    {
        if (!VFS::checkAccess(
                file, checkRead, flags & (O_WRONLY | O_RDWR | O_TRUNC), false))
        {
            // checkAccess does a SYSCALL_ERROR for us.
            F_NOTICE("  -> file access denied.");
            return -1;
        }
        // Check for the desired permissions.
        if ((newFile != file) &&
            (!VFS::checkAccess(
                newFile, checkRead, flags & (O_WRONLY | O_RDWR | O_TRUNC),
                false)))
        {
            // checkAccess does a SYSCALL_ERROR for us.
            F_NOTICE("  -> file access denied.");
            return -1;
        }
    }

    // ensure we tweak the correct file now
    file = newFile;

    // Check for console (as we have special handling needed here)
    if (ConsoleManager::instance().isConsole(file))
    {
        // If a master console, attempt to lock.
        if (ConsoleManager::instance().isMasterConsole(file))
        {
            // Lock the master, we now own it.
            // Or, we don't - if someone else has it open for example.
            if (!ConsoleManager::instance().lockConsole(file))
            {
                F_NOTICE("Couldn't lock pseudoterminal master");
                SYSCALL_ERROR(DeviceBusy);
                pSubsystem->freeFd(fd);
                return -1;
            }
        }
        else
        {
            // Slave - set as controlling unless noctty is set.
            if ((flags & O_NOCTTY) == 0 && !openingCtty)
            {
                F_NOTICE(
                    "  -> setting opened terminal '" << file->getName()
                                                     << "' to be controlling");
                console_setctty(file, false);
            }
        }
    }

    // Permissions were OK.
    if ((flags & O_TRUNC) &&
        ((flags & O_CREAT) || (flags & O_WRONLY) || (flags & O_RDWR)))
    {
        F_NOTICE("  -> {O_TRUNC}");
        // truncate the file
        file->truncate();
    }

    FileDescriptor *f = new FileDescriptor(
        file, (flags & O_APPEND) ? file->getSize() : 0, fd, 0, flags);
    if (f)
        pSubsystem->addFileDescriptor(fd, f);

    F_NOTICE("    -> " << fd);

    return static_cast<int>(fd);
}

int posix_mkdirat(int dirfd, const char *pathname, mode_t mode)
{
    F_NOTICE("mkdirat");

    File *cwd = check_dirfd(dirfd);
    if (!cwd)
    {
        return -1;
    }

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE("mkdirat -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("mkdirat(" << dirfd << ", " << pathname << ", " << mode << ")");

    String realPath;
    normalisePath(realPath, pathname);

    PosixProcess *pPosixProcess = getPosixProcess();
    if (pPosixProcess)
    {
        mode &= ~pPosixProcess->getMask();
    }

    bool worked = VFS::instance().createDirectory(realPath, mode, cwd);
    return worked ? 0 : -1;
}

int posix_fchownat(
    int dirfd, const char *pathname, uid_t owner, gid_t group, int flags)
{
    F_NOTICE("fchownat");

    File *cwd = check_dirfd(dirfd, flags);
    if (!cwd)
    {
        return -1;
    }

    if (!pathname)
    {
        if (flags & AT_EMPTY_PATH)
        {
            // no pathname provided but it's an empty path chownat
            pathname = "";
        }
        else
        {
            // no pathname provided!
            SYSCALL_ERROR(InvalidArgument);
            return -1;
        }
    }
    else if (!PosixSubsystem::checkAddress(
                 reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
                 PosixSubsystem::SafeRead))
    {
        F_NOTICE("chown -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "fchownat(" << dirfd << ", " << pathname << ", " << owner << ", "
                    << group << ", " << flags << ")");

    File *file = 0;

    // Is there any need to change?
    if ((owner == group) && (owner == static_cast<uid_t>(-1)))
        return 0;

    bool onDevFs = false;
    String realPath;
    normalisePath(realPath, pathname, &onDevFs);

    if (onDevFs)
    {
        // Silently ignore.
        return 0;
    }

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        F_NOTICE("  -> No subsystem for this process!");
        return -1;
    }

    // AT_EMPTY_PATH only takes effect if the pathname is actually empty
    if ((flags & AT_EMPTY_PATH) && ((pathname == 0) || (*pathname == 0)))
    {
        FileDescriptor *pFd = pSubsystem->getFileDescriptor(dirfd);
        if (!pFd)
        {
            // Error - no such file descriptor.
            SYSCALL_ERROR(BadFileDescriptor);
            return -1;
        }

        file = pFd->file;
    }
    else
    {
        file = findFileWithAbiFallbacks(realPath, cwd);
        if (!file)
        {
            SYSCALL_ERROR(DoesNotExist);
            return -1;
        }
    }

    // Read-only filesystem?
    if (file->getFilesystem()->isReadOnly())
    {
        SYSCALL_ERROR(ReadOnlyFilesystem);
        return -1;
    }

    // Symlink traversal
    if ((flags & AT_SYMLINK_NOFOLLOW) == 0)
    {
        file = traverseSymlink(file);
    }

    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    return doChown(file, owner, group) ? 0 : -1;
}

int posix_futimesat(
    int dirfd, const char *pathname, const struct timeval *times)
{
    F_NOTICE("futimesat");

    File *cwd = check_dirfd(dirfd);
    if (!cwd)
    {
        return -1;
    }

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          ((!times) ||
           PosixSubsystem::checkAddress(
               reinterpret_cast<uintptr_t>(times), sizeof(struct timeval) * 2,
               PosixSubsystem::SafeRead))))
    {
        F_NOTICE("utimes -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("futimesat(" << dirfd << ", " << pathname << ", " << times << ")");

    String realPath;
    normalisePath(realPath, pathname);

    File *file = findFileWithAbiFallbacks(realPath, cwd);
    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // Symlink traversal
    file = traverseSymlink(file);
    if (!file)
        return -1;

    if (!VFS::checkAccess(file, false, true, false))
    {
        // checkAccess does a SYSCALL_ERROR for us.
        return -1;
    }

    Time::Timestamp accessTime;
    Time::Timestamp modifyTime;
    if (times)
    {
        struct timeval access = times[0];
        struct timeval modify = times[1];

        accessTime = access.tv_sec * Time::Multiplier::SECOND;
        accessTime += access.tv_usec * Time::Multiplier::MICROSECOND;

        modifyTime = modify.tv_sec * Time::Multiplier::SECOND;
        modifyTime += modify.tv_usec * Time::Multiplier::MICROSECOND;
    }
    else
    {
        accessTime = modifyTime = Time::getTimeNanoseconds();
    }

    file->setAccessedTime(accessTime);
    file->setModifiedTime(modifyTime);

    return 0;
}

int posix_unlinkat(int dirfd, const char *pathname, int flags)
{
    F_NOTICE("unlinkat");

    File *cwd = check_dirfd(dirfd);
    if (!cwd)
    {
        return -1;
    }

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE("unlink -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("unlinkat(" << dirfd << ", " << pathname << ", " << flags << ")");

    String realPath;
    normalisePath(realPath, pathname);

    File *pFile = findFileWithAbiFallbacks(realPath, cwd);
    if (!pFile)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }
    else if (pFile->isDirectory() && ((flags & AT_REMOVEDIR) == 0))
    {
        // unless AT_REMOVEDIR is specified, we won't rmdir
        SYSCALL_ERROR(NotEnoughPermissions);
        return -1;
    }

    // remove() checks permissions to ensure we can delete the file.
    if (VFS::instance().remove(realPath, GET_CWD()))
    {
        return 0;
    }
    else
    {
        return -1;
    }
}

int posix_renameat(
    int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
    F_NOTICE("renameat");

    File *oldcwd = check_dirfd(olddirfd);
    if (!oldcwd)
    {
        return -1;
    }

    File *newcwd = check_dirfd(newdirfd);
    if (!newcwd)
    {
        return -1;
    }

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(oldpath), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(newpath), PATH_MAX,
              PosixSubsystem::SafeRead)))
    {
        F_NOTICE("rename -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "renameat(" << olddirfd << ", " << oldpath << ", " << newdirfd << ", "
                    << newpath << ")");

    String realSource;
    String realDestination;
    normalisePath(realSource, oldpath);
    normalisePath(realDestination, newpath);

    File *src = findFileWithAbiFallbacks(realSource, oldcwd);
    File *dest = findFileWithAbiFallbacks(realDestination, newcwd);

    if (!src)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // traverse symlink
    src = traverseSymlink(src);
    if (!src)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if (dest)
    {
        // traverse symlink
        dest = traverseSymlink(dest);
        if (!dest)
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
        VFS::instance().createFile(realDestination, 0777, newcwd);
        dest = findFileWithAbiFallbacks(realDestination, newcwd);
        if (!dest)
        {
            // Failed to create the file?
            return -1;
        }
    }

    // Gay algorithm.
    uint8_t *buf = new uint8_t[src->getSize()];
    src->read(0, src->getSize(), reinterpret_cast<uintptr_t>(buf));
    dest->truncate();
    dest->write(0, src->getSize(), reinterpret_cast<uintptr_t>(buf));
    VFS::instance().remove(realSource, oldcwd);
    delete[] buf;

    return 0;
}

int posix_linkat(
    int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
    int flags)
{
    F_NOTICE("linkat");

    File *oldcwd = check_dirfd(olddirfd, flags);
    if (!oldcwd)
    {
        return -1;
    }

    File *newcwd = check_dirfd(newdirfd);
    if (!newcwd)
    {
        return -1;
    }

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(oldpath), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(newpath), PATH_MAX,
              PosixSubsystem::SafeRead)))
    {
        F_NOTICE("link -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "linkat(" << olddirfd << ", " << oldpath << ", " << newdirfd << ", "
                  << newpath << ", " << flags << ")");

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    // Try and find the target.
    String realTarget;
    String realLink;
    normalisePath(realTarget, oldpath);
    normalisePath(realLink, newpath);

    File *pTarget = 0;
    if ((flags & AT_EMPTY_PATH) && ((oldpath == 0) || (*oldpath == 0)))
    {
        FileDescriptor *pFd = pSubsystem->getFileDescriptor(olddirfd);
        if (!pFd)
        {
            // Error - no such file descriptor.
            SYSCALL_ERROR(BadFileDescriptor);
            return -1;
        }

        pTarget = pFd->file;
    }
    else
    {
        pTarget = findFileWithAbiFallbacks(realTarget, oldcwd);
    }

    if (flags & AT_SYMLINK_FOLLOW)
    {
        pTarget = traverseSymlink(pTarget);
    }

    if (!pTarget)
    {
        F_NOTICE(" -> target '" << realTarget << "' did not exist.");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    bool result = VFS::instance().createLink(realLink, pTarget, newcwd);

    if (!result)
    {
        F_NOTICE(" -> failed to create link");
        return -1;
    }

    F_NOTICE(" -> ok");
    return 0;
}

int posix_symlinkat(const char *oldpath, int newdirfd, const char *newpath)
{
    F_NOTICE("symlinkat");

    File *cwd = check_dirfd(newdirfd);
    if (!cwd)
    {
        return -1;
    }

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(oldpath), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(newpath), PATH_MAX,
              PosixSubsystem::SafeRead)))
    {
        F_NOTICE("symlink -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "symlinkat(" << oldpath << ", " << newdirfd << ", " << newpath << ")");

    bool worked =
        VFS::instance().createSymlink(String(newpath), String(oldpath), cwd);
    if (worked)
        return 0;
    else
        ERROR("Symlink failed for `" << newpath << "' -> `" << oldpath << "'");
    return -1;
}

int posix_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz)
{
    F_NOTICE("readlinkat");

    File *cwd = check_dirfd(dirfd);
    if (!cwd)
    {
        return -1;
    }

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(buf), bufsiz,
              PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("readlink -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "readlinkat(" << dirfd << ", " << pathname << ", " << buf << ", "
                      << bufsiz << ")");

    String realPath;
    normalisePath(realPath, pathname);

    File *f = findFileWithAbiFallbacks(realPath, cwd);
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

    return Symlink::fromFile(f)->followLink(buf, bufsiz);
}

int posix_fchmodat(int dirfd, const char *pathname, mode_t mode, int flags)
{
    F_NOTICE("fchmodat");

    File *cwd = check_dirfd(dirfd, flags);
    if (!cwd)
    {
        return -1;
    }

    if (!pathname)
    {
        if (flags & AT_EMPTY_PATH)
        {
            // no pathname provided but it's an empty path chmodat
            pathname = "";
        }
        else
        {
            // no pathname provided!
            SYSCALL_ERROR(InvalidArgument);
            return -1;
        }
    }
    else if (!PosixSubsystem::checkAddress(
                 reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
                 PosixSubsystem::SafeRead))
    {
        F_NOTICE("chmod -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "fchmodat(" << dirfd << ", " << pathname << ", " << Oct << mode << Hex
                    << ", " << flags << ")");

    if (mode == static_cast<mode_t>(-1))
    {
        F_NOTICE(" -> invalid mode");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    bool onDevFs = false;
    String realPath;
    normalisePath(realPath, pathname, &onDevFs);

    if (onDevFs)
    {
        // Silently ignore.
        return 0;
    }

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        F_NOTICE("  -> No subsystem for this process!");
        return -1;
    }

    // AT_EMPTY_PATH only takes effect if the pathname is actually empty
    File *file = 0;
    if ((flags & AT_EMPTY_PATH) && ((pathname == 0) || (*pathname == 0)))
    {
        FileDescriptor *pFd = pSubsystem->getFileDescriptor(dirfd);
        if (!pFd)
        {
            // Error - no such file descriptor.
            SYSCALL_ERROR(BadFileDescriptor);
            return -1;
        }

        file = pFd->file;
    }
    else
    {
        file = findFileWithAbiFallbacks(realPath, GET_CWD());
        if (!file)
        {
            SYSCALL_ERROR(DoesNotExist);
            return -1;
        }
    }

    // Read-only filesystem?
    if (file->getFilesystem()->isReadOnly())
    {
        SYSCALL_ERROR(ReadOnlyFilesystem);
        return -1;
    }

    // Symlink traversal
    if ((flags & AT_SYMLINK_NOFOLLOW) == 0)
    {
        file = traverseSymlink(file);
    }

    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    return doChmod(file, mode) ? 0 : -1;
}

int posix_faccessat(int dirfd, const char *pathname, int mode, int flags)
{
    F_NOTICE("faccessat");

    File *cwd = check_dirfd(dirfd);
    if (!cwd)
    {
        return -1;
    }

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE("access -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "faccessat(" << dirfd << ", " << pathname << ", " << mode << ", "
                     << flags << ")");

    if (!pathname)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    String realPath;
    normalisePath(realPath, pathname);

    // Grab the file
    File *file = findFileWithAbiFallbacks(realPath, cwd);

    if ((flags & AT_SYMLINK_NOFOLLOW) == 0)
    {
        file = traverseSymlink(file);
    }

    if (!file)
    {
        F_NOTICE("  -> '" << realPath << "' does not exist");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    // If we're only checking for existence, we're done here.
    if (mode == F_OK)
    {
        F_NOTICE("  -> ok");
        return 0;
    }

    if (!VFS::checkAccess(file, mode & R_OK, mode & W_OK, mode & X_OK))
    {
        // checkAccess does a SYSCALL_ERROR for us.
        F_NOTICE("  -> not ok");
        return -1;
    }

    F_NOTICE("  -> ok");
    return 0;
}

int posix_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags)
{
    F_NOTICE("fstatat");

    File *cwd = check_dirfd(dirfd, flags);
    if (!cwd)
    {
        F_NOTICE(" -> current working directory could not be determined");
        return -1;
    }

    /// \todo also check pathname
    if (!((!pathname) ||
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
              PosixSubsystem::SafeRead) &&
              PosixSubsystem::checkAddress(
                  reinterpret_cast<uintptr_t>(buf), sizeof(struct stat),
                  PosixSubsystem::SafeWrite)))
    {
        F_NOTICE("fstat -> invalid address");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE(
        "fstatat(" << dirfd << ", " << (pathname ? pathname : "(n/a)") << ", "
                   << buf << ", " << flags << ")");

    F_NOTICE("  -> cwd=" << cwd->getFullPath());

    if (!buf)
    {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    // Lookup this process.
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
    if (!pSubsystem)
    {
        ERROR("No subsystem for this process!");
        return -1;
    }

    // AT_EMPTY_PATH only takes effect if the pathname is actually empty
    File *file = 0;
    if ((flags & AT_EMPTY_PATH) && ((pathname == 0) || (*pathname == 0)))
    {
        FileDescriptor *pFd = pSubsystem->getFileDescriptor(dirfd);
        if (!pFd)
        {
            // Error - no such file descriptor.
            SYSCALL_ERROR(BadFileDescriptor);
            return -1;
        }

        file = pFd->file;
    }
    else
    {
        String realPath;
        normalisePath(realPath, pathname);

        F_NOTICE(" -> finding file with real path " << realPath << " in " << cwd->getFullPath());

        file = findFileWithAbiFallbacks(realPath, cwd);
        if (!file)
        {
            SYSCALL_ERROR(DoesNotExist);
            F_NOTICE(" -> unable to find '" << realPath << "' here");
            return -1;
        }
    }

    if ((flags & AT_SYMLINK_NOFOLLOW) == 0)
    {
        file = traverseSymlink(file);
    }

    if (!file)
    {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if (!doStat(0, file, buf))
    {
        return -1;
    }

    F_NOTICE("    -> Success");
    return 0;
}

/** Do-er for getting extended attributes. If filepath is null, fd is used. */
ssize_t doGetXattr(
    const char *filepath, int fd, const char *name, void *value, size_t size,
    bool follow_links)
{
    /// \todo implement?
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
}

ssize_t
posix_getxattr(const char *path, const char *name, void *value, size_t size)
{
    return doGetXattr(path, -1, name, value, size, true);
}

ssize_t
posix_lgetxattr(const char *path, const char *name, void *value, size_t size)
{
    return doGetXattr(path, -1, name, value, size, false);
}

ssize_t posix_fgetxattr(int fd, const char *name, void *value, size_t size)
{
    return doGetXattr(nullptr, fd, name, value, size, true);
}

int posix_mknod(const char *pathname, mode_t mode, dev_t dev)
{
    F_NOTICE("mknod");
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(pathname), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE(" -> invalid address for pathname");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("mknod(" << pathname << ", " << mode << ", " << dev << ")");

    File *targetFile = findFileWithAbiFallbacks(String(pathname), GET_CWD());
    if (targetFile)
    {
        F_NOTICE(" -> already exists");
        SYSCALL_ERROR(FileExists);
        return -1;
    }

    // Open parent directory if we can.
    const char *parentDirectory = DirectoryName(pathname);
    const char *baseName = BaseName(pathname);
    if (!baseName)
    {
        F_NOTICE(" -> no filename provided");
        SYSCALL_ERROR(DoesNotExist);
        delete[] parentDirectory;
        return -1;
    }

    PointerGuard<const char> guard1(parentDirectory, true);
    PointerGuard<const char> guard2(baseName, true);

    // support mknod("foo") as well as mknod("/path/to/foo")
    File *parentFile = GET_CWD();
    if (parentDirectory)
    {
        String parentPath;
        normalisePath(parentPath, parentDirectory, nullptr);
        NOTICE("finding parent directory " << parentDirectory << "...");
        NOTICE(" -> " << parentPath << "...");
        parentFile = findFileWithAbiFallbacks(parentPath, GET_CWD());

        parentFile = traverseSymlink(parentFile);
        if (!parentFile)
        {
            // traverseSymlink sets error for us
            F_NOTICE(" -> symlink traversal failed");
            return -1;
        }
    }
    else
    {
        NOTICE("NO parent directory was found for path " << pathname);
    }

    if (!parentFile->isDirectory())
    {
        SYSCALL_ERROR(NotADirectory);
        F_NOTICE(" -> target parent is not a directory");
        return -1;
    }

    Directory *parentDir = Directory::fromFile(parentFile);

    if ((mode & S_IFIFO) == S_IFIFO)
    {
        // Need to create a FIFO (i.e. named pipe).
        Pipe *pipe = new Pipe(
            String(baseName), 0, 0, 0, 0, parentDir->getFilesystem(), 0,
            parentDir);
        parentDir->addEphemeralFile(pipe);

        F_NOTICE(" -> fifo/pipe '" << baseName << "' created!");
        F_NOTICE(" -> full path is " << pipe->getFullPath(true));
    }
    else
    {
        SYSCALL_ERROR(Unimplemented);
        F_NOTICE(" -> unimplemented mode requested");
        return -1;
    }

    return 0;
}

static int do_statfs(File *file, struct statfs *buf)
{
    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(buf), sizeof(struct statfs),
            PosixSubsystem::SafeWrite))
    {
        F_NOTICE(" -> invalid address for buf [" << buf << "]");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    if (!file)
    {
        F_NOTICE(" -> file does not exist");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    Filesystem *pFs = file->getFilesystem();

    /// \todo this is all terrible
    bool bFilled = false;
    if (pFs == g_pDevFs)
    {
        F_NOTICE(" -> file '" << file->getName() << "' is on devfs");

        // Special handling for devfs
        if (file->getName() == "pts")
        {
            F_NOTICE(" -> filling statfs struct with /dev/pts data");
            ByteSet(buf, 0, sizeof(*buf));
            buf->f_type = 0x1CD1;  // DEVPTS_SUPER_MAGIC
            buf->f_bsize = 4096;
            buf->f_namelen = PATH_MAX;
            buf->f_frsize = 4096;
            bFilled = true;
        }
    }

    if (!bFilled)
    {
        /// \todo none of this is really correct...
        F_NOTICE(" -> filling statfs struct with ext2 data");
        ByteSet(buf, 0, sizeof(*buf));
        buf->f_type = 0xEF53;  // EXT2_SUPER_MAGIC
        if (pFs->getDisk())
        {
            buf->f_bsize = pFs->getDisk()->getBlockSize();
            buf->f_blocks =
                pFs->getDisk()->getSize() / pFs->getDisk()->getBlockSize();
        }
        else
        {
            buf->f_bsize = 4096;
            buf->f_blocks = 0x100000;  // incorrect...
        }
        buf->f_bfree = buf->f_blocks;
        buf->f_bavail = buf->f_blocks;
        buf->f_files = 1234;
        buf->f_ffree = 5678;
        buf->f_namelen = PATH_MAX;
        buf->f_frsize = 0;
    }

    F_NOTICE(" -> ok");
    return 0;
}

int posix_statfs(const char *path, struct statfs *buf)
{
    F_NOTICE("statfs");

    if (!PosixSubsystem::checkAddress(
            reinterpret_cast<uintptr_t>(path), PATH_MAX,
            PosixSubsystem::SafeRead))
    {
        F_NOTICE(" -> invalid address for path");
        SYSCALL_ERROR(InvalidArgument);
        return -1;
    }

    F_NOTICE("statfs(" << path << ")");
    String normalisedPath;
    normalisePath(normalisedPath, path);
    F_NOTICE(" -> actually performing statfs on " << normalisedPath);
    File *file = findFileWithAbiFallbacks(normalisedPath, GET_CWD());

    return do_statfs(file, buf);
}

int posix_fstatfs(int fd, struct statfs *buf)
{
    F_NOTICE("fstatfs(" << fd << ")");

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    PosixSubsystem *pSubsystem =
        reinterpret_cast<PosixSubsystem *>(pProcess->getSubsystem());
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

    return do_statfs(pFd->file, buf);
}

int posix_mount(
    const char *src, const char *tgt, const char *fs, size_t flags,
    const void *data)
{
    F_NOTICE("mount");

    if (!(PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(src), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(tgt), PATH_MAX,
              PosixSubsystem::SafeRead) &&
          PosixSubsystem::checkAddress(
              reinterpret_cast<uintptr_t>(fs), PATH_MAX,
              PosixSubsystem::SafeRead)))
    {
        F_NOTICE(" -> invalid address");
        SYSCALL_ERROR(BadAddress);
        return -1;
    }

    String source(src);
    String target(tgt);
    String fstype(fs);

    F_NOTICE(
        "mount(" << source << ", " << target << ", " << fstype << ", " << Hex
                 << flags << ", " << data << ")");

    // Is the target a valid directory?
    String targetNormalised;
    normalisePath(targetNormalised, tgt);
    File *targetFile = findFileWithAbiFallbacks(targetNormalised, nullptr);
    if (!targetFile)
    {
        F_NOTICE(" -> target does not exist");
        SYSCALL_ERROR(DoesNotExist);
        return -1;
    }

    if (!targetFile->isDirectory())
    {
        F_NOTICE(" -> target not a directory");
        SYSCALL_ERROR(NotADirectory);
        return -1;
    }

    Directory *targetDir = Directory::fromFile(targetFile);

    // Check for special filesystems.
    if (fstype == "proc")
    {
        F_NOTICE(" -> adding another procfs mount");

        Filesystem *pFs = VFS::instance().lookupFilesystem(String("proc"));
        if (!pFs)
        {
            SYSCALL_ERROR(DeviceDoesNotExist);
            return -1;
        }

        if (targetFile == pFs->getRoot() ||
            targetDir->getReparsePoint() == pFs->getRoot())
        {
            // Already mounted here?
            return 0;
        }

        // Add reparse point.
        targetDir->setReparsePoint(Directory::fromFile(pFs->getRoot()));
        return 0;
    }
    else if (fstype == "tmpfs")
    {
        F_NOTICE(" -> creating new tmpfs");

        RamFs *pRamFs = new RamFs;
        pRamFs->initialise(0);

        targetDir->setReparsePoint(Directory::fromFile(pRamFs->getRoot()));
        return 0;
    }
    else
    {
        F_NOTICE(" -> unsupported fstype");
        SYSCALL_ERROR(DeviceDoesNotExist);
        return -1;
    }

    SYSCALL_ERROR(PermissionDenied);
    return -1;
}

void generate_mtab(String &result)
{
    result = "";

    struct Remapping *remap = g_Remappings;
    while (remap->from != nullptr)
    {
        if (remap->fsname)
        {
            String line;
            line.Format(
                "%s %s %s rw 0 0\n", remap->to, remap->from, remap->fsname);

            result += line;
        }

        ++remap;
    }

    // Add root filesystem.
    Filesystem *pRootFs = VFS::instance().lookupFilesystem(String("root"));
    if (pRootFs)
    {
        /// \todo fix disk path to use rawfs
        /// \todo fix filesystem identification string
        String line;
        line.Format("/dev/sda1 / ext2 rw 0 0\n");

        result += line;
    }

    F_NOTICE("generated mtab:\n" << result);
}

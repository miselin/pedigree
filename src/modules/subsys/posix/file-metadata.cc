/* Copyright (c) 2026, Pedigree Developers. */
#include "file-metadata.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/syscallError.h"

#include "DevFs-block.h"
#include "DevFs.h"
#include "logging.h"
#include "modules/system/console/Console.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"
#include <sys/stat.h>

bool posix_stat_file(const char* name, File* pFile, struct stat* st) {
  static ConstantString nullName = MakeConstantString("null");
  int mode = 0;
  /// \todo files really should be able to expose their "type"...
  if (ConsoleManager::instance().isConsole(pFile) ||
      (pFile->getFilesystem() == g_pDevFs && pFile->getName() == nullName)) {
    F_NOTICE("    -> S_IFCHR");
    mode = S_IFCHR;
  } else if (pFile->isBlockDevice()) {
    mode = S_IFBLK;
  } else if (pFile->isDirectory()) {
    F_NOTICE("    -> S_IFDIR");
    mode = S_IFDIR;
  } else if (pFile->isSymlink()) {
    F_NOTICE("    -> S_IFLNK");
    mode = S_IFLNK;
  } else if (pFile->isPipe() || pFile->isFifo()) {
    F_NOTICE("    -> S_FIFO");
    mode = S_IFIFO;
  } else if (pFile->isSocket()) {
    F_NOTICE("    -> S_SOCK");
    mode = S_IFSOCK;
  } else {
    F_NOTICE("    -> S_IFREG");
    mode = S_IFREG;
  }

  // Clear any cruft in the stat structure before we fill it.
  ByteSet(st, 0, sizeof(*st));

  const File::Attributes attributes = pFile->getAttributes();
  uint32_t permissions = attributes.permissions;
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

  Filesystem* pFs = pFile->getFilesystem();

  st->st_dev = static_cast<short>(reinterpret_cast<uintptr_t>(pFile->getFilesystem()));
  VFS::MountOperation mount;
  if (VFS::instance().acquireMount(pFs, mount) && mount.filesystem()->getDisk()) {
    if (mount.id() > PosixBlock::MaximumMinor) {
      SYSCALL_ERROR(ValueTooLarge);
      return false;
    }
    st->st_dev = PosixBlock::encode(PosixBlock::MountedMajor, mount.id());
  }
  F_NOTICE("    -> " << st->st_dev);
  st->st_ino = attributes.inode ? attributes.inode : pFile->getInode();
  F_NOTICE("    -> " << st->st_ino);
  st->st_mode = mode;
  st->st_nlink = attributes.links;
  st->st_uid = attributes.uid;
  st->st_gid = attributes.gid;
  F_NOTICE("    -> uid=" << Dec << st->st_uid);
  F_NOTICE("    -> gid=" << Dec << st->st_gid);
  st->st_rdev = pFile->isBlockDevice() ? pFile->deviceNumber() : 0;
  st->st_size = attributes.size;
  F_NOTICE("    -> " << st->st_size);
  st->st_atime = attributes.accessed;
  st->st_mtime = attributes.modified;
  st->st_ctime = attributes.changed;
  st->st_blksize = pFile->getBlockSize();
  st->st_blocks = attributes.blocks;

  // Special fixups
  if (pFs == g_pDevFs) {
    if (pFile->getName() == nullName) {
      F_NOTICE("/dev/null, fixing st_rdev");
      // major/minor device numbers
      st->st_rdev = 0x0103;
    } else if (ConsoleManager::instance().isConsole(pFile)) {
      /// \todo assumption here
      ConsoleFile* pConsole = static_cast<ConsoleFile*>(pFile);
      st->st_rdev = 0x8800 | pConsole->getConsoleNumber();
    }
  }

  return true;
}

bool posix_chmod_file(File* pFile, mode_t mode) {
  // Privilege-changing mode bits have no complete execution policy yet.
  if (mode & (S_ISUID | S_ISGID)) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  FilesystemCredentials credentials;
  if (!Process::currentFilesystemCredentials(credentials) ||
      (credentials.uid != pFile->getUid() && credentials.uid != 0)) {
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

bool posix_chown_file(File* pFile, uid_t owner, gid_t group) {
  FilesystemCredentials credentials;
  if (!Process::currentFilesystemCredentials(credentials)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  auto attributes = pFile->getAttributes();
  const uint32_t newOwner = owner == UINT32_MAX ? attributes.uid : owner;
  const uint32_t newGroup = group == UINT32_MAX ? attributes.gid : group;
  if (credentials.uid != 0 && (credentials.uid != attributes.uid || newOwner != attributes.uid ||
                               (newGroup != attributes.gid && !credentials.inGroup(newGroup)))) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  if (owner == UINT32_MAX && group == UINT32_MAX) {
    pFile->setTimes(0, 0, false, false);
    return true;
  }
  return pFile->setOwnership(newOwner, newGroup, owner != UINT32_MAX, group != UINT32_MAX);
}

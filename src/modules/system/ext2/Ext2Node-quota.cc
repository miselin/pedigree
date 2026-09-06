/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"

#include "Ext2Directory.h"
#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Symlink.h"
#include "ext2.h"

bool Ext2Node::changeInodeOwnership(size_t uid, size_t gid, bool changeUid, bool changeGid) {
  LockGuard<Mutex> inode(m_State->writebackLock);
  LockGuard<Mutex> allocation(m_pExt2Fs->m_WriteLock);
  if (m_pExt2Fs->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  if (m_State->quotaFile) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  if ((changeUid && uid > 0xffffffffULL) || (changeGid && gid > 0xffffffffULL)) {
    SYSCALL_ERROR(ValueTooLarge);
    return false;
  }
  if (!m_pExt2Fs->prepareInodeWrite(m_InodeNumber))
    return false;
  auto status = m_pExt2Fs->prepareQuotaInodeLocked(m_InodeNumber);
  if (status == QuotaStatus::Success)
    status = m_pExt2Fs->m_Quota.transfer(m_InodeNumber, changeUid ? uid : Ext2Owner::uid(*m_pInode),
                                         changeGid ? gid : Ext2Owner::gid(*m_pInode));
  if (!m_pExt2Fs->quotaSucceeded(status))
    return false;
  if (changeUid)
    Ext2Owner::setUid(*m_pInode, uid);
  if (changeGid)
    Ext2Owner::setGid(*m_pInode, gid);
  m_pInode->i_ctime = HOST_TO_LITTLE32(Time::getTime());
  m_pExt2Fs->writeInode(m_InodeNumber);
  return true;
}

bool Ext2File::changeOwnership(size_t uid, size_t gid, bool changeUid, bool changeGid) {
  return changeInodeOwnership(uid, gid, changeUid, changeGid);
}
bool Ext2Directory::changeOwnership(size_t uid, size_t gid, bool changeUid, bool changeGid) {
  return changeInodeOwnership(uid, gid, changeUid, changeGid);
}
bool Ext2Symlink::changeOwnership(size_t uid, size_t gid, bool changeUid, bool changeGid) {
  return changeInodeOwnership(uid, gid, changeUid, changeGid);
}

bool Ext2File::allowResize(size_t, size_t) {
  if (__atomic_load_n(&m_State->quotaFile, __ATOMIC_ACQUIRE)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  return true;
}

bool Ext2File::allowPhysicalPage() const {
  return !__atomic_load_n(&m_State->quotaFile, __ATOMIC_ACQUIRE);
}

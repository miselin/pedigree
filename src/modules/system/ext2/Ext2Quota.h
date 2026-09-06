/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_EXT2_QUOTA_H
#define PEDIGREE_EXT2_QUOTA_H

#include "pedigree/kernel/utilities/Tree.h"

#include "modules/system/vfs/QuotaTable.h"

/** Every method requires the filesystem allocation lock. */
class Ext2QuotaLedger {
 public:
  struct InodeCharge {
    uint32_t uid = 0, gid = 0;
    uint64_t bytes = 0;
    bool exempt = false;
  };

  ~Ext2QuotaLedger();
  InodeCharge* find(uint32_t inode) const;
  QuotaStatus track(uint32_t inode, uint32_t uid, uint32_t gid, uint64_t bytes);
  QuotaStatus create(uint32_t inode, uint32_t uid, uint32_t gid);
  QuotaStatus reserve(uint32_t inode, uint64_t bytes);
  void refund(uint32_t inode, uint64_t bytes);
  void forget(uint32_t inode);
  QuotaStatus transfer(uint32_t inode, uint32_t uid, uint32_t gid);
  QuotaStatus exempt(uint32_t inode, bool exempt);
  QuotaStatus enable(QuotaType type, QuotaTable& loaded, uint32_t maximumId = 0xffffffffU);
  void disable(QuotaType type);
  bool enabled(QuotaType type) const;
  QuotaTable& table(QuotaType type);
  const Tree<uint32_t, InodeCharge*>& inodes() const {
    return m_Inodes;
  }

 private:
  QuotaStatus prepareCharge(uint32_t uid, uint32_t gid, uint64_t bytes, uint64_t inodes,
                            bool enforce);
  void charge(uint32_t uid, uint32_t gid, uint64_t bytes, uint64_t inodes);
  void refund(uint32_t uid, uint32_t gid, uint64_t bytes, uint64_t inodes);
  Tree<uint32_t, InodeCharge*> m_Inodes;
  QuotaTable m_Tables[2];
  bool m_Enabled[2] = {};
  uint32_t m_MaximumId[2] = {0xffffffffU, 0xffffffffU};
};

#endif

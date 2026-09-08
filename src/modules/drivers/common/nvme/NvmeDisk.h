/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef NVME_DISK_H
#define NVME_DISK_H
#include "modules/drivers/common/scsi/ScsiDisk.h"
class NvmeController;
class EXPORTED_PUBLIC NvmeDisk final : public ScsiDisk {
 public:
  NvmeDisk(NvmeController* controller, uint32_t nsid);
  ~NvmeDisk() override;
  bool initialise();
  void getName(String& name) override;
  size_t getSize() const override {
    return m_Bytes;
  }
  size_t getBlockCount() const override {
    return m_Blocks;
  }
  size_t getBlockSize() const override;
  size_t getNativeBlockSize() const override {
    return m_BlockBytes;
  }
  uint64_t doRead(uint64_t location) override;
  uint64_t doWrite(uint64_t location) override;
  uint64_t doWriteDirect(uint64_t location, uintptr_t page) override;
  uint64_t doSync(uint64_t location) override;
  NvmeController* controller() const {
    return m_Controller;
  }
  uint32_t namespaceId() const {
    return m_Nsid;
  }

 private:
  size_t validPageLength(uint64_t location) const;
  NvmeController* m_Controller;
  const uint32_t m_Nsid;
  size_t m_Blocks;
  size_t m_Bytes;
  size_t m_BlockBytes;
};
#endif

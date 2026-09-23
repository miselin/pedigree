/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef VIRTIO_BLK_DISK_H
#define VIRTIO_BLK_DISK_H

#include "modules/drivers/common/scsi/ScsiDisk.h"

class VirtioBlkController;

class EXPORTED_PUBLIC VirtioBlkDisk final : public ScsiDisk {
 public:
  explicit VirtioBlkDisk(VirtioBlkController* controller);
  ~VirtioBlkDisk() override;

  void getName(String& name) override;
  size_t getSize() const override;
  size_t getBlockCount() const override;
  size_t getBlockSize() const override;
  size_t getNativeBlockSize() const override;
  bool writeFrom(uint64_t location, const void* buffer, size_t length) override;
  bool writeFromBatch(WriteBuffer* buffers, size_t count) override;
  void write(uint64_t location) override;
  bool zero(uint64_t location, size_t length) override;

  uint64_t doRead(uint64_t location) override;
  uint64_t doWrite(uint64_t location) override;
  uint64_t doWriteDirect(uint64_t location, uintptr_t page) override;
  uint64_t doSync(uint64_t location) override;

 protected:
  BufferView acquireView(uint64_t location, bool writable, uint64_t& token) override;
  bool supportsBufferTransfers() const override {
    return true;
  }
  bool transferBuffer(uint64_t location, void* buffer, size_t length, bool writing) override;

 private:
  friend class VirtioBlkController;
  size_t validPageLength(uint64_t location) const;

  VirtioBlkController* m_Controller;
};

#endif

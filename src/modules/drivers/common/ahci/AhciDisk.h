/*
 * Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC
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

#ifndef AHCI_DISK_H
#define AHCI_DISK_H

#include "modules/drivers/common/scsi/ScsiDisk.h"

class AhciController;

class EXPORTED_PUBLIC AhciDisk final : public ScsiDisk {
 public:
  AhciDisk(AhciController* controller, size_t port);
  ~AhciDisk() override;

  bool initialise();

  void getName(String& name) override;
  size_t getSize() const override;
  size_t getBlockCount() const override;
  size_t getBlockSize() const override;
  size_t getNativeBlockSize() const override;

  uint64_t doRead(uint64_t location) override;
  uint64_t doWrite(uint64_t location) override;
  uint64_t doWriteDirect(uint64_t location, uintptr_t page) override;
  uint64_t doSync(uint64_t location) override;

  AhciController* controller() const {
    return m_Controller;
  }
  size_t port() const {
    return m_Port;
  }

 private:
  size_t validPageLength(uint64_t location) const;

  AhciController* m_Controller;
  const size_t m_Port;
  size_t m_Sectors;
  size_t m_Bytes;
  bool m_ExtendedFlush;
  bool m_Initialised;
  char m_Model[41];
};

#endif

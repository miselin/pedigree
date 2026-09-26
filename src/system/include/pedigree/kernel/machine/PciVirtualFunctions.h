/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_VIRTUAL_FUNCTIONS_H
#define PEDIGREE_PCI_VIRTUAL_FUNCTIONS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/PciSriov.h"

class Device;

/** Owns an explicitly enabled, isolated group of VFs for one physical function.
 * The PF and this group must outlive all VF users. The caller must quiesce the PF
 * and clear its bus-master bit before enable(). VF controllers are children of
 * the returned off-tree nodes and must be removed before disable(). The caller
 * serializes all VF users with enable/disable.
 *
 * VF Address entries named barN describe CPU-translated slices of PF VF BARs.
 * The actual VF configuration BARs remain zero; drivers must use these assigned
 * ranges without probing or rewriting VF BARs. Isolation covers requester DMA,
 * not peer-to-peer routing or ACS.
 */
class EXPORTED_PUBLIC PciVirtualFunctions {
 public:
  explicit PciVirtualFunctions(Device* pf);
  ~PciVirtualFunctions();

  bool enable(size_t count);
  Device* function(size_t index) const;
  size_t count() const;
  bool disable();
  static bool bar(Device* vf, uint8_t index, uint64_t& pciBase, uint64_t& bytes);

 private:
  PciVirtualFunctions(const PciVirtualFunctions&) = delete;
  PciVirtualFunctions& operator=(const PciVirtualFunctions&) = delete;

  void reclaim();

  // The current VT-d backend has eight requester-domain slots. Other devices
  // may occupy some of them; attach failure rolls the whole group back.
  static constexpr size_t MaxFunctions = 8;
  Device* m_Pf;
  Device* m_Functions[MaxFunctions] = {};
  bool m_Present[MaxFunctions] = {};
  bool m_Reserved[6] = {};
  size_t m_PlannedCount = 0;
  size_t m_Count = 0;
  uint16_t m_Offset = 0;
  uint16_t m_NextCapability = 0;
  PciSriov::State m_Original;
  PciSriov::Geometry m_Bars;
  bool m_Touched = false;
  bool m_EnableAttempted = false;
};

#endif

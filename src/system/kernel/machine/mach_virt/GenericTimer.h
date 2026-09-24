#ifndef PEDIGREE_MACH_VIRT_GENERIC_TIMER_H
#define PEDIGREE_MACH_VIRT_GENERIC_TIMER_H

#include <config.h>
#include <stdint.h>

namespace VirtGenericTimer {
inline uint64_t count() {
#if ARMV7
  uint32_t low, high;
  asm volatile("mrrc p15, 1, %0, %1, c14" : "=r"(low), "=r"(high));
  return (uint64_t(high) << 32) | low;
#else
  uint64_t value;
  asm volatile("mrs %0, cntvct_el0" : "=r"(value));
  return value;
#endif
}

inline uint32_t frequency() {
#if ARMV7
  uint32_t value;
  asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(value));
  return value;
#else
  uint64_t value;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
  return static_cast<uint32_t>(value);
#endif
}

inline void virtualControl(uint32_t value) {
#if ARMV7
  asm volatile("mcr p15, 0, %0, c14, c3, 1\n\tisb" : : "r"(value) : "memory");
#else
  asm volatile("msr cntv_ctl_el0, %0\n\tisb" : : "r"(uint64_t(value)) : "memory");
#endif
}

inline void physicalControl(uint32_t value) {
#if ARMV7
  asm volatile("mcr p15, 0, %0, c14, c2, 1\n\tisb" : : "r"(value) : "memory");
#else
  asm volatile("msr cntp_ctl_el0, %0\n\tisb" : : "r"(uint64_t(value)) : "memory");
#endif
}

inline void setVirtualTimer(uint32_t ticks) {
#if ARMV7
  const uint32_t enabled = 1;
  asm volatile(
      "mcr p15, 0, %0, c14, c3, 0\n\t"
      "mcr p15, 0, %1, c14, c3, 1\n\tisb"
      :
      : "r"(ticks), "r"(enabled)
      : "memory");
#else
  asm volatile("msr cntv_tval_el0, %0\n\tmsr cntv_ctl_el0, %1\n\tisb"
               :
               : "r"(uint64_t(ticks)), "r"(uint64_t(1))
               : "memory");
#endif
}

inline void setPhysicalTimer(uint32_t ticks) {
#if ARMV7
  const uint32_t enabled = 1;
  asm volatile(
      "mcr p15, 0, %0, c14, c2, 0\n\t"
      "mcr p15, 0, %1, c14, c2, 1\n\tisb"
      :
      : "r"(ticks), "r"(enabled)
      : "memory");
#else
  asm volatile("msr cntp_tval_el0, %0\n\tmsr cntp_ctl_el0, %1\n\tisb"
               :
               : "r"(uint64_t(ticks)), "r"(uint64_t(1))
               : "memory");
#endif
}
}  // namespace VirtGenericTimer

#endif

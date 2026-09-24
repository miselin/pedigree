#ifndef KERNEL_PROCESSOR_ARMV7_PROCESSOR_H
#define KERNEL_PROCESSOR_ARMV7_PROCESSOR_H

#include "pedigree/kernel/processor/Processor.h"

class Armv7Processor : public ProcessorBase {};

#if ARMV7
ALWAYS_INLINE inline ProcessorInformation& ProcessorBase::information() {
  ProcessorInformation* info;
  asm volatile("mrc p15, 0, %0, c13, c0, 4" : "=r"(info));
  return info ? *info : m_SafeBspProcessorInformation;
}

ALWAYS_INLINE inline size_t ProcessorBase::index() {
  return information().processorId();
}
#endif

#endif

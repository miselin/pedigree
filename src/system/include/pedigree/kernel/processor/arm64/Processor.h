#ifndef KERNEL_PROCESSOR_ARM64_PROCESSOR_H
#define KERNEL_PROCESSOR_ARM64_PROCESSOR_H

#include "pedigree/kernel/processor/Processor.h"

class Arm64Processor : public ProcessorBase {};

#if ARM64
ALWAYS_INLINE inline ProcessorInformation& ProcessorBase::information() {
  ProcessorInformation* info;
  asm volatile("mrs %0, tpidr_el1" : "=r"(info));
  return info ? *info : m_SafeBspProcessorInformation;
}

ALWAYS_INLINE inline size_t ProcessorBase::index() {
  return information().processorId();
}
#endif

#endif

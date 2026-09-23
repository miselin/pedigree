#ifndef KERNEL_PROCESSOR_ARM64_TYPES_H
#define KERNEL_PROCESSOR_ARM64_TYPES_H

#include <config.h>

typedef signed char Arm64int8_t;
typedef unsigned char Arm64uint8_t;
typedef signed short Arm64int16_t;
typedef unsigned short Arm64uint16_t;
typedef signed int Arm64int32_t;
typedef unsigned int Arm64uint32_t;
typedef signed long Arm64int64_t;
typedef unsigned long Arm64uint64_t;
typedef Arm64int64_t Arm64intptr_t;
typedef Arm64uint64_t Arm64uintptr_t;
typedef Arm64uint64_t Arm64physical_uintptr_t;
typedef Arm64uint64_t Arm64processor_register_t;
typedef Arm64int64_t Arm64ssize_t;
typedef Arm64uint64_t Arm64size_t;
typedef Arm64uint64_t Arm64io_port_t;

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#elif PAGE_SIZE != 4096
#error PAGE_SIZE disagrees with the ARM64 MMU geometry
#endif

#endif

#ifndef KERNEL_PROCESSOR_ARMV7_TYPES_H
#define KERNEL_PROCESSOR_ARMV7_TYPES_H

#include <config.h>

typedef signed char Armv7int8_t;
typedef unsigned char Armv7uint8_t;
typedef signed short Armv7int16_t;
typedef unsigned short Armv7uint16_t;
typedef signed int Armv7int32_t;
typedef unsigned int Armv7uint32_t;
typedef signed long long Armv7int64_t;
typedef unsigned long long Armv7uint64_t;
typedef Armv7int32_t Armv7intptr_t;
typedef Armv7uint32_t Armv7uintptr_t;
typedef Armv7uint32_t Armv7physical_uintptr_t;
typedef Armv7uint32_t Armv7processor_register_t;
typedef Armv7int32_t Armv7ssize_t;
typedef Armv7uint32_t Armv7size_t;
typedef Armv7uint32_t Armv7io_port_t;

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#elif PAGE_SIZE != 4096
#error PAGE_SIZE disagrees with the ARMv7 MMU geometry
#endif

#endif

include_guard(GLOBAL)

# This file describes what we are building, independently of where the
# compiler itself runs. Cross and Pedigree-native builds must keep these
# settings aligned so that they produce the same kernel and userspace ABI.
set(PEDIGREE_COMPILER_TARGET aarch64-linux-musl)
set(PEDIGREE_ARCH_TARGET ARM64)
set(PEDIGREE_TARGET_PAGE_SIZE 4096 CACHE STRING
    "Pedigree base virtual-memory page size in bytes.")
set(PEDIGREE_LINKERSCRIPT
    "${CMAKE_SOURCE_DIR}/src/system/kernel/core/processor/arm64/kernel.ld")
set(PEDIGREE_ARCHDIR "arm64")
set(PEDIGREE_MACHDIR "mach_virt")
set(PEDIGREE_LINKERDIR "arm64")
set(PEDIGREE_DRIVERDIR "arm64")
set(PEDIGREE_ASMDIR "arm64")
set(PEDIGREE_MUSLARCH "arm64")
set(PEDIGREE_MUSL_ARCH_TARGET ${PEDIGREE_ARCH_TARGET})

if (NOT DEFINED PEDIGREE_BUILD_USER_DIR)
    set(PEDIGREE_BUILD_USER_DIR FALSE)
endif ()

set(PEDIGREE_MACHINE_HASPS2 FALSE)
set(PEDIGREE_MACHINE_HASPCI TRUE)

if (NOT CMAKE_SCRIPT_MODE_FILE)
    add_definitions(
        -DARM64=1 -DMACH_VIRT=1 -DBITS_64=1 -DBITS_32=0
        -DTHREADS=1 -DKERNEL_STANDALONE=1
        -DTARGET_IS_LITTLE_ENDIAN=1
        -DKERNEL_PROCESSOR_NO_PORT_IO=1)
endif ()

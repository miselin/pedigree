set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 1)

set(PEDIGREE_C_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-gcc)
set(PEDIGREE_CXX_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(TARGET_SUPPORTS_SHARED_LIBS TRUE)

set(PEDIGREE_COMPILER_TARGET x86_64-pedigree)
set(PEDIGREE_ARCH_TARGET HOSTED)
set(PEDIGREE_LINKERSCRIPT "${CMAKE_SOURCE_DIR}/src/system/kernel/core/processor/hosted/kernel.ld")
set(PEDIGREE_ARCHDIR "hosted")
set(PEDIGREE_MACHDIR "hosted")
set(PEDIGREE_LINKERDIR "hosted")
set(PEDIGREE_DRIVERDIR "hosted")
# TODO: normalize on this so the number of definitions above can be reduced
set(PEDIGREE_ASMDIR "amd64")
set(PEDIGREE_MUSLARCH "amd64")
set(PEDIGREE_MUSL_ARCH_TARGET X64)
set(PEDIGREE_HOSTED TRUE)
set(PEDIGREE_STATIC_DRIVERS TRUE)

# no need for ISOs/GRUB, we run the kernel as a userspace process
set(PEDIGREE_TARGET_LIVECD FALSE)
set(GRUB FALSE)

# Build src/user/... ?
set(PEDIGREE_BUILD_USER_DIR FALSE)

# Machine-specific info
set(PEDIGREE_MACHINE_HASPS2 FALSE)
set(PEDIGREE_MACHINE_HASPCI FALSE)

# Definitions for amd64 across the entire source tree.
add_definitions(-DMACH_HOSTED=1 -DBITS_64=1 -DBITS_32=0
    -DTHREADS=1 -DKERNEL_STANDALONE=1 -DMULTIBOOT=1 -DTARGET_IS_LITTLE_ENDIAN=1
    -DKERNEL_NEEDS_ADDRESS_SPACE_SWITCH=0 -DKERNEL_PROCESSOR_NO_PORT_IO=1
    -DTARGET_HAS_NO_ATOMICS=0 -DSYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH=1)

# Hack around the shared library bits on the Linux platform.
set(__LINUX_COMPILER_GNU 1)  # don't add -rdynamic

macro(__linux_compiler_gnu lang)
    set(CMAKE_${lang}_COMPILER_PREDEFINES_COMMAND "${CMAKE_${lang}_COMPILER}" "-dM" "-E" "-c" "${CMAKE_ROOT}/Modules/CMakeCXXCompilerABI.cpp")
endmacro()

# -lrt
set(LIBRT "rt")


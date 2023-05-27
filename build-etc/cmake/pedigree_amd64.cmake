set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 1)

set(PEDIGREE_C_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-gcc)
set(PEDIGREE_CXX_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-g++)

# CMAKE_TRY_COMPILE_TARGET_TYPE is new in 3.6 and newer, so we need this little
# song and dance to make sure older versions of cmake still work with this
# toolchain file.
if (${CMAKE_VERSION} VERSION_LESS 3.6.0)
    include(CMakeForceCompiler)
    cmake_force_c_compiler(${PEDIGREE_C_COMPILER} GNU)
    cmake_force_cxx_compiler(${PEDIGREE_C_COMPILER} GNU)

    set(CMAKE_OBJCOPY ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-objcopy)
    set(CMAKE_STRIP ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-strip)
else ()
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_C_COMPILER ${PEDIGREE_C_COMPILER})
    set(CMAKE_CXX_COMPILER ${PEDIGREE_C_COMPILER})
endif ()

set(CMAKE_SYSROOT "${CMAKE_SOURCE_DIR}/compilers/dir/x86_64-pedigree")

set(CMAKE_FIND_ROOT_PATH ${CMAKE_SOURCE_DIR}/compilers/dir ${CMAKE_SOURCE_DIR}/compilers/dir/bin ${CMAKE_SOURCE_DIR}/compilers/dir/x86_64-pedigree)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(TARGET_SUPPORTS_SHARED_LIBS TRUE)

set(PEDIGREE_COMPILER_TARGET x86_64-pedigree)
set(PEDIGREE_ARCH_TARGET X64)
set(PEDIGREE_LINKERSCRIPT "${CMAKE_SOURCE_DIR}/src/system/kernel/core/processor/x64/kernel.ld")
set(PEDIGREE_ARCHDIR "x64")
set(PEDIGREE_MACHDIR "mach_pc")
set(PEDIGREE_LINKERDIR "amd64")
set(PEDIGREE_DRIVERDIR "x86")
# TODO: normalize on this so the number of definitions above can be reduced
set(PEDIGREE_ASMDIR "amd64")
set(PEDIGREE_MUSLARCH "amd64")
set(PEDIGREE_MUSL_ARCH_TARGET ${PEDIGREE_ARCH_TARGET})
set(PEDIGREE_TARGET_LIVECD TRUE)
set(GRUB TRUE)

# Build src/user/... ?
set(PEDIGREE_BUILD_USER_DIR TRUE)

# Machine-specific info
set(PEDIGREE_MACHINE_HASPS2 TRUE)
set(PEDIGREE_MACHINE_HASPCI TRUE)

# Definitions for amd64 across the entire source tree.
add_definitions(-DX86_COMMON=1 -DX64=1 -DMACH_PC=1 -DBITS_64=1 -DBITS_32=0
    -DTHREADS=1 -DKERNEL_STANDALONE=1 -DMULTIBOOT=1 -DTARGET_IS_LITTLE_ENDIAN=1
    -DKERNEL_NEEDS_ADDRESS_SPACE_SWITCH=0 -DKERNEL_PROCESSOR_NO_PORT_IO=0
    -DTARGET_HAS_NO_ATOMICS=0)

# Hack around the shared library bits on the Linux platform.
set(__LINUX_COMPILER_GNU 1)  # don't add -rdynamic

macro(__linux_compiler_gnu lang)
    set(CMAKE_${lang}_COMPILER_PREDEFINES_COMMAND "${CMAKE_${lang}_COMPILER}" "-dM" "-E" "-c" "${CMAKE_ROOT}/Modules/CMakeCXXCompilerABI.cpp")
endmacro()

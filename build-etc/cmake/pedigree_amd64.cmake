set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 1)

set(CMAKE_C_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-gcc)
set(CMAKE_CXX_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-g++)

set(CMAKE_FIND_ROOT_PATH ${CMAKE_SOURCE_DIR}/compilers/dir ${CMAKE_SOURCE_DIR}/compilers/dir/x86_64-pedigree)

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

# Definitions for amd64 across the entire source tree.
add_definitions(-DX86_COMMON -DX64 -DMACH_PC -DBITS_64 -DTHREADS
    -DKERNEL_STANDALONE -DMULTIBOOT -DTARGET_IS_LITTLE_ENDIAN)

# Hack around the shared library bits on the Linux platform.
set(__LINUX_COMPILER_GNU 1)  # don't add -rdynamic

macro(__linux_compiler_gnu lang)
    set(CMAKE_${lang}_COMPILER_PREDEFINES_COMMAND "${CMAKE_${lang}_COMPILER}" "-dM" "-E" "-c" "${CMAKE_ROOT}/Modules/CMakeCXXCompilerABI.cpp")
endmacro()

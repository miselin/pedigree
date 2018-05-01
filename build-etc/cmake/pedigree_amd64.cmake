set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)

set(CMAKE_C_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-gcc)
set(CMAKE_CXX_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/x86_64-pedigree-g++)

set(CMAKE_FIND_ROOT_PATH ${CMAKE_SOURCE_DIR}/compilers/dir ${CMAKE_SOURCE_DIR}/compilers/dir/x86_64-pedigree)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(PEDIGREE_LINKERSCRIPT "${CMAKE_SOURCE_DIR}/src/system/kernel/core/processor/x64/kernel.ld")
set(PEDIGREE_ARCHDIR "x64")
set(PEDIGREE_MACHDIR "mach_pc")

# Definitions for amd64 across the entire source tree.
add_definitions(-DX86_COMMON -DX64 -DMACH_PC -DBITS_64 -DTHREADS -DKERNEL_STANDALONE -DMULTIBOOT -DTARGET_IS_LITTLE_ENDIAN)

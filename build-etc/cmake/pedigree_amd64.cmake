set(CMAKE_SYSTEM_NAME Pedigree)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(
    PEDIGREE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
set(PEDIGREE_TOOLCHAIN_ROOT
    "${PEDIGREE_SOURCE_ROOT}/compilers/dir" CACHE PATH
    "Pedigree cross-toolchain installation root")
if (NOT DEFINED PEDIGREE_TARGET_SYSROOT)
    set(PEDIGREE_TARGET_SYSROOT
        "${PEDIGREE_TOOLCHAIN_ROOT}/x86_64-pedigree")
endif ()
set(PEDIGREE_TOOLCHAIN_TRIPLE x86_64-pedigree)
include("${CMAKE_CURRENT_LIST_DIR}/PedigreeCrossToolchain.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/pedigree_amd64_target.cmake")

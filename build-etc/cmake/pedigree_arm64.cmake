set(CMAKE_SYSTEM_NAME Pedigree)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR arm64)

get_filename_component(
    PEDIGREE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
set(PEDIGREE_TOOLCHAIN_ROOT
    "${PEDIGREE_SOURCE_ROOT}/pedigree-compiler-15.3.0-r2" CACHE PATH
    "Pedigree cross-toolchain installation root")
if (NOT DEFINED PEDIGREE_TARGET_SYSROOT)
    set(PEDIGREE_TARGET_SYSROOT
        "${PEDIGREE_SOURCE_ROOT}/scripts/alpine/build/aarch64/sysroot")
endif ()
set(PEDIGREE_TOOLCHAIN_TRIPLE aarch64-linux-musl)
include("${CMAKE_CURRENT_LIST_DIR}/PedigreeCrossToolchain.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/pedigree_arm64_target.cmake")

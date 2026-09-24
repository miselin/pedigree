set(CMAKE_SYSTEM_NAME Pedigree)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR armv7)

get_filename_component(
    PEDIGREE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
if (NOT DEFINED PEDIGREE_TOOLCHAIN_ROOT AND
    NOT "$ENV{PEDIGREE_TOOLCHAIN_ROOT}" STREQUAL "")
    set(PEDIGREE_TOOLCHAIN_ROOT "$ENV{PEDIGREE_TOOLCHAIN_ROOT}" CACHE PATH
        "Pedigree cross-toolchain installation root")
endif ()
set(PEDIGREE_TOOLCHAIN_ROOT
    "${PEDIGREE_SOURCE_ROOT}/pedigree-compiler-15.3.0-r2" CACHE PATH
    "Pedigree cross-toolchain installation root")
if (NOT DEFINED PEDIGREE_TARGET_SYSROOT AND
    NOT "$ENV{PEDIGREE_TARGET_SYSROOT}" STREQUAL "")
    set(PEDIGREE_TARGET_SYSROOT "$ENV{PEDIGREE_TARGET_SYSROOT}")
endif ()
if (NOT DEFINED PEDIGREE_TARGET_SYSROOT)
    set(PEDIGREE_TARGET_SYSROOT
        "${PEDIGREE_SOURCE_ROOT}/scripts/alpine/build/armv7/sysroot")
endif ()
set(PEDIGREE_TOOLCHAIN_TRIPLE armv7-alpine-linux-musleabihf)
include("${CMAKE_CURRENT_LIST_DIR}/PedigreeCrossToolchain.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/pedigree_armv7_target.cmake")

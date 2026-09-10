cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/PedigreeMuslSdkManifest.cmake")
pedigree_validate_musl_sdk_root(
    ROOT "${SDK_ROOT}" ARCHITECTURE x86_64 LAYOUT fhs-usr-v1)

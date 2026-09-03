cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/PedigreeMuslSdkManifest.cmake")

if (NOT DEFINED MANIFEST OR "${MANIFEST}" STREQUAL "")
    message(FATAL_ERROR "MANIFEST is required")
endif ()
if (NOT DEFINED SDK_ROOT OR "${SDK_ROOT}" STREQUAL "")
    message(FATAL_ERROR "SDK_ROOT is required")
endif ()

pedigree_validate_musl_sdk_manifest(
    MANIFEST "${MANIFEST}"
    ROOT "${SDK_ROOT}"
    EXPECTED_BUILD_ID "${EXPECTED_BUILD_ID}")

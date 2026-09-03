cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/PedigreeMuslSdkManifest.cmake")

foreach(required
    SDK_ROOT
    OUTPUT
    MUSL_VERSION
    PORT_REVISION
    ARCHITECTURE
    ABI
    LAYOUT
    PROFILE
    PAGE_SIZE
    DT_RELR
    UPSTREAM_SHA256
    PEDIGREE_REVISION
    BUILD_ID
    COMPILER_TARGET
    COMPILER_VERSION)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif ()
endforeach()

if (NOT IS_ABSOLUTE "${OUTPUT}")
    message(FATAL_ERROR "OUTPUT must be an absolute path: ${OUTPUT}")
endif ()
_pedigree_musl_sdk_require_token("MUSL_VERSION" "${MUSL_VERSION}")
if (NOT "${PORT_REVISION}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "PORT_REVISION must be a positive integer: ${PORT_REVISION}")
endif ()
_pedigree_musl_sdk_require_token("ABI" "${ABI}")
if (NOT "${PROFILE}" STREQUAL "target" AND
    NOT "${PROFILE}" STREQUAL "hosted")
    message(FATAL_ERROR "PROFILE must be target or hosted: ${PROFILE}")
endif ()
if (NOT "${PAGE_SIZE}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "PAGE_SIZE must be a positive integer: ${PAGE_SIZE}")
endif ()
math(EXPR page_size_mask "${PAGE_SIZE} - 1")
math(EXPR page_size_remainder "${PAGE_SIZE} & ${page_size_mask}")
if (NOT page_size_remainder EQUAL 0)
    message(FATAL_ERROR "PAGE_SIZE must be a power of two: ${PAGE_SIZE}")
endif ()
string(TOUPPER "${DT_RELR}" dt_relr_input)
if (NOT dt_relr_input MATCHES "^(0|1|ON|OFF|TRUE|FALSE|YES|NO)$")
    message(FATAL_ERROR "DT_RELR must be a boolean value: ${DT_RELR}")
endif ()
if (dt_relr_input)
    set(dt_relr_json true)
else ()
    set(dt_relr_json false)
endif ()
string(TOLOWER "${UPSTREAM_SHA256}" UPSTREAM_SHA256)
string(LENGTH "${UPSTREAM_SHA256}" upstream_sha256_length)
if (NOT upstream_sha256_length EQUAL 64 OR
    NOT "${UPSTREAM_SHA256}" MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "UPSTREAM_SHA256 must be 64 hexadecimal characters")
endif ()
_pedigree_musl_sdk_require_token(
    "PEDIGREE_REVISION" "${PEDIGREE_REVISION}")
_pedigree_musl_sdk_require_token("BUILD_ID" "${BUILD_ID}")
_pedigree_musl_sdk_require_token("COMPILER_TARGET" "${COMPILER_TARGET}")
_pedigree_musl_sdk_require_token("COMPILER_VERSION" "${COMPILER_VERSION}")

pedigree_validate_musl_sdk_root(
    ROOT "${SDK_ROOT}"
    ARCHITECTURE "${ARCHITECTURE}"
    LAYOUT "${LAYOUT}"
    LOADER_TARGET_OUTPUT loader_target)
_pedigree_musl_sdk_paths("${ARCHITECTURE}" "${LAYOUT}")

set(manifest [=[{
  "schema": "@PEDIGREE_MUSL_SDK_MANIFEST_SCHEMA@",
  "schema_version": @PEDIGREE_MUSL_SDK_MANIFEST_SCHEMA_VERSION@,
  "component": {
    "name": "musl",
    "version": "@MUSL_VERSION@",
    "port_revision": @PORT_REVISION@
  },
  "target": {
    "architecture": "@ARCHITECTURE@",
    "abi": "@ABI@",
    "profile": "@PROFILE@",
    "page_size": @PAGE_SIZE@,
    "dt_relr": @dt_relr_json@
  },
  "layout": {
    "name": "@LAYOUT@",
    "include_directory": "@PEDIGREE_MUSL_SDK_INCLUDE_DIR@",
    "library_directory": "@PEDIGREE_MUSL_SDK_LIBRARY_DIR@",
    "dynamic_loader": "@PEDIGREE_MUSL_SDK_LOADER@",
    "dynamic_loader_target": "@loader_target@"
  },
  "provenance": {
    "upstream_sha256": "@UPSTREAM_SHA256@",
    "pedigree_revision": "@PEDIGREE_REVISION@",
    "build_id": "@BUILD_ID@",
    "compiler_target": "@COMPILER_TARGET@",
    "compiler_version": "@COMPILER_VERSION@"
  }
}
]=])
string(CONFIGURE "${manifest}" manifest @ONLY)

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
set(temporary "${OUTPUT}.tmp")
file(WRITE "${temporary}" "${manifest}")
file(RENAME "${temporary}" "${OUTPUT}")

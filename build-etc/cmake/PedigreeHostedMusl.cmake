set(MUSL_VERSION "1.2.6")
set(MUSL_NAME "musl-${MUSL_VERSION}")
set(MUSL_FILENAME "${MUSL_NAME}.tar.gz")
set(MUSL_SHA256 "d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a")
set(PEDIGREE_MUSL_PORT_REVISION 1)

# Hosted libc redirects syscalls through the in-process kernel bridge.
# The target musl PUP cannot replace this variant.
set(PEDIGREE_MUSL_ARCHIVE
    "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_FILENAME}" CACHE FILEPATH
    "Local musl source archive used to build Pedigree's libc.")
get_filename_component(MUSL_ARCHIVE "${PEDIGREE_MUSL_ARCHIVE}" ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}")
set(PEDIGREE_MUSL_CONFIG_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/musl-config")
file(MAKE_DIRECTORY "${PEDIGREE_MUSL_CONFIG_DIR}")
if (PEDIGREE_MUSL_ARCH_TARGET STREQUAL "HOSTED")
    set(PEDIGREE_MUSL_CONFIG_HOSTED 1)
    set(PEDIGREE_MUSL_CONFIG_X64 0)
else ()
    set(PEDIGREE_MUSL_CONFIG_HOSTED 0)
    set(PEDIGREE_MUSL_CONFIG_X64 1)
endif ()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/config.h.in
    ${PEDIGREE_MUSL_CONFIG_DIR}/config.h)
set(PEDIGREE_MUSL_PORT_INPUTS
    ${CMAKE_SOURCE_DIR}/build-etc/cmake/GenerateMuslSdkManifest.cmake
    ${CMAKE_SOURCE_DIR}/build-etc/cmake/PedigreeMuslSdkManifest.cmake
    ${CMAKE_SOURCE_DIR}/build-etc/cmake/ValidateMuslSdkManifest.cmake
    ${CMAKE_SOURCE_DIR}/build-etc/toolchain/musl-sdk-manifest-v1.schema.json
    ${CMAKE_SOURCE_DIR}/build-etc/toolchain/musl-1.2.6-cve-2026-40200-qsort.patch
    ${CMAKE_SOURCE_DIR}/build-etc/toolchain/musl-1.2.6-cve-2026-6042-iconv.patch
    ${CMAKE_SOURCE_DIR}/scripts/build-musl-${PEDIGREE_MUSLARCH}.sh)
if (PEDIGREE_MUSL_ARCH_TARGET STREQUAL "HOSTED")
    list(APPEND PEDIGREE_MUSL_PORT_INPUTS
        ${PEDIGREE_MUSL_CONFIG_DIR}/config.h
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/clone-hosted-amd64.musl-s
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/glue-musl.c
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/syscall_arch.h
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/syscall_cp-${PEDIGREE_MUSLARCH}.musl-s
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/syscalls/posix-syscall.h
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/syscalls/posixSyscallNumbers.h
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/processor/Syscalls.h
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/processor/syscall-stubs.h
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/processor/hosted/syscall-stubs.h)
endif ()

set(_PEDIGREE_MUSL_PORT_HASH_INPUT "")
foreach (_PEDIGREE_MUSL_PORT_INPUT IN LISTS PEDIGREE_MUSL_PORT_INPUTS)
    file(SHA256 "${_PEDIGREE_MUSL_PORT_INPUT}"
        _PEDIGREE_MUSL_PORT_INPUT_HASH)
    string(APPEND _PEDIGREE_MUSL_PORT_HASH_INPUT
        "${_PEDIGREE_MUSL_PORT_INPUT_HASH}\n")
endforeach ()
string(SHA256 PEDIGREE_MUSL_REVISION
    "${_PEDIGREE_MUSL_PORT_HASH_INPUT}")

execute_process(
    COMMAND "${PEDIGREE_C_COMPILER}" -dumpmachine
    RESULT_VARIABLE _PEDIGREE_MUSL_COMPILER_TARGET_RESULT
    OUTPUT_VARIABLE PEDIGREE_MUSL_COMPILER_TARGET
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND "${PEDIGREE_C_COMPILER}" -dumpfullversion -dumpversion
    RESULT_VARIABLE _PEDIGREE_MUSL_COMPILER_VERSION_RESULT
    OUTPUT_VARIABLE PEDIGREE_MUSL_COMPILER_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if (NOT _PEDIGREE_MUSL_COMPILER_TARGET_RESULT EQUAL 0 OR
    NOT _PEDIGREE_MUSL_COMPILER_VERSION_RESULT EQUAL 0 OR
    NOT PEDIGREE_MUSL_COMPILER_TARGET OR NOT PEDIGREE_MUSL_COMPILER_VERSION)
    message(FATAL_ERROR
        "Could not identify the compiler used to build the musl SDK: "
        "${PEDIGREE_C_COMPILER}")
endif ()

if (PEDIGREE_MUSL_ARCH_TARGET STREQUAL "HOSTED")
    set(PEDIGREE_MUSL_PROFILE hosted)
else ()
    set(PEDIGREE_MUSL_PROFILE target)
endif ()
set(PEDIGREE_MUSL_ABI pedigree-linux-amd64-v1)
string(CONCAT _PEDIGREE_MUSL_BUILD_ID_INPUT
    "${MUSL_VERSION}\n${MUSL_SHA256}\n${PEDIGREE_MUSL_PORT_REVISION}\n"
    "${PEDIGREE_MUSL_REVISION}\n${PEDIGREE_MUSL_ARCH_TARGET}\n"
    "${PEDIGREE_MUSL_ABI}\n${PEDIGREE_TARGET_PAGE_SIZE}\n"
    "${PEDIGREE_DTRELR_ENABLED}\n${PEDIGREE_MUSL_COMPILER_TARGET}\n"
    "${PEDIGREE_MUSL_COMPILER_VERSION}\n")
string(SHA256 PEDIGREE_MUSL_BUILD_ID
    "${_PEDIGREE_MUSL_BUILD_ID_INPUT}")
set(PEDIGREE_MUSL_MANIFEST
    ${PEDIGREE_MUSL_SDK_ROOT}/usr/share/pedigree/libc/manifest.json)

set(MUSL_DOWNLOAD "${MUSL_ARCHIVE}.download")
set(MUSL_DTRELR_STATE "${CMAKE_CURRENT_BINARY_DIR}/musl-dtrelr.state")
set(MUSL_PAGE_SIZE_STATE "${CMAKE_CURRENT_BINARY_DIR}/musl-page-size.state")
set(MUSL_PORT_STATE "${CMAKE_CURRENT_BINARY_DIR}/musl-port.state")
set(MUSL_DERIVATION_STATE
    "${CMAKE_CURRENT_BINARY_DIR}/musl-derivation.state")
file(GENERATE OUTPUT "${MUSL_DTRELR_STATE}"
    CONTENT "PEDIGREE_DTRELR=$<BOOL:${PEDIGREE_DTRELR_ENABLED}>\n")
file(GENERATE OUTPUT "${MUSL_PAGE_SIZE_STATE}"
    CONTENT "PEDIGREE_TARGET_PAGE_SIZE=${PEDIGREE_TARGET_PAGE_SIZE}\n")
file(GENERATE OUTPUT "${MUSL_PORT_STATE}"
    CONTENT "PEDIGREE_MUSL_PORT_REVISION=${PEDIGREE_MUSL_PORT_REVISION}\n")
file(GENERATE OUTPUT "${MUSL_DERIVATION_STATE}"
    CONTENT "PEDIGREE_MUSL_BUILD_ID=${PEDIGREE_MUSL_BUILD_ID}\n")

if (EXISTS "${MUSL_ARCHIVE}")
    file(SHA256 "${MUSL_ARCHIVE}" MUSL_ACTUAL_SHA256)
    if (NOT "${MUSL_ACTUAL_SHA256}" STREQUAL "${MUSL_SHA256}")
        message(FATAL_ERROR
            "Cached musl archive has SHA256 ${MUSL_ACTUAL_SHA256}; expected ${MUSL_SHA256}: ${MUSL_ARCHIVE}")
    endif ()
else ()
    if (PEDIGREE_SELF_HOSTED)
        message(FATAL_ERROR
            "Self-hosted builds do not download dependencies during configure. "
            "Provide the verified musl archive with "
            "-DPEDIGREE_MUSL_ARCHIVE=/path/to/${MUSL_FILENAME}")
    endif ()

    file(REMOVE "${MUSL_DOWNLOAD}")
    file(DOWNLOAD
        "https://www.musl-libc.org/releases/${MUSL_FILENAME}"
        "${MUSL_DOWNLOAD}"
        STATUS MUSL_DOWNLOAD_STATUS)
    list(GET MUSL_DOWNLOAD_STATUS 0 MUSL_DOWNLOAD_STATUS_CODE)
    list(GET MUSL_DOWNLOAD_STATUS 1 MUSL_DOWNLOAD_STATUS_MESSAGE)
    if (NOT MUSL_DOWNLOAD_STATUS_CODE EQUAL 0)
        file(REMOVE "${MUSL_DOWNLOAD}")
        message(FATAL_ERROR
            "Failed to download ${MUSL_FILENAME}: ${MUSL_DOWNLOAD_STATUS_MESSAGE}")
    endif ()

    file(SHA256 "${MUSL_DOWNLOAD}" MUSL_ACTUAL_SHA256)
    if (NOT "${MUSL_ACTUAL_SHA256}" STREQUAL "${MUSL_SHA256}")
        file(REMOVE "${MUSL_DOWNLOAD}")
        message(FATAL_ERROR
            "Downloaded musl archive has SHA256 ${MUSL_ACTUAL_SHA256}; expected ${MUSL_SHA256}")
    endif ()

    file(RENAME "${MUSL_DOWNLOAD}" "${MUSL_ARCHIVE}")
endif ()

# The SDK provenance is computed while CMake configures. Make every input that
# contributes to it part of CMake's own regeneration graph so an incremental
# build cannot rebuild libc with stale manifest data.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MUSL_ARCHIVE}"
    "${PEDIGREE_C_COMPILER}"
    ${PEDIGREE_MUSL_PORT_INPUTS})

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}"
    COMMAND ${CMAKE_COMMAND} -E tar xzf "${MUSL_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure"
    DEPENDS
        "${MUSL_ARCHIVE}"
        "${MUSL_PORT_STATE}"
        ${PEDIGREE_MUSL_PORT_INPUTS}
)
add_custom_target(musl_extract ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure")

set(PEDIGREE_MUSL_TOOL_ENV)
if (PEDIGREE_SELF_HOSTED)
    # A compiler hosted on Pedigree is already targeting Pedigree, so native
    # tool names are sufficient even when no target-prefixed aliases exist.
    list(APPEND PEDIGREE_MUSL_TOOL_ENV
        PEDIGREE_USE_PATH_TOOLCHAIN=1
        PEDIGREE_CROSS_COMPILE_PREFIX=
        "PEDIGREE_NATIVE_TOOL_ROOT=${PEDIGREE_NATIVE_TOOL_ROOT}")
    if (CMAKE_OBJDUMP AND NOT CMAKE_OBJDUMP MATCHES "-NOTFOUND$")
        list(APPEND PEDIGREE_MUSL_TOOL_ENV PEDIGREE_OBJDUMP=${CMAKE_OBJDUMP})
    endif ()
    if (CMAKE_READELF AND NOT CMAKE_READELF MATCHES "-NOTFOUND$")
        list(APPEND PEDIGREE_MUSL_TOOL_ENV PEDIGREE_READELF=${CMAKE_READELF})
    endif ()
endif ()

set(_PEDIGREE_MUSL_BUILD_COMMAND
    ${CMAKE_COMMAND} -E env
    CC=${PEDIGREE_C_COMPILER} LD=${CMAKE_C_COMPILER}
    COMPILER_TARGET=${PEDIGREE_COMPILER_TARGET}
    PEDIGREE_DTRELR=$<BOOL:${PEDIGREE_DTRELR_ENABLED}>
    PEDIGREE_TARGET_PAGE_SIZE=${PEDIGREE_TARGET_PAGE_SIZE}
    PEDIGREE_CONFIG_INCLUDE_DIR=${PEDIGREE_MUSL_CONFIG_DIR}
    PEDIGREE_TOOLCHAIN_ROOT=${PEDIGREE_TOOLCHAIN_ROOT}
    CMAKE_COMMAND=${CMAKE_COMMAND}
    MUSL_VERSION=${MUSL_VERSION}
    MUSL_UPSTREAM_SHA256=${MUSL_SHA256}
    PEDIGREE_MUSL_PORT_REVISION=${PEDIGREE_MUSL_PORT_REVISION}
    PEDIGREE_MUSL_REVISION=${PEDIGREE_MUSL_REVISION}
    PEDIGREE_MUSL_BUILD_ID=${PEDIGREE_MUSL_BUILD_ID}
    PEDIGREE_MUSL_ABI=${PEDIGREE_MUSL_ABI}
    PEDIGREE_MUSL_PROFILE=${PEDIGREE_MUSL_PROFILE}
    PEDIGREE_MUSL_COMPILER_TARGET=${PEDIGREE_MUSL_COMPILER_TARGET}
    PEDIGREE_MUSL_COMPILER_VERSION=${PEDIGREE_MUSL_COMPILER_VERSION}
    PEDIGREE_MUSL_MANIFEST_GENERATOR=${CMAKE_SOURCE_DIR}/build-etc/cmake/GenerateMuslSdkManifest.cmake
    PEDIGREE_MUSL_MANIFEST_VALIDATOR=${CMAKE_SOURCE_DIR}/build-etc/cmake/ValidateMuslSdkManifest.cmake
    ${PEDIGREE_MUSL_TOOL_ENV}
    SRCDIR=${CMAKE_SOURCE_DIR}
    TARGETDIR=${PEDIGREE_MUSL_SDK_ROOT}
    ARCH_TARGET=${PEDIGREE_MUSL_ARCH_TARGET}
    ${CMAKE_SOURCE_DIR}/scripts/build-musl-${PEDIGREE_MUSLARCH}.sh)

# Build libc. The target also invokes this command after checking the primary
# output; the script exits immediately for a complete matching SDK and repairs
# missing secondary package files otherwise.
add_custom_command(
    OUTPUT
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libc.so
    ${MUSL_LIBNAME}
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/crt1.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/crti.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/crtn.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/rcrt1.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/Scrt1.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libc.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libcrypt.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libdl.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libm.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/librt.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libpthread.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libresolv.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libutil.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libxnet.a
    ${PEDIGREE_MUSL_MANIFEST}
    COMMAND ${_PEDIGREE_MUSL_BUILD_COMMAND}
    DEPENDS
    ${CMAKE_TOOLCHAIN_FILE}
    ${MUSL_DTRELR_STATE}
    ${MUSL_PAGE_SIZE_STATE}
    ${MUSL_PORT_STATE}
    ${MUSL_DERIVATION_STATE}
    ${PEDIGREE_MUSL_PORT_INPUTS}
    ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}
)

add_custom_target(libc ALL
    COMMAND ${_PEDIGREE_MUSL_BUILD_COMMAND}
    DEPENDS ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libc.so
    COMMENT "Validating the musl SDK"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}
    VERBATIM)
add_dependencies(pedigree_musl_headers libc)

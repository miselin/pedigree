cmake_minimum_required(VERSION 3.20)

foreach (required_variable
        PEDIGREE_EXTERNAL_WINMAN_SOURCE_DIR
        PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR
        PEDIGREE_EXTERNAL_WINMAN_STAGE_DIR
        PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP
        PEDIGREE_EXTERNAL_WINMAN_DISABLED_STAMP
        PEDIGREE_EXTERNAL_WINMAN_EXPECTED_OUTPUTS
        PEDIGREE_ROOT
        PEDIGREE_BUILD_DIR)
    if (NOT DEFINED ${required_variable} OR
        "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

set(expected_outputs ${PEDIGREE_EXTERNAL_WINMAN_EXPECTED_OUTPUTS})
file(MAKE_DIRECTORY "${PEDIGREE_EXTERNAL_WINMAN_STAGE_DIR}")

function(winman_manifest output_variable complete_variable)
    set(manifest
        "source=${PEDIGREE_EXTERNAL_WINMAN_SOURCE_DIR}\nstage=${PEDIGREE_EXTERNAL_WINMAN_STAGE_DIR}\n")
    foreach (input IN LISTS inputs)
        if (NOT EXISTS "${input}")
            set(${output_variable} "" PARENT_SCOPE)
            set(${complete_variable} OFF PARENT_SCOPE)
            return()
        endif ()
        file(SHA256 "${input}" digest)
        string(APPEND manifest "input=${input}:${digest}\n")
    endforeach ()
    foreach (output IN LISTS expected_outputs)
        if (NOT EXISTS "${output}")
            set(${output_variable} "" PARENT_SCOPE)
            set(${complete_variable} OFF PARENT_SCOPE)
            return()
        endif ()
        file(SHA256 "${output}" digest)
        file(RELATIVE_PATH relative_output
            "${PEDIGREE_EXTERNAL_WINMAN_STAGE_DIR}" "${output}")
        string(APPEND manifest "${relative_output}=${digest}\n")
    endforeach ()
    set(${output_variable} "${manifest}" PARENT_SCOPE)
    set(${complete_variable} ON PARENT_SCOPE)
endfunction()

set(inputs
    "${PEDIGREE_EXTERNAL_WINMAN_SOURCE_DIR}/CMakeLists.txt"
    "${PEDIGREE_TARGET_SYSROOT}/usr/lib/libc.so"
    "${PEDIGREE_ROOT}/build-etc/cmake/PedigreeAlpineSdk.cmake"
    "${PEDIGREE_ROOT}/build-etc/cmake/PedigreeMuslLink.cmake")
foreach (source_directory
        assets
        cmake
        libui-buffer
        libui-client
        libui-core
        libui-platform
        libui-protocol
        libui-theme
        libui-transport
        libui-widget
        packaging
        src
        tools)
    file(GLOB_RECURSE directory_inputs LIST_DIRECTORIES FALSE
        "${PEDIGREE_EXTERNAL_WINMAN_SOURCE_DIR}/${source_directory}/*")
    list(APPEND inputs ${directory_inputs})
endforeach ()
list(FILTER inputs EXCLUDE REGEX "/\\.[^/]+(/|$)|/#[^/]*#$")
list(REMOVE_DUPLICATES inputs)
list(SORT inputs)

set(stamp_needs_update OFF)
winman_manifest(current_manifest outputs_complete)
if (NOT outputs_complete OR
    NOT EXISTS "${PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP}")
    set(stamp_needs_update ON)
else ()
    file(READ "${PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP}" recorded_manifest)
    if (NOT "${current_manifest}" STREQUAL "${recorded_manifest}")
        set(stamp_needs_update ON)
    endif ()
endif ()

# Prune the exact files owned by this integration before installing. This keeps
# removed or renamed clients from surviving in the shared image overlay. Force
# the final links as well so runtime, SDK, and raw -l dependencies are resampled
# after the parent build finishes them.
set(nested_executables
    "${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}/winman-external"
    "${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}/winman-external-notepad"
    "${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}/winman-external-terminal"
    "${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}/winman-external-widgets"
    "${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}/winman-external-painter"
    "${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}/winman-external-greeter"
    "${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}/winman-external-shell")
file(REMOVE ${nested_executables})
file(REMOVE ${expected_outputs})
message(STATUS "Refreshing out-of-tree pedigree-winman and graphical clients")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PEDIGREE_ROOT=${PEDIGREE_ROOT}"
        "PEDIGREE_WINMAN_SOURCE_DIR=${PEDIGREE_EXTERNAL_WINMAN_SOURCE_DIR}"
        "LIBUI_PROTOC_EXECUTABLE=${PEDIGREE_ROOT}/scripts/alpine/protoc.sh"
        "PEDIGREE_BUILD_DIR=${PEDIGREE_BUILD_DIR}"
        "PEDIGREE_TARGET_SYSROOT=${PEDIGREE_TARGET_SYSROOT}"
        "PEDIGREE_TOOLCHAIN_ROOT=${PEDIGREE_TOOLCHAIN_ROOT}"
        "PEDIGREE_STAGE_DIR=${PEDIGREE_EXTERNAL_WINMAN_STAGE_DIR}"
        "WINMAN_BUILD_DIR=${PEDIGREE_EXTERNAL_WINMAN_BUILD_DIR}"
        "${PEDIGREE_EXTERNAL_WINMAN_SOURCE_DIR}/tools/build-pedigree.sh"
        --no-image
    WORKING_DIRECTORY "${PEDIGREE_EXTERNAL_WINMAN_SOURCE_DIR}"
    RESULT_VARIABLE build_result
    COMMAND_ECHO STDOUT)
if (NOT build_result EQUAL 0)
    message(FATAL_ERROR "pedigree-winman build failed (${build_result})")
endif ()

winman_manifest(installed_manifest outputs_complete)
if (NOT outputs_complete)
    message(FATAL_ERROR
        "pedigree-winman completed without installing every expected artifact")
endif ()
if (NOT DEFINED recorded_manifest OR
    NOT "${installed_manifest}" STREQUAL "${recorded_manifest}")
    set(stamp_needs_update ON)
endif ()

file(REMOVE "${PEDIGREE_EXTERNAL_WINMAN_DISABLED_STAMP}")
if (stamp_needs_update)
    file(WRITE "${PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP}.tmp"
        "${installed_manifest}")
    file(RENAME
        "${PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP}.tmp"
        "${PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP}")
endif ()

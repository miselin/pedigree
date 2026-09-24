include_guard(GLOBAL)

include(ExternalProject)

function(_pedigree_import_nested_host_tool
         target_name executable_name producer stage_dir)
    if (TARGET ${target_name})
        message(FATAL_ERROR "Host tool target already exists: ${target_name}")
    endif ()

    add_executable(${target_name} IMPORTED GLOBAL)
    set_target_properties(${target_name} PROPERTIES
        IMPORTED_LOCATION
            "${stage_dir}/${executable_name}${_PEDIGREE_HOST_EXECUTABLE_SUFFIX}")
    add_dependencies(${target_name} ${producer})
endfunction()

function(pedigree_add_nested_host_tools)
    set(multi_value_args REQUIRED_TARGETS)
    cmake_parse_arguments(PARSE_ARGV 0 PEDIGREE_HOST_TOOLS
        "" "" "${multi_value_args}")

    if (NOT PEDIGREE_HOST_TOOLS_REQUIRED_TARGETS)
        return()
    endif ()

    get_filename_component(_PEDIGREE_SOURCE_ROOT
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)

    set(PEDIGREE_HOST_TOOLS_STAGE_DIR
        "${CMAKE_BINARY_DIR}/host-tools/bin" CACHE PATH
        "Native executables used while building Pedigree target artifacts.")
    set(PEDIGREE_HOST_TOOLS_BUILD_DIR
        "${CMAKE_BINARY_DIR}/host-tools/build" CACHE PATH
        "Internal native build trees for Pedigree host tools.")

    if (NOT PEDIGREE_BUILD_HOST_C_COMPILER)
        find_program(PEDIGREE_BUILD_HOST_C_COMPILER
            NAMES cc clang gcc
            NO_CACHE
            NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    endif ()

    if (CMAKE_HOST_WIN32)
        set(_PEDIGREE_HOST_EXECUTABLE_SUFFIX ".exe")
    else ()
        set(_PEDIGREE_HOST_EXECUTABLE_SUFFIX "")
    endif ()

    set(_PEDIGREE_HOST_CONFIGURATION_DEFAULT "${CMAKE_BUILD_TYPE}")
    if (NOT _PEDIGREE_HOST_CONFIGURATION_DEFAULT)
        set(_PEDIGREE_HOST_CONFIGURATION_DEFAULT Debug)
    endif ()
    set(PEDIGREE_HOST_TOOLS_CONFIGURATION
        "${_PEDIGREE_HOST_CONFIGURATION_DEFAULT}" CACHE STRING
        "Configuration used for native Pedigree host utilities.")
    set(_PEDIGREE_HOST_CONFIGURATION
        "${PEDIGREE_HOST_TOOLS_CONFIGURATION}")

    set(_PEDIGREE_NEEDS_ARTIFACT_GENERATORS FALSE)
    set(_PEDIGREE_NEEDS_DISTRIBUTION_TOOLS FALSE)
    foreach (_PEDIGREE_HOST_TARGET IN LISTS
             PEDIGREE_HOST_TOOLS_REQUIRED_TARGETS)
        if (_PEDIGREE_HOST_TARGET STREQUAL
                "host-pedigree-initrd-builder")
            set(_PEDIGREE_NEEDS_ARTIFACT_GENERATORS TRUE)
        elseif (_PEDIGREE_HOST_TARGET STREQUAL "host-keymap" OR
                _PEDIGREE_HOST_TARGET STREQUAL "host-headerify" OR
                _PEDIGREE_HOST_TARGET STREQUAL "host-ext2img")
            set(_PEDIGREE_NEEDS_DISTRIBUTION_TOOLS TRUE)
        else ()
            message(FATAL_ERROR
                "Nested host tools do not provide ${_PEDIGREE_HOST_TARGET}.")
        endif ()
    endforeach ()

    if (_PEDIGREE_NEEDS_DISTRIBUTION_TOOLS AND
        NOT PEDIGREE_BUILD_HOST_CXX_COMPILER)
        find_program(PEDIGREE_BUILD_HOST_CXX_COMPILER
            NAMES c++ clang++ g++
            NO_CACHE
            NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    endif ()

    # A CMake cache cannot safely change compilers after language detection,
    # and different configurations can otherwise overwrite one staged output.
    # Keep each native build identity and its outputs isolated.
    string(JOIN "|" _PEDIGREE_HOST_TOOLCHAIN_KEY
        "${PEDIGREE_BUILD_HOST_C_COMPILER}"
        "${PEDIGREE_BUILD_HOST_CXX_COMPILER}"
        "${_PEDIGREE_HOST_CONFIGURATION}"
        "${CMAKE_GENERATOR}"
        "${CMAKE_GENERATOR_PLATFORM}"
        "${CMAKE_GENERATOR_TOOLSET}"
        "${CMAKE_GENERATOR_INSTANCE}")
    string(SHA256 _PEDIGREE_HOST_TOOLCHAIN_ID
        "${_PEDIGREE_HOST_TOOLCHAIN_KEY}")
    string(SUBSTRING "${_PEDIGREE_HOST_TOOLCHAIN_ID}" 0 12
        _PEDIGREE_HOST_TOOLCHAIN_ID)
    set(_PEDIGREE_HOST_TOOLCHAIN_BUILD_DIR
        "${PEDIGREE_HOST_TOOLS_BUILD_DIR}/${_PEDIGREE_HOST_TOOLCHAIN_ID}")
    set(_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR
        "${PEDIGREE_HOST_TOOLS_STAGE_DIR}/${_PEDIGREE_HOST_TOOLCHAIN_ID}")

    set(_PEDIGREE_HOST_RUNTIME_ARGS
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY:PATH=${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}")
    if (CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_PEDIGREE_HOST_CONFIGURATION}"
            _PEDIGREE_HOST_CONFIGURATION_UPPER)
        list(APPEND _PEDIGREE_HOST_RUNTIME_ARGS
            "-DCMAKE_CONFIGURATION_TYPES:STRING=${_PEDIGREE_HOST_CONFIGURATION}"
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_${_PEDIGREE_HOST_CONFIGURATION_UPPER}:PATH=${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}")
    endif ()

    if (_PEDIGREE_NEEDS_ARTIFACT_GENERATORS)
        set(_PEDIGREE_INITRD_BUILDER_PATH
            "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}/pedigree-initrd-builder${_PEDIGREE_HOST_EXECUTABLE_SUFFIX}")

        ExternalProject_Add(pedigree-host-artifact-generators
            PREFIX
                "${_PEDIGREE_HOST_TOOLCHAIN_BUILD_DIR}/artifact-generators-prefix"
            SOURCE_DIR
                "${_PEDIGREE_SOURCE_ROOT}/src/buildutil/artifact-generators"
            BINARY_DIR
                "${_PEDIGREE_HOST_TOOLCHAIN_BUILD_DIR}/artifact-generators"
            DOWNLOAD_COMMAND ""
            UPDATE_COMMAND ""
            PATCH_COMMAND ""
            CMAKE_ARGS
                "-DCMAKE_TOOLCHAIN_FILE:FILEPATH="
                "-DCMAKE_BUILD_TYPE:STRING=${_PEDIGREE_HOST_CONFIGURATION}"
                "-DCMAKE_C_COMPILER:FILEPATH=${PEDIGREE_BUILD_HOST_C_COMPILER}"
                "-DBUILD_TESTING:BOOL=OFF"
                ${_PEDIGREE_HOST_RUNTIME_ARGS}
            BUILD_ALWAYS TRUE
            BUILD_COMMAND
                "${CMAKE_COMMAND}" --build <BINARY_DIR>
                --config "${_PEDIGREE_HOST_CONFIGURATION}"
                --target pedigree-artifact-generators
            BUILD_BYPRODUCTS
                "${_PEDIGREE_INITRD_BUILDER_PATH}"
            INSTALL_COMMAND ""
            TEST_COMMAND ""
            USES_TERMINAL_CONFIGURE TRUE
            USES_TERMINAL_BUILD TRUE)

        if ("host-pedigree-initrd-builder" IN_LIST
            PEDIGREE_HOST_TOOLS_REQUIRED_TARGETS)
            _pedigree_import_nested_host_tool(
                host-pedigree-initrd-builder pedigree-initrd-builder
                pedigree-host-artifact-generators
                "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}")
        endif ()
    endif ()

    if (_PEDIGREE_NEEDS_DISTRIBUTION_TOOLS)
        set(_PEDIGREE_KEYMAP_PATH
            "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}/keymap${_PEDIGREE_HOST_EXECUTABLE_SUFFIX}")
        set(_PEDIGREE_HEADERIFY_PATH
            "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}/headerify${_PEDIGREE_HOST_EXECUTABLE_SUFFIX}")
        set(_PEDIGREE_EXT2IMG_PATH
            "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}/ext2img${_PEDIGREE_HOST_EXECUTABLE_SUFFIX}")

        ExternalProject_Add(pedigree-host-distribution-tools
            PREFIX
                "${_PEDIGREE_HOST_TOOLCHAIN_BUILD_DIR}/distribution-prefix"
            SOURCE_DIR "${_PEDIGREE_SOURCE_ROOT}"
            BINARY_DIR
                "${_PEDIGREE_HOST_TOOLCHAIN_BUILD_DIR}/distribution"
            DOWNLOAD_COMMAND ""
            UPDATE_COMMAND ""
            PATCH_COMMAND ""
            CMAKE_ARGS
                "-DCMAKE_TOOLCHAIN_FILE:FILEPATH="
                "-DCMAKE_BUILD_TYPE:STRING=${_PEDIGREE_HOST_CONFIGURATION}"
                "-DCMAKE_C_COMPILER:FILEPATH=${PEDIGREE_BUILD_HOST_C_COMPILER}"
                "-DCMAKE_CXX_COMPILER:FILEPATH=${PEDIGREE_BUILD_HOST_CXX_COMPILER}"
                "-DPEDIGREE_BUILD_ROLE:STRING=HOST_TOOLS"
                "-DPEDIGREE_BUILDUTILS_ASAN:BOOL=OFF"
                "-DPEDIGREE_REGENERATE_KEYMAP_SOURCES:BOOL=OFF"
                "-DBUILD_TESTING:BOOL=OFF"
                ${_PEDIGREE_HOST_RUNTIME_ARGS}
            BUILD_ALWAYS TRUE
            BUILD_COMMAND
                "${CMAKE_COMMAND}" --build <BINARY_DIR>
                --config "${_PEDIGREE_HOST_CONFIGURATION}"
                --target pedigree-distribution-tools
            BUILD_BYPRODUCTS
                "${_PEDIGREE_KEYMAP_PATH}"
                "${_PEDIGREE_HEADERIFY_PATH}"
                "${_PEDIGREE_EXT2IMG_PATH}"
            INSTALL_COMMAND ""
            TEST_COMMAND ""
            USES_TERMINAL_CONFIGURE TRUE
            USES_TERMINAL_BUILD TRUE)

        if ("host-keymap" IN_LIST PEDIGREE_HOST_TOOLS_REQUIRED_TARGETS)
            _pedigree_import_nested_host_tool(
                host-keymap keymap pedigree-host-distribution-tools
                "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}")
        endif ()
        if ("host-headerify" IN_LIST PEDIGREE_HOST_TOOLS_REQUIRED_TARGETS)
            _pedigree_import_nested_host_tool(
                host-headerify headerify pedigree-host-distribution-tools
                "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}")
        endif ()
        if ("host-ext2img" IN_LIST PEDIGREE_HOST_TOOLS_REQUIRED_TARGETS)
            _pedigree_import_nested_host_tool(
                host-ext2img ext2img pedigree-host-distribution-tools
                "${_PEDIGREE_HOST_TOOLCHAIN_STAGE_DIR}")
        endif ()
    endif ()

    set(PEDIGREE_HOST_TOOLS_STAGE_DIR
        "${PEDIGREE_HOST_TOOLS_STAGE_DIR}" PARENT_SCOPE)
endfunction()

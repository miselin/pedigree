# This is a native toolchain description: it intentionally leaves
# CMAKE_SYSTEM_NAME unset so CMake uses the Pedigree host as the target and
# keeps CMAKE_CROSSCOMPILING false.
string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" _PEDIGREE_SELFHOST_SYSTEM_NAME)
if (NOT _PEDIGREE_SELFHOST_SYSTEM_NAME STREQUAL "pedigree")
    message(FATAL_ERROR
        "pedigree_selfhost.cmake can only be used by CMake running on Pedigree")
endif ()
unset(_PEDIGREE_SELFHOST_SYSTEM_NAME)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

set(PEDIGREE_NATIVE_TOOL_ROOT "/usr" CACHE PATH
    "Installation prefix for Pedigree-native build tools")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PEDIGREE_NATIVE_TOOL_ROOT)

function(pedigree_find_native_program variable)
    # A build directory may be reused after installing a replacement native
    # toolchain. Resolve every companion from the selected prefix again.
    unset(${variable} CACHE)
    unset(${variable})
    if (PEDIGREE_NATIVE_TOOL_ROOT)
        find_program(${variable} NAMES ${ARGN}
            PATHS "${PEDIGREE_NATIVE_TOOL_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
    else ()
        find_program(${variable} NAMES ${ARGN} REQUIRED)
    endif ()
    set(${variable} "${${variable}}" PARENT_SCOPE)
endfunction()

pedigree_find_native_program(PEDIGREE_C_COMPILER gcc)
pedigree_find_native_program(PEDIGREE_CXX_COMPILER g++)
pedigree_find_native_program(PEDIGREE_ASM_NASM_COMPILER nasm)
pedigree_find_native_program(PEDIGREE_ADDR2LINE addr2line)
pedigree_find_native_program(PEDIGREE_AR ar)
pedigree_find_native_program(PEDIGREE_GCC_AR gcc-ar)
pedigree_find_native_program(PEDIGREE_GCC_RANLIB gcc-ranlib)
pedigree_find_native_program(PEDIGREE_LINKER ld)
pedigree_find_native_program(PEDIGREE_NM nm)
pedigree_find_native_program(PEDIGREE_OBJCOPY objcopy)
pedigree_find_native_program(PEDIGREE_OBJDUMP objdump)
pedigree_find_native_program(PEDIGREE_RANLIB ranlib)
pedigree_find_native_program(PEDIGREE_READELF readelf)
pedigree_find_native_program(PEDIGREE_STRIP strip)

foreach(compiler IN ITEMS
    "${PEDIGREE_C_COMPILER}" "${PEDIGREE_CXX_COMPILER}")
    execute_process(
        COMMAND "${compiler}" -dumpmachine
        RESULT_VARIABLE PEDIGREE_DUMPMACHINE_RESULT
        OUTPUT_VARIABLE PEDIGREE_DUMPMACHINE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (NOT PEDIGREE_DUMPMACHINE_RESULT EQUAL 0 OR
        NOT PEDIGREE_DUMPMACHINE STREQUAL "x86_64-pedigree")
        message(FATAL_ERROR
            "The native compiler must target x86_64-pedigree; "
            "${compiler} reports '${PEDIGREE_DUMPMACHINE}'")
    endif ()
endforeach()
unset(compiler)
unset(PEDIGREE_DUMPMACHINE)
unset(PEDIGREE_DUMPMACHINE_RESULT)

function(pedigree_native_runtime_directory variable compiler query)
    execute_process(
        COMMAND "${compiler}" "${query}"
        RESULT_VARIABLE runtime_result
        OUTPUT_VARIABLE runtime_library
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (NOT runtime_result EQUAL 0 OR
        NOT IS_ABSOLUTE "${runtime_library}" OR
        NOT EXISTS "${runtime_library}" OR
        IS_DIRECTORY "${runtime_library}")
        message(FATAL_ERROR
            "Unable to locate the x86_64-pedigree compiler runtime with "
            "'${compiler} ${query}'; got '${runtime_library}'")
    endif ()

    get_filename_component(runtime_directory "${runtime_library}" DIRECTORY)
    set(${variable} "${runtime_directory}" PARENT_SCOPE)
endfunction()

pedigree_native_runtime_directory(PEDIGREE_NATIVE_LIBGCC_DIRECTORY
    "${PEDIGREE_C_COMPILER}" -print-libgcc-file-name)
pedigree_native_runtime_directory(PEDIGREE_NATIVE_LIBSTDCXX_DIRECTORY
    "${PEDIGREE_CXX_COMPILER}" -print-file-name=libstdc++.a)
set(PEDIGREE_NATIVE_RUNTIME_LIBRARY_DIRS
    "${PEDIGREE_NATIVE_LIBGCC_DIRECTORY}"
    "${PEDIGREE_NATIVE_LIBSTDCXX_DIRECTORY}")
list(REMOVE_DUPLICATES PEDIGREE_NATIVE_RUNTIME_LIBRARY_DIRS)
unset(PEDIGREE_NATIVE_LIBGCC_DIRECTORY)
unset(PEDIGREE_NATIVE_LIBSTDCXX_DIRECTORY)

set(CMAKE_ADDR2LINE "${PEDIGREE_ADDR2LINE}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${PEDIGREE_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${PEDIGREE_C_COMPILER}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER_AR "${PEDIGREE_GCC_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER_RANLIB "${PEDIGREE_GCC_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_NASM_COMPILER "${PEDIGREE_ASM_NASM_COMPILER}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER "${PEDIGREE_C_COMPILER}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_AR "${PEDIGREE_GCC_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_RANLIB "${PEDIGREE_GCC_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${PEDIGREE_CXX_COMPILER}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_AR "${PEDIGREE_GCC_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${PEDIGREE_GCC_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${PEDIGREE_LINKER}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${PEDIGREE_NM}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${PEDIGREE_OBJCOPY}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${PEDIGREE_OBJDUMP}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${PEDIGREE_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_READELF "${PEDIGREE_READELF}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${PEDIGREE_STRIP}" CACHE FILEPATH "" FORCE)

include("${CMAKE_CURRENT_LIST_DIR}/pedigree_amd64_target.cmake")

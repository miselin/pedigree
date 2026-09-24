include_guard(GLOBAL)

function(pedigree_define_musl_sdk_headers)
    if (TARGET pedigree_musl_headers)
        return()
    endif ()
    if (NOT PEDIGREE_MUSL_PREFIX_ROOT)
        message(FATAL_ERROR
            "PEDIGREE_MUSL_PREFIX_ROOT is required for target headers")
    endif ()

    add_library(pedigree_musl_headers INTERFACE)
    file(REAL_PATH "${PEDIGREE_MUSL_PREFIX_ROOT}/include"
        _PEDIGREE_CANONICAL_INCLUDE_DIR)
    # GCC's C++ wrappers use include_next to reach libc. Rebuild its implicit
    # search order with the active SDK replacing the installed libc directory;
    # a leading -isystem would put libc before those wrappers and break them.
    foreach (_PEDIGREE_LANGUAGE C CXX)
        set(_PEDIGREE_IMPLICIT_INCLUDE_DIRS
            ${CMAKE_${_PEDIGREE_LANGUAGE}_IMPLICIT_INCLUDE_DIRECTORIES})
        target_compile_options(pedigree_musl_headers INTERFACE
            "$<$<COMPILE_LANGUAGE:${_PEDIGREE_LANGUAGE}>:-nostdinc>")
        foreach (_PEDIGREE_INCLUDE_DIR IN LISTS
                 _PEDIGREE_IMPLICIT_INCLUDE_DIRS)
            if (_PEDIGREE_INCLUDE_DIR STREQUAL "/usr/include" OR
                _PEDIGREE_INCLUDE_DIR STREQUAL "${PEDIGREE_MUSL_PREFIX_ROOT}/include" OR
                _PEDIGREE_INCLUDE_DIR STREQUAL "${_PEDIGREE_CANONICAL_INCLUDE_DIR}" OR
                _PEDIGREE_INCLUDE_DIR STREQUAL "${CMAKE_SYSROOT}/include" OR
                _PEDIGREE_INCLUDE_DIR STREQUAL "${CMAKE_SYSROOT}/usr/include" OR
                _PEDIGREE_INCLUDE_DIR MATCHES
                    "/${PEDIGREE_COMPILER_TARGET}/include/?$")
                continue()
            endif ()
            target_compile_options(pedigree_musl_headers INTERFACE
                "$<$<COMPILE_LANGUAGE:${_PEDIGREE_LANGUAGE}>:-isystem${_PEDIGREE_INCLUDE_DIR}>")
        endforeach ()
        target_compile_options(pedigree_musl_headers INTERFACE
            "$<$<COMPILE_LANGUAGE:${_PEDIGREE_LANGUAGE}>:-isystem${PEDIGREE_MUSL_PREFIX_ROOT}/include>")
        # Imported packages can advertise the same SDK include directory. CMake
        # would emit those includes before the ordered options above. Mark both
        # spellings as already supplied so it does not move libc before C++.
        list(APPEND _PEDIGREE_IMPLICIT_INCLUDE_DIRS
            "${PEDIGREE_MUSL_PREFIX_ROOT}/include"
            "${_PEDIGREE_CANONICAL_INCLUDE_DIR}")
        list(REMOVE_DUPLICATES _PEDIGREE_IMPLICIT_INCLUDE_DIRS)
        set(CMAKE_${_PEDIGREE_LANGUAGE}_IMPLICIT_INCLUDE_DIRECTORIES
            "${_PEDIGREE_IMPLICIT_INCLUDE_DIRS}" PARENT_SCOPE)
        if (PEDIGREE_SELF_HOSTED)
            if (PEDIGREE_MUSL_INSTALLED_INCLUDE_DIR)
                set(_PEDIGREE_INSTALLED_INCLUDE_DIR
                    "${PEDIGREE_MUSL_INSTALLED_INCLUDE_DIR}")
            else ()
                set(_PEDIGREE_INSTALLED_INCLUDE_DIR "/usr/include")
            endif ()
            # Installed package headers remain available, but may not shadow
            # the libc headers in the selected SDK.
            target_compile_options(pedigree_musl_headers INTERFACE
                "$<$<COMPILE_LANGUAGE:${_PEDIGREE_LANGUAGE}>:-idirafter${_PEDIGREE_INSTALLED_INCLUDE_DIR}>")
        endif ()
    endforeach ()
endfunction()

function(pedigree_use_musl_sdk_startfiles target)
    if (PEDIGREE_ARCH_TARGET MATCHES "^(X64|ARM64|ARMV7)$")
        if (NOT PEDIGREE_MUSL_PREFIX_ROOT)
            message(FATAL_ERROR
                "PEDIGREE_MUSL_PREFIX_ROOT is required for target user links")
        endif ()

        # GCC does not use -L when locating crt*.o. Prefer the CRT objects from
        # the selected SDK over those installed with the compiler.
        target_link_options(${target} PRIVATE
            "-B${PEDIGREE_MUSL_PREFIX_ROOT}/lib/")
        get_target_property(target_type ${target} TYPE)
        if (target_type STREQUAL "EXECUTABLE")
            if (NOT PEDIGREE_MUSL_DYNAMIC_LOADER)
                message(FATAL_ERROR "PEDIGREE_MUSL_DYNAMIC_LOADER is required for target executables")
            endif ()
            target_link_options(${target} PRIVATE
                "-Wl,--dynamic-linker,${PEDIGREE_MUSL_DYNAMIC_LOADER}")
        endif ()
    endif ()
endfunction()

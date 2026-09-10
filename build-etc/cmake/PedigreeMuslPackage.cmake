include_guard(GLOBAL)

set(PEDIGREE_PUP_COMMAND "" CACHE STRING
    "PUP command (a CMake list); defaults to pup or Python's pedigree_updater module")
set(PEDIGREE_MUSL_PACKAGE_CACHE "${CMAKE_BINARY_DIR}/musl-pup-cache" CACHE PATH
    "Download cache for the pinned musl PUP")
set(PEDIGREE_PUP_SERVER "https://pup.pedigree-project.org" CACHE STRING
    "PUP repository for build dependencies")

function(pedigree_acquire_musl_package)
    if (NOT PEDIGREE_TARGET_PAGE_SIZE EQUAL 4096)
        message(FATAL_ERROR "The target musl PUP requires 4096-byte pages")
    endif ()
    if (NOT PEDIGREE_PUP_COMMAND)
        find_program(_PEDIGREE_PUP NAMES pup
            HINTS "${CMAKE_SOURCE_DIR}/.venv/bin")
        if (_PEDIGREE_PUP)
            set(PEDIGREE_PUP_COMMAND "${_PEDIGREE_PUP}")
        else ()
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" -c "import pedigree_updater"
                RESULT_VARIABLE pup_available OUTPUT_QUIET ERROR_QUIET)
            if (pup_available EQUAL 0)
                set(PEDIGREE_PUP_COMMAND "${Python3_EXECUTABLE};-m;pedigree_updater")
            else ()
                set(PEDIGREE_PUP_WHEEL "${CMAKE_BINARY_DIR}/pup-client/pup.whl"
                    CACHE FILEPATH "PUP client wheel for builds without installed PUP")
                if (NOT EXISTS "${PEDIGREE_PUP_WHEEL}")
                    get_filename_component(client_dir "${PEDIGREE_PUP_WHEEL}" DIRECTORY)
                    file(MAKE_DIRECTORY "${client_dir}")
                    file(DOWNLOAD "${PEDIGREE_PUP_SERVER}/pup.whl"
                        "${PEDIGREE_PUP_WHEEL}.download"
                        TLS_VERIFY ON STATUS download_status TIMEOUT 120)
                    list(GET download_status 0 download_result)
                    if (NOT download_result EQUAL 0)
                        file(REMOVE "${PEDIGREE_PUP_WHEEL}.download")
                        message(FATAL_ERROR "Could not download PUP: ${download_status}")
                    endif ()
                    file(RENAME "${PEDIGREE_PUP_WHEEL}.download" "${PEDIGREE_PUP_WHEEL}")
                endif ()
                set(PEDIGREE_PUP_COMMAND
                    "${Python3_EXECUTABLE};${PEDIGREE_PUP_WHEEL}/pedigree_updater")
            endif ()
        endif ()
        set(PEDIGREE_PUP_COMMAND "${PEDIGREE_PUP_COMMAND}" CACHE STRING
            "PUP command (a CMake list)" FORCE)
    endif ()
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            "-DSDK_ROOT=${PEDIGREE_MUSL_SDK_ROOT}"
            "-DPUP_COMMAND=${PEDIGREE_PUP_COMMAND}"
            "-DPUP_CACHE=${PEDIGREE_MUSL_PACKAGE_CACHE}"
            "-DPUP_SERVER=${PEDIGREE_PUP_SERVER}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/InstallMuslPackage.cmake"
        RESULT_VARIABLE result)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR
            "Could not acquire the musl SDK. Install PUP with its Python dependencies "
            "or set PEDIGREE_PUP_COMMAND; "
            "see docs/musl-sdk.md for offline builds.")
    endif ()
endfunction()

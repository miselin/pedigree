cmake_minimum_required(VERSION 3.20)

foreach (required_variable
        PEDIGREE_EXTERNAL_WINMAN_EXPECTED_OUTPUTS
        PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP
        PEDIGREE_EXTERNAL_WINMAN_DISABLED_STAMP)
    if (NOT DEFINED ${required_variable} OR
        "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

set(changed OFF)
foreach (output IN LISTS PEDIGREE_EXTERNAL_WINMAN_EXPECTED_OUTPUTS)
    if (EXISTS "${output}")
        file(REMOVE "${output}")
        if (EXISTS "${output}")
            message(FATAL_ERROR "Could not remove staged artifact: ${output}")
        endif ()
        set(changed ON)
    endif ()
endforeach ()

if (EXISTS "${PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP}")
    file(REMOVE "${PEDIGREE_EXTERNAL_WINMAN_INSTALL_STAMP}")
    set(changed ON)
endif ()
if (NOT EXISTS "${PEDIGREE_EXTERNAL_WINMAN_DISABLED_STAMP}")
    set(changed ON)
endif ()
if (changed)
    file(TOUCH "${PEDIGREE_EXTERNAL_WINMAN_DISABLED_STAMP}")
endif ()

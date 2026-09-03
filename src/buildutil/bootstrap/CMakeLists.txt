if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(PedigreeBootstrapUtilities C)
    include(CTest)
endif ()

find_package(ZLIB REQUIRED)

add_executable(pedigree-initrd-builder initrd/main.c)
target_link_libraries(pedigree-initrd-builder PRIVATE ZLIB::ZLIB)
set_target_properties(pedigree-initrd-builder PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

if (BUILD_TESTING AND PYTHON_EXECUTABLE)
    add_test(NAME initrd-builder-tests
        COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/initrd/test.py"
            "$<TARGET_FILE:pedigree-initrd-builder>" -v)
endif ()

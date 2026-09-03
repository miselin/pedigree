if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(PedigreeConfigDb C)
    include(CTest)
endif ()

set(PEDIGREE_SQLITE_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/../../modules/system/config/sqlite3/sqlite3.c")

add_library(pedigree-configdb-sqlite STATIC ${PEDIGREE_SQLITE_SOURCE})
target_compile_definitions(pedigree-configdb-sqlite PRIVATE
    SQLITE_DISABLE_LFS=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_FLOATING_POINT=1
    SQLITE_OS_UNIX=1
    SQLITE_TEMP_STORE=3
    SQLITE_THREADSAFE=0)
# The checked-in SQLite amalgamation predates the warning sets used by the
# rest of the tree and is maintained as third-party source.
target_compile_options(pedigree-configdb-sqlite PRIVATE -w)

add_executable(pedigree-configdb main.c)
target_include_directories(pedigree-configdb PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../modules/system/config/sqlite3")
target_link_libraries(pedigree-configdb PRIVATE pedigree-configdb-sqlite)
set_target_properties(pedigree-configdb PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES)

if (BUILD_TESTING AND PYTHON_EXECUTABLE)
    add_test(NAME configdb-generator-tests
        COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test.py"
            "$<TARGET_FILE:pedigree-configdb>" -v)
endif ()

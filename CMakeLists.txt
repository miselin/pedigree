project(Pedigree)
cmake_minimum_required(VERSION 3.3)

if (PEDIGREE_MACHDIR STREQUAL "mach_pc")
    # All nasm on x86-based systems
    enable_language(ASM_NASM)

    set(CMAKE_ASM_NASM_FLAGS ${CMAKE_ASM_NASM_FLAGS} "-felf64")
endif ()

cmake_host_system_information(RESULT PEDIGREE_BUILD_HOSTNAME QUERY FQDN)

# Set up some general default flags for the entire project.
# NOTE: don't include optimization flags - they need to be added elsewhere based on input flags.
set(GENERIC_COMPILE_FLAGS "-fvisibility=hidden" "-pipe" "-O0")
set(GENERIC_COMPILE_CFLAGS "-std=gnu99")
set(GENERIC_COMPILE_CXXFLAGS "-std=gnu++11")

# Set up warning flags for the full build.
set(GENERIC_WARNING_FLAGS
    "-Wall" "-Wextra" "-Wpointer-arith" "-Wcast-align" "-Wwrite-strings"
    "-Wno-long-long" "-Wvariadic-macros" "-Wno-unused-parameter"
    "-Wuninitialized" "-Wstrict-aliasing" "-Wsuggest-attribute=noreturn"
    "-Wtrampolines" "-Wfloat-equal" "-Wundef" "-Wcast-qual" "-Wlogical-op"
    "-Wdisabled-optimization")
set(GENERIC_WARNING_CFLAGS "-Wnested-externs" "-Wbad-function-cast")
set(GENERIC_WARNING_CXXFLAGS
    "-Wsign-promo" "-Woverloaded-virtual" "-Wnon-virtual-dtor"
    "-Wctor-dtor-privacy" "-Wabi" "-Wuseless-cast")
set(GENERIC_WARNING_DISABLES "-Wno-unused" "-Wno-unused-variable"
    "-Wno-conversion" "-Wno-format" "-Wno-packed-bitfield-compat"
    "-Wno-error=disabled-optimization" "-Wno-error=deprecated-declarations"
    "-Wno-abi")

set(GENERIC_LINKFLAGS "-fvisibility=hidden")

# Prepare flags for all builds across the entire project.
set(GENERIC_CFLAGS
    ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CFLAGS} ${GENERIC_WARNING_FLAGS}
    ${GENERIC_WARNING_CFLAGS} ${GENERIC_WARNING_DISABLES})
set(GENERIC_CXXFLAGS ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CXXFLAGS}
    ${GENERIC_WARNING_FLAGS} ${GENERIC_WARNING_CXXFLAGS}
    ${GENERIC_WARNING_DISABLES})

# Set up include directories for most targets
include_directories(src src/system/include)

if(CMAKE_CROSSCOMPILING)
    set(IMPORT_EXECUTABLES "IMPORTFILE-NOTFOUND" CACHE FILEPATH "Path to an exports file from a native build to be able to find build and test utilities.")
    include(${IMPORT_EXECUTABLES})

    add_subdirectory(src/system/kernel)
else ()
    add_subdirectory(external/googletest)
    add_subdirectory(src/buildutil)
endif ()

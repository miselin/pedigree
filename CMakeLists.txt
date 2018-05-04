# Add our internal CMake modules path before project() to enable our custom platform
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/build-etc/cmake")

project(Pedigree)
cmake_minimum_required(VERSION 3.3)

find_package(BISON)
find_package(FLEX)
find_package(PythonInterp)
find_program(MKISOFS NAMES mkisofs genisoimage xorriso)

# Flags that affect the entire build.
set(PEDIGREE_DEBUGGER TRUE CACHE BOOL "Enable the kernel debugger.")
set(PEDIGREE_ADDITIONAL_CHECKS TRUE CACHE BOOL "Enable additional checks on some operations across the kernel.")
set(PEDIGREE_CRIPPLE_HDD TRUE CACHE BOOL "Disable writing to hard disks at runtime.")
set(PEDIGREE_ASSERTS TRUE CACHE BOOL "Enable runtime assertions.")
set(PEDIGREE_DEBUG_LOGGING TRUE CACHE BOOL "Whether to enable debug-level logging, which can dump massive amounts of data to the kernel log. Probable performance hit too, use at your own risk.")
set(PEDIGREE_SUPERDEBUG FALSE CACHE BOOL "Enable superdebug. Superdebug is like enabling PEDIGREE_DEBUG_LOGGING, except even MORE verbose. Expect hundreds of thousands of lines of output to the kernel log.")
set(PEDIGREE_TRACING TRUE CACHE BOOL "Enable kernel traces, which may output to the screen or a spare serial port independently of the kernel log.")
set(PEDIGREE_DEBUG_ALLOCATOR FALSE CACHE BOOL "Enable the debug allocator, which is a very trimmed-down version of the normal kernel allocator and helps find underflows/overflows.")
set(PEDIGREE_SERIAL_IS_FILE TRUE CACHE BOOL "Set to true if the serial port will be connected to a file, to avoid filling the file with terminal codes.")
set(PEDIGREE_MEMORY_LOG TRUE CACHE BOOL "Enable periodic memory usage reporting to the second serial port.")
set(PEDIGREE_MEMORY_LOG_INLINE FALSE CACHE BOOL "Enable the periodic memory usage report on the primary serial port.")
set(PEDIGREE_MEMORY_TRACING FALSE CACHE BOOL "Enable tracing memory allocations and frees for use with the 'memorytracer' utility.")
set(PEDIGREE_TRACK_LOCKS TRUE CACHE BOOL "Enable lock tracking, which performs very rudimentary deadlock detection and lock state validation.")
set(PEDIGREE_MULTIPROCESSOR TRUE CACHE BOOL "Enable multiprocessor support for processor targets that support it.")
set(PEDIGREE_ACPI TRUE CACHE BOOL "Enable ACPI support for machine targets that support it.")
set(PEDIGREE_GRAPHICS FALSE CACHE BOOL "Enable graphics, including the graphical splash screen.")
set(PEDIGREE_TRACK_HIDDEN_SYMBOLS FALSE CACHE BOOL "Enable tracking of hidden symbols, which increases kernel memory usage but dramatically improves debugger backtraces.")

# General build options.
set(PEDIGREE_LTO FALSE CACHE BOOL "Enable LTO.")
set(PEDIGREE_TRAVIS FALSE CACHE BOOL "Enable to report this build is happening on travis-ci.")
set(PEDIGREE_OPTIMIZE TRUE CACHE BOOL "Build an optimized kernel and modules.")
set(PEDIGREE_OPTIMIZE_SIZE TRUE CACHE BOOL "If building an optimized kernel, optimize for size.")
set(PEDIGREE_WARNINGS FALSE CACHE BOOL "Allow compiler warnings.")

# Build artifact controls.
set(PEDIGREE_LIVECD FALSE CACHE BOOL "Build a live CD version of the ISO image, which includes a hard disk image and relevant customizations.")

if (PEDIGREE_MACHDIR STREQUAL "mach_pc")
    # All nasm on x86-based systems
    enable_language(ASM_NASM)

    set(CMAKE_ASM_NASM_FLAGS ${CMAKE_ASM_NASM_FLAGS} "-felf64")
endif ()

cmake_host_system_information(RESULT PEDIGREE_BUILD_HOSTNAME QUERY FQDN)

if (NOT PEDIGREE_GRAPHICS)
    add_definitions(-DNOGFX=1)
endif ()

if (PEDIGREE_CRIPPLE_HDD)
    add_definitions(-DCRIPPLE_HDD=1)
endif ()

if (PEDIGREE_DEBUGGER)
    add_definitions(-DDEBUGGER=1)
endif ()

if (PEDIGREE_ADDITIONAL_CHECKS)
    add_definitions(-DADDITIONAL_CHECKS=1)
endif ()

if (PEDIGREE_ASSERTS)
    add_definitions(-DASSERTS=1)
endif ()

if (PEDIGREE_DEBUG_LOGGING)
    add_definitions(-DDEBUG_LOGGING=1)
endif ()

if (PEDIGREE_SUPERDEBUG)
    add_definitions(-DSUPERDEBUG=1)
endif ()

if (PEDIGREE_TRACING)
    add_definitions(-DTRACING=1)
endif ()

if (PEDIGREE_DEBUG_ALLOCATOR)
    add_definitions(-DDEBUG_ALLOCATOR=1)
endif ()

if (PEDIGREE_SERIAL_IS_FILE)
    add_definitions(-DSERIAL_IS_FILE=1)
endif ()

if (PEDIGREE_MEMORY_LOG)
    add_definitions(-DMEMORY_LOGGING_ENABLED=1)
endif ()

if (PEDIGREE_MEMORY_LOG_INLINE)
    add_definitions(-DMEMORY_LOG_INLINE=1)
endif ()

if (PEDIGREE_MEMORY_TRACING)
    add_definitions(-DMEMORY_TRACING=1)
endif ()

if (PEDIGREE_TRACK_LOCKS)
    add_definitions(-DTRACK_LOCKS=1)
endif ()

if (PEDIGREE_MULTIPROCESSOR)
    add_definitions(-DMULTIPROCESSOR=1)
endif ()

if (PEDIGREE_ACPI)
    add_definitions(-DACPI=1)
endif ()

if (PEDIGREE_GRAPHICS)
    add_definitions(-DGRAPHICS=1)
endif ()

if (PEDIGREE_TRACK_HIDDEN_SYMBOLS)
    add_definitions(-DTRACK_HIDDEN_SYMBOLS=1)
endif ()


if (PEDIGREE_LIVECD)
    add_definitions(-DLIVECD=1)
endif ()

# Set up some general default flags for the entire project.
# NOTE: don't include optimization flags - they need to be added elsewhere based on input flags.
set(GENERIC_COMPILE_FLAGS "-fvisibility=hidden" "-pipe")
set(GENERIC_COMPILE_CFLAGS "-std=gnu99")
set(GENERIC_COMPILE_CXXFLAGS "-std=gnu++11")
set(GENERIC_LINKFLAGS "-fvisibility=hidden")

if (NOT PEDIGREE_WARNINGS)
    set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -Werror)
endif ()

if (PEDIGREE_OPTIMIZE)
    if (PEDIGREE_OPTIMIZE_SIZE)
        set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -Os)
    else ()
        set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -O3)
    endif ()
else ()
    set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -O0)
endif ()

if (PEDIGREE_LTO)
    set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -flto)
    set(GENERIC_LINKFLAGS ${GENERIC_LINKFLAGS} -flto)
endif ()

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

# Prepare flags for all builds across the entire project.
set(GENERIC_CFLAGS
    ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CFLAGS} ${GENERIC_WARNING_FLAGS}
    ${GENERIC_WARNING_CFLAGS} ${GENERIC_WARNING_DISABLES})
set(GENERIC_CXXFLAGS ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CXXFLAGS}
    ${GENERIC_WARNING_FLAGS} ${GENERIC_WARNING_CXXFLAGS}
    ${GENERIC_WARNING_DISABLES})

# Set up include directories for most targets
include_directories(src src/system/include)

# Always do this regardless of cross-compile state
BISON_TARGET(KeymapParser
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/parser.y
    ${CMAKE_BINARY_DIR}/src/user/applications/keymap/keymap-parser.c)
FLEX_TARGET(KeymapScanner
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/lexer.l
    ${CMAKE_BINARY_DIR}/src/user/applications/keymap/keymap-lexer.c)
ADD_FLEX_BISON_DEPENDENCY(KeymapScanner KeymapParser)

# Build flex/bison files into a static library for use in targets that need it
add_library(libkeymap STATIC
    ${BISON_KeymapParser_OUTPUTS}
    ${FLEX_KeymapScanner_OUTPUTS})
target_include_directories(libkeymap PRIVATE
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap)

if(CMAKE_CROSSCOMPILING)
    set(IMPORT_EXECUTABLES "IMPORTFILE-NOTFOUND" CACHE FILEPATH "Path to an exports file from a native build to be able to find build and test utilities.")
    include(${IMPORT_EXECUTABLES})

    add_subdirectory(src/system/kernel)
    add_subdirectory(src/modules)
    add_subdirectory(src/user)

    # Build ISO file to run with
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pedigree.iso
        COMMAND ${MKISOFS} -D -joliet -graft-points -quiet -input-charset iso8859-1
        -R -b boot/grub/stage2_eltorito -no-emul-boot -boot-load-size 4 -boot-info-table
        -o ${CMAKE_CURRENT_BINARY_DIR}/pedigree.iso -V PEDIGREE
        boot/grub/stage2_eltorito=${CMAKE_SOURCE_DIR}/images/grub/stage2_eltorito-x64
        boot/grub/menu.lst=${CMAKE_SOURCE_DIR}/images/grub/menu.lst
        boot/kernel=${CMAKE_BINARY_DIR}/src/system/kernel/kernel-mini64
        boot/initrd.tar=${CMAKE_BINARY_DIR}/src/modules/initrd.tar
        .pedigree-root=${CMAKE_SOURCE_DIR}/build/config.db
        DEPENDS kernel kernelfinal ${CMAKE_BINARY_DIR}/src/modules/initrd.tar)
    add_custom_target(livecd ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/pedigree.iso)
else ()
    set(POSITION_INDEPENDENT_CODE TRUE)

    add_subdirectory(external/googletest)
    add_subdirectory(src/buildutil)
endif ()

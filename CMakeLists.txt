# Add our internal CMake modules path before project() to enable our custom platform
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/build-etc/cmake")

SET(CMAKE_BUILD_TYPE_INIT Debug)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(Pedigree)
cmake_minimum_required(VERSION 3.3)

if (NOT CMAKE_CXX_COMPILER_VERSION)
    # TODO: keep this up to date with checkBuildSystemNoInteractive.pl
    set(CMAKE_C_COMPILER_VERSION 8.3.0)
    set(CMAKE_CXX_COMPILER_VERSION 8.3.0)
endif ()

include(CTest)
enable_testing()

include(FindGettext)

find_package(BISON)
find_package(FLEX)
find_package(PythonInterp)
find_program(MKISOFS NAMES mkisofs genisoimage xorriso)

find_library(LIBRT rt)

# No rpath for anything.
set(CMAKE_SKIP_RPATH TRUE)

# Flags that affect the entire build.
set(PEDIGREE_DEBUGGER TRUE CACHE BOOL "Enable the kernel debugger.")
set(PEDIGREE_ADDITIONAL_CHECKS TRUE CACHE BOOL "Enable additional checks on some operations across the kernel.")
set(PEDIGREE_EXCESSIVE_ADDITIONAL_CHECKS FALSE CACHE BOOL "Enable even more additional checks on some operations across the kernel, many of which have a massive impact on performance.")
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
set(PEDIGREE_PEDANTIC FALSE CACHE BOOL "Enable pedantic mode for some warnings in Pedigree which indicate bad practices, but are otherwise handled correctly by the code emitting the warning.")
set(PEDIGREE_DEBUGGER_RUN_AT_START FALSE CACHE BOOL "Enter directly into the debugger during startup.")
set(PEDIGREE_LOG_TO_SERIAL TRUE CACHE BOOL "Emit the kernel log to the serial port.")
set(PEDIGREE_STATIC_DRIVERS FALSE CACHE BOOL "Link all drivers directly into the kernel instead of loading them at runtime.")
set(PEDIGREE_STRICT_LOCK_ORDERING FALSE CACHE BOOL "Enforce strict lock ordering for all types of locks in the kernel.")
set(PEDIGREE_LOGGING TRUE CACHE BOOL "Enable the kernel logging facility.")
set(PEDIGREE_TRACK_PAGE_ALLOCS FALSE CACHE BOOL "Track physical page allocations in the debugger allocations command.")
set(PEDIGREE_VERBOSE_MEMORY_MANAGER FALSE CACHE BOOL "Be more verbose in PhysicalMemoryManager.")

# init configuration
set(PEDIGREE_WITH_INIT TRUE CACHE BOOL "Build and run the init module at startup. It can be useful to skip running init to test without having the system start an entire userspace.")

# Hosted build options
# TODO: detect HOSTED=1 and cross compiling and error out!
set(PEDIGREE_HOSTED FALSE CACHE BOOL "Run a hosted kernel build, which is not compatible with a cross-compiled build.")
set(PEDIGREE_HOSTED_SYSTEM_MALLOC FALSE CACHE BOOL "Use the system malloc instead of Pedigree's in hosted builds.")

# General build options.
set(PEDIGREE_LTO FALSE CACHE BOOL "Enable LTO.")
set(PEDIGREE_TRAVIS FALSE CACHE BOOL "Enable to report this build is happening on travis-ci.")
set(PEDIGREE_OPTIMIZE TRUE CACHE BOOL "Build an optimized kernel and modules.")
set(PEDIGREE_OPTIMIZE_SIZE TRUE CACHE BOOL "If building an optimized kernel, optimize for size.")
set(PEDIGREE_WARNINGS FALSE CACHE BOOL "Allow compiler warnings.")

# Build artifact controls.
set(PEDIGREE_LIVECD FALSE CACHE BOOL "Build a live CD version of the ISO image, which includes a hard disk image and relevant customizations.")
set(PEDIGREE_IMAGESDIR "${CMAKE_SOURCE_DIR}/images/local" CACHE PATH "Directory containing the directory tree to build disk images from.")

# All nasm on x86-based systems
enable_language(ASM_NASM ASM)

# Fix up nasm command line to not include a default like -f elf (so we can
# control the bitness of the nasm output)
set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> -felf64 -o <OBJECT> <SOURCE>")

cmake_host_system_information(RESULT PEDIGREE_BUILD_HOSTNAME QUERY FQDN)

# Set up some general default flags for the entire project.
# NOTE: don't include optimization flags - they need to be added elsewhere based on input flags.
set(GENERIC_COMPILE_FLAGS "-fvisibility=hidden" "-pipe")
set(GENERIC_COMPILE_CFLAGS "-std=gnu99")
set(GENERIC_COMPILE_CXXFLAGS "")
set(GENERIC_LINKFLAGS "-fvisibility=hidden")

if (NOT PEDIGREE_WARNINGS)
    set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -Werror)
endif ()

if (PEDIGREE_LTO)
    set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -flto)
    set(GENERIC_LINKFLAGS ${GENERIC_LINKFLAGS} -flto)
endif ()

if (PEDIGREE_TRAVIS)
    message("We appear to be building on Travis-CI.")
endif ()

if (CMAKE_COMPILER_IS_GNU)
    set(COMPILER_SPECIFIC_WARNINGS "-Wlogical-op" "-Wsuggest-attribute=noreturn" "-Wtrampolines")
    set(COMPILER_SPECIFIC_WARNINGS_C "")
    set(COMPILER_SPECIFIC_WARNINGS_CXX "-Wuseless-cast")
    set(COMPILER_SPECIFIC_WARNING_DISABLES "-Wno-packed-bitfield-compat")
else ()
    set(COMPILER_SPECIFIC_WARNINGS "")
    set(COMPILER_SPECIFIC_WARNINGS_C "")
    set(COMPILER_SPECIFIC_WARNINGS_CXX "")
    set(COMPILER_SPECIFIC_WARNING_DISABLES "")
endif ()

# Set up warning flags for the full build.
# TODO: add "-Wundef" back in
set(GENERIC_WARNING_FLAGS "-Wall" "-Wextra" "-Wpointer-arith" "-Wcast-align" "-Wwrite-strings" "-Wno-long-long" "-Wvariadic-macros" "-Wno-unused-parameter" "-Wuninitialized" "-Wstrict-aliasing" "-Wfloat-equal" "-Wcast-qual" "-Wdisabled-optimization" ${COMPILER_SPECIFIC_WARNINGS})
set(GENERIC_WARNING_CFLAGS "-Wnested-externs" "-Wbad-function-cast" ${COMPILER_SPECIFIC_WARNINGS_C})
set(GENERIC_WARNING_CXXFLAGS "-Wsign-promo" "-Woverloaded-virtual" "-Wnon-virtual-dtor" "-Wctor-dtor-privacy" "-Wabi" ${COMPILER_SPECIFIC_WARNINGS_CXX})
set(GENERIC_WARNING_DISABLES "-Wno-unused" "-Wno-unused-variable" "-Wno-conversion" "-Wno-format" "-Wno-error=disabled-optimization" "-Wno-error=deprecated-declarations" "-Wno-abi" ${COMPILER_SPECIFIC_WARNING_DISABLES})

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
    ${CMAKE_BINARY_DIR}/keymap-parser.c)
FLEX_TARGET(KeymapScanner
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/lexer.l
    ${CMAKE_BINARY_DIR}/keymap-lexer.c)
ADD_FLEX_BISON_DEPENDENCY(KeymapScanner KeymapParser)

# Build flex/bison files into a static library for use in targets that need it
add_library(libkeymap STATIC
    ${BISON_KeymapParser_OUTPUTS}
    ${FLEX_KeymapScanner_OUTPUTS})
target_include_directories(libkeymap PRIVATE
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap)
if(CMAKE_CROSSCOMPILING)
    add_dependencies(libkeymap libc)
endif()

if(CMAKE_CROSSCOMPILING)
    set(POSITION_INDEPENDENT_CODE FALSE)

    set(IMPORT_EXECUTABLES "IMPORTFILE-NOTFOUND" CACHE FILEPATH "Path to an exports file from a native build to be able to find build and test utilities.")
    include(${IMPORT_EXECUTABLES})

    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/keymaps)

    # Builds a keymap.
    function(pedigree_keymap name source)
        add_custom_command(OUTPUT
            ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc
            COMMAND
            $<TARGET_FILE:host-keymap> compile ${source} &&
            rm ${source}.h &&
            mv ${source}.kmc ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc
            DEPENDS ${source})
        add_custom_target(keymap-${name} ALL DEPENDS ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc)

        list(APPEND PEDIGREE_KEYMAPS ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc)
        list(APPEND PEDIGREE_KEYMAPS_TARGETS keymap-${name})
        set(PEDIGREE_KEYMAPS ${PEDIGREE_KEYMAPS} PARENT_SCOPE)
        set(PEDIGREE_KEYMAPS_TARGETS ${PEDIGREE_KEYMAPS_TARGETS} PARENT_SCOPE)
    endfunction()

    # Don't affect build utilities with kernel optimization flag choices.
    if (PEDIGREE_OPTIMIZE)
        if (PEDIGREE_OPTIMIZE_SIZE)
            set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -Os)
        else ()
            set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -O3)
        endif ()
    else ()
        set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -O0)
    endif ()

    if (PEDIGREE_ARCH_TARGET STREQUAL "arm")
        # No multiprocessor support for ARM
        set(PEDIGREE_MULTIPROCESSOR FALSE)

        # Need static drivers for ARM
        set(PEDIGREE_STATIC_DRIVERS TRUE)
    elseif (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
        set(PEDIGREE_STATIC_DRIVERS TRUE)
    endif ()

    # Copy over all our definitions.
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/config.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/config.h)

    # Include the definitions file in every translation unit
    add_definitions("--include=${CMAKE_CURRENT_BINARY_DIR}/config.h")

    # cross-compiled variant is definitely not running on a linux host
    add_definitions(-DUTILITY_LINUX=0 -DPEDIGREE_BENCHMARK=0)

    # Build config database.
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/config.db
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/create_db.py
        ${CMAKE_CURRENT_BINARY_DIR}/config.db
        ${CMAKE_SOURCE_DIR}/src/system/kernel/schema
        ${CMAKE_SOURCE_DIR}/src/modules/drivers/x86/vbe/schema
        ${CMAKE_SOURCE_DIR}/src/modules/system/splash/schema
        ${CMAKE_SOURCE_DIR}/src/modules/system/users/schema
        DEPENDS
        ${CMAKE_SOURCE_DIR}/src/system/kernel/schema
        ${CMAKE_SOURCE_DIR}/src/modules/drivers/x86/vbe/schema
        ${CMAKE_SOURCE_DIR}/src/modules/system/splash/schema
        ${CMAKE_SOURCE_DIR}/src/modules/system/users/schema)
    add_custom_target(configdb ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/config.db)

    # Embed config database in a header for static driver builds
    if (PEDIGREE_STATIC_DRIVERS)
        # generate kernel as an embedded file
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/embedconfigdb.c
            COMMAND $<TARGET_FILE:host-headerify>
            -i ${CMAKE_CURRENT_BINARY_DIR}/config.db
            -o ${CMAKE_BINARY_DIR}/embedconfigdb.c
            -v embeddedConfigDb
            DEPENDS configdb)
        add_custom_target(embedconfigdb ALL DEPENDS ${CMAKE_BINARY_DIR}/embedconfigdb.c)
    endif ()

    add_subdirectory(src/modules)
    add_subdirectory(src/system/kernel)
    add_subdirectory(src/po)

    # Build everything else with the target compiler rather than the host compiler
    if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
        set(CMAKE_C_COMPILER ${PEDIGREE_C_COMPILER})
        set(CMAKE_CXX_COMPILER ${PEDIGREE_CXX_COMPILER})
        set(CMAKE_LINKER ${PEDIGREE_CXX_COMPILER})
        set(CMAKE_SYSROOT "${CMAKE_SOURCE_DIR}/compilers/dir/x86_64-pedigree")
    endif ()

    if (PEDIGREE_BUILD_USER_DIR)
        add_subdirectory(src/user)
    endif ()

    if (PEDIGREE_ARCH_TARGET STREQUAL "arm")
        add_subdirectory(src/system/boot/arm)
    endif ()

    # Build hard disk image ready for a live CD or imaging.
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hdd.img
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/create_diskimage.py
        ${CMAKE_CURRENT_BINARY_DIR}/hdd.img  # => target image
        $<TARGET_FILE:host-ext2img>  # => ext2img binary
        ${PEDIGREE_IMAGESDIR}  # => image tree directory
        ${CMAKE_SOURCE_DIR}  # => base source directory
        ${CMAKE_SOURCE_DIR}/images/base  # => image base directory
        ${KERNEL_FILENAME}  # => kernel
        $<$<NOT:$<BOOL:${PEDIGREE_STATIC_DRIVERS}>>:${CMAKE_BINARY_DIR}/src/modules/initrd.tar>  # => initrd
        $<$<BOOL:${PEDIGREE_STATIC_DRIVERS}>:__noinitrd__>
        ${CMAKE_CURRENT_BINARY_DIR}/config.db  # => config DB
        ${CMAKE_SOURCE_DIR}/images/grub/menu-hdd.lst  # => grub list
        ${CMAKE_BINARY_DIR}/musl  # => musl build directory
        ${CMAKE_BINARY_DIR}  # => build directory
        ${PEDIGREE_KERNEL_MODULES}  # => other files to inject to image
        ${PEDIGREE_APPS}
        ${PEDIGREE_LIBS}
        ${PEDIGREE_SUBSYSTEM_LIBS}
        DEPENDS
        kernel kernelfinal
        $<$<NOT:$<BOOL:${PEDIGREE_STATIC_DRIVERS}>>:initrd>
        ${PEDIGREE_KERNEL_MODULE_TARGETS}
        configdb ${CMAKE_SOURCE_DIR}/images/grub/menu-hdd.lst
        libc ${PEDIGREE_APPS_TARGETS} ${PEDIGREE_LIBS_TARGETS}
        ${PEDIGREE_KEYMAPS_TARGETS} ${CMAKE_SOURCE_DIR}/scripts/create_diskimage.py
        )
    add_custom_target(hddimage ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/hdd.img)

    # Build ISO file for live CD.
    if (PEDIGREE_TARGET_LIVECD)
        if (GRUB)
            set(STAGE2_FILENAME "stage2_eltorito-${PEDIGREE_ARCHDIR}")
            file(COPY ${CMAKE_SOURCE_DIR}/images/grub/${STAGE2_FILENAME}
                DESTINATION ${CMAKE_BINARY_DIR})
        endif ()

        if (PEDIGREE_LIVECD)
            set(ISO_EXTRAFILES
                livedisk.img=${CMAKE_CURRENT_BINARY_DIR}/hdd.img)
            set(ISO_EXTRADEPENDS hddimage)
        else ()
            set(ISO_EXTRAFILES "")
            set(ISO_EXTRADEPENDS "")
        endif ()

        if (MKISOFS MATCHES "xorriso$")
            set(MKISOFS_EXTRA_OPTS -as mkisofs)
        else ()
            set(MKISOFS_EXTRA_OPTS "")
        endif ()

        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pedigree.iso
            COMMAND ${MKISOFS} ${MKISOFS_EXTRA_OPTS} -D -joliet -graft-points -quiet -input-charset iso8859-1
            -R -b boot/grub/stage2_eltorito -no-emul-boot -boot-load-size 4 -boot-info-table
            -o ${CMAKE_CURRENT_BINARY_DIR}/pedigree.iso -V PEDIGREE
            boot/grub/stage2_eltorito=${CMAKE_BINARY_DIR}/${STAGE2_FILENAME}
            boot/grub/menu.lst=${CMAKE_SOURCE_DIR}/images/grub/menu.lst
            boot/kernel=${CMAKE_BINARY_DIR}/src/system/kernel/kernel-mini64
            boot/initrd.tar=${CMAKE_BINARY_DIR}/src/modules/initrd.tar
            .pedigree-root=${CMAKE_CURRENT_BINARY_DIR}/config.db
            ${ISO_EXTRAFILES}
            DEPENDS kernel kernelfinal initrd configdb ${PEDIGREE_KERNEL_MODULE_TARGETS}
            ${ISO_EXTRADEPENDS})
        add_custom_target(livecd ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/pedigree.iso)
    endif ()
else ()
    set(POSITION_INDEPENDENT_CODE TRUE)

    # Override some definitions for build utilities
    set(PEDIGREE_HOSTED TRUE)
    set(PEDIGREE_LOG_TO_SERIAL FALSE)
    set(PEDIGREE_DEBUGGER FALSE)
    set(PEDIGREE_MULTIPROCESSOR FALSE)
    set(PEDIGREE_MEMORY_TRACING FALSE)
    set(PEDIGREE_SERIAL_IS_FILE FALSE)

    add_definitions(-DTHREADS=0 -DKERNEL_NEEDS_ADDRESS_SPACE_SWITCH=0)

    # Copy over all our definitions.
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/config.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/config.h)

    # Include the definitions file in every translation unit
    add_definitions(-include ${CMAKE_CURRENT_BINARY_DIR}/config.h)

    add_subdirectory(external/googletest)
    add_subdirectory(src/buildutil)
endif ()

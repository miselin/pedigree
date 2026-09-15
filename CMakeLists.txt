cmake_minimum_required(VERSION 3.21)
# The bundled GoogleTest release still declares compatibility with CMake 2.6.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Add our internal CMake modules path before project() to enable our custom platform
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/build-etc/cmake")

SET(CMAKE_BUILD_TYPE_INIT Debug)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Pedigree)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

# Select products independently of whether CMake happens to be using a
# toolchain file. A native Pedigree build is a target build even though CMake
# correctly reports that it is not cross-compiling.
set(PEDIGREE_BUILD_ROLE "AUTO" CACHE STRING
    "Products to build: AUTO, HOST_TOOLS, or TARGET.")
set_property(CACHE PEDIGREE_BUILD_ROLE PROPERTY STRINGS
    AUTO HOST_TOOLS TARGET)
string(TOUPPER "${PEDIGREE_BUILD_ROLE}" _PEDIGREE_REQUESTED_BUILD_ROLE)
if (NOT _PEDIGREE_REQUESTED_BUILD_ROLE MATCHES
    "^(AUTO|HOST_TOOLS|TARGET)$")
    message(FATAL_ERROR
        "PEDIGREE_BUILD_ROLE must be AUTO, HOST_TOOLS, or TARGET.")
endif ()

string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" _PEDIGREE_HOST_SYSTEM_NAME)
if (_PEDIGREE_REQUESTED_BUILD_ROLE STREQUAL "AUTO")
    if (_PEDIGREE_HOST_SYSTEM_NAME STREQUAL "pedigree" OR
        CMAKE_CROSSCOMPILING)
        set(PEDIGREE_EFFECTIVE_BUILD_ROLE TARGET)
    else ()
        set(PEDIGREE_EFFECTIVE_BUILD_ROLE HOST_TOOLS)
    endif ()
else ()
    set(PEDIGREE_EFFECTIVE_BUILD_ROLE
        "${_PEDIGREE_REQUESTED_BUILD_ROLE}")
endif ()

set(PEDIGREE_SELF_HOSTED FALSE)
if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "TARGET" AND
    _PEDIGREE_HOST_SYSTEM_NAME STREQUAL "pedigree")
    set(PEDIGREE_SELF_HOSTED TRUE)
endif ()

if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "TARGET" AND
    NOT PEDIGREE_ARCH_TARGET)
    message(FATAL_ERROR
        "The TARGET build role requires a Pedigree target toolchain file. "
        "On Pedigree, run easy_build_selfhost.sh.")
endif ()

message(STATUS
    "Pedigree build role: ${PEDIGREE_EFFECTIVE_BUILD_ROLE} "
    "(requested ${PEDIGREE_BUILD_ROLE}, host ${CMAKE_HOST_SYSTEM_NAME})")

check_c_compiler_flag(
    "-ftrivial-auto-var-init=pattern"
    PEDIGREE_C_HAS_TRIVIAL_AUTO_VAR_INIT_PATTERN)
check_c_compiler_flag(
    "-ftrivial-auto-var-init=zero"
    PEDIGREE_C_HAS_TRIVIAL_AUTO_VAR_INIT_ZERO)
check_cxx_compiler_flag(
    "-ftrivial-auto-var-init=pattern"
    PEDIGREE_CXX_HAS_TRIVIAL_AUTO_VAR_INIT_PATTERN)
check_cxx_compiler_flag(
    "-ftrivial-auto-var-init=zero"
    PEDIGREE_CXX_HAS_TRIVIAL_AUTO_VAR_INIT_ZERO)

if (NOT PEDIGREE_C_HAS_TRIVIAL_AUTO_VAR_INIT_PATTERN OR
    NOT PEDIGREE_C_HAS_TRIVIAL_AUTO_VAR_INIT_ZERO OR
    NOT PEDIGREE_CXX_HAS_TRIVIAL_AUTO_VAR_INIT_PATTERN OR
    NOT PEDIGREE_CXX_HAS_TRIVIAL_AUTO_VAR_INIT_ZERO)
    message(FATAL_ERROR
        "The selected C and C++ compilers must support "
        "-ftrivial-auto-var-init=pattern and =zero")
endif ()

add_compile_options(
    "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C,CXX>>:-ftrivial-auto-var-init=pattern>"
    "$<$<AND:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>,$<COMPILE_LANGUAGE:C,CXX>>:-ftrivial-auto-var-init=zero>")

if (NOT CMAKE_CXX_COMPILER_VERSION)
    # Keep this aligned with the bootstrap toolchain manifest.
    set(CMAKE_C_COMPILER_VERSION 15.3.0)
    set(CMAKE_CXX_COMPILER_VERSION 15.3.0)
endif ()

find_library(LIBRT rt)

# No rpath for anything.
set(CMAKE_SKIP_RPATH TRUE)

# Flags that affect the entire build.
set(PEDIGREE_DEBUGGER TRUE CACHE BOOL "Enable the kernel debugger.")
set(PEDIGREE_ADDITIONAL_CHECKS TRUE CACHE BOOL "Enable additional checks on some operations across the kernel.")
set(PEDIGREE_EXCESSIVE_ADDITIONAL_CHECKS FALSE CACHE BOOL "Enable even more additional checks on some operations across the kernel, many of which have a massive impact on performance.")
set(PEDIGREE_CRIPPLE_HDD TRUE CACHE BOOL "Disable writing to hard disks at runtime.")
option(PEDIGREE_AHCI_SMOKE_TESTS "Build the destructive AHCI test for a marked disposable disk." OFF)
option(PEDIGREE_NVME_SMOKE_TESTS "Build the destructive NVMe test for marked disposable disks." OFF)
option(PEDIGREE_USB_SMOKE_TESTS "Build USB input and marked disposable storage tests." OFF)
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
set(PEDIGREE_SYSCALL_COUNTER FALSE CACHE BOOL "Count POSIX syscalls per process for benchmark diagnostics.")

# init configuration
set(PEDIGREE_WITH_INIT TRUE CACHE BOOL "Build and run the init module at startup. It can be useful to skip running init to test without having the system start an entire userspace.")

# Hosted build options
# TODO: detect HOSTED=1 and cross compiling and error out!
set(PEDIGREE_HOSTED FALSE CACHE BOOL "Run a hosted kernel build, which is not compatible with a cross-compiled build.")
set(PEDIGREE_HOSTED_SYSTEM_MALLOC FALSE CACHE BOOL "Use the system malloc instead of Pedigree's in hosted builds.")
set(PEDIGREE_HOSTED_DYNAMIC_MODULES FALSE CACHE BOOL "Load modules from an initrd in hosted builds instead of linking them into the kernel.")
set(PEDIGREE_HOSTED_SMOKE_TESTS FALSE CACHE BOOL "Build the hosted smoke-test module and userspace.")
set(PEDIGREE_HOSTED_ASAN FALSE CACHE BOOL "Instrument hosted kernels and modules with AddressSanitizer.")
set(PEDIGREE_HOSTED_IRQ_CLOSURE_TESTS FALSE CACHE BOOL "Link the focused IRQ and scheduler closure regressions into a native Linux hosted kernel.")
set(PEDIGREE_CONCURRENCY_SMOKE_TESTS FALSE CACHE BOOL "Build focused kernel concurrency smoke tests for QEMU.")
set(PEDIGREE_VM_OWNERSHIP_SMOKE_TESTS FALSE CACHE BOOL "Build focused VM ownership smoke tests for QEMU with a root filesystem.")
set(PEDIGREE_VM_REMAP_TESTS FALSE CACHE BOOL "Run focused x64 remap transaction regressions during POSIX startup.")
set(PEDIGREE_MEMORY_LOCK_TESTS FALSE CACHE BOOL "Run focused memory-lock residency regressions during POSIX startup.")
set(PEDIGREE_REMAP_FILE_PAGES_TESTS FALSE CACHE BOOL "Run focused file-offset remapping regressions during POSIX startup.")
set(PEDIGREE_PROCESS_MEMORY_TESTS FALSE CACHE BOOL "Run focused process-memory copy regressions during POSIX startup.")
set(PEDIGREE_UTS_NAMESPACE_TESTS FALSE CACHE BOOL "Run focused namespace lifecycle regressions during POSIX startup.")
set(PEDIGREE_AFFINITY_TESTS FALSE CACHE BOOL "Run focused scheduler affinity and wake-publication regressions.")
set(PEDIGREE_CHILD_WAIT_TESTS FALSE CACHE BOOL "Run focused child wait and reaper lifetime regressions.")
set(PEDIGREE_PTRACE_TESTS FALSE CACHE BOOL "Run focused tracing frame and stop lifetime regressions.")
set(PEDIGREE_MODULE_SYSCALL_TESTS FALSE CACHE BOOL "Include the module syscall lifecycle fixture.")
set(PEDIGREE_RUNTIME_MODULE_TESTS FALSE CACHE BOOL "Run hosted runtime module loading regressions.")
set(PEDIGREE_GLOBAL_SYNC_TESTS FALSE CACHE BOOL "Run hosted cache and device sync regressions.")

# General build options.
set(PEDIGREE_LTO FALSE CACHE BOOL "Enable LTO.")
set(PEDIGREE_DTRELR TRUE CACHE BOOL "Pack userspace relative relocations as DT_RELR.")
set(PEDIGREE_OPTIMIZE TRUE CACHE BOOL "Build an optimized kernel and modules.")
set(PEDIGREE_OPTIMIZE_SIZE TRUE CACHE BOOL "If building an optimized kernel, optimize for size.")
set(PEDIGREE_WARNINGS FALSE CACHE BOOL "Allow compiler warnings.")

set(PEDIGREE_FLAGS
    "target=${PEDIGREE_ARCHDIR}-${PEDIGREE_MACHDIR}, build=${CMAKE_BUILD_TYPE}, debugger=${PEDIGREE_DEBUGGER}, optimize=${PEDIGREE_OPTIMIZE}, lto=${PEDIGREE_LTO}, static-drivers=${PEDIGREE_STATIC_DRIVERS}")

set(PEDIGREE_TARGET_PAGE_SIZE 4096 CACHE STRING
    "Pedigree base virtual-memory page size in bytes.")
if (NOT PEDIGREE_TARGET_PAGE_SIZE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "PEDIGREE_TARGET_PAGE_SIZE must be a positive integer.")
endif ()
math(EXPR PEDIGREE_TARGET_PAGE_MASK "${PEDIGREE_TARGET_PAGE_SIZE} - 1")
math(EXPR PEDIGREE_TARGET_PAGE_SIZE_HEX "${PEDIGREE_TARGET_PAGE_SIZE}"
    OUTPUT_FORMAT HEXADECIMAL)
math(EXPR PEDIGREE_TARGET_PAGE_REMAINDER
    "${PEDIGREE_TARGET_PAGE_SIZE} & ${PEDIGREE_TARGET_PAGE_MASK}")
if (NOT PEDIGREE_TARGET_PAGE_REMAINDER EQUAL 0)
    message(FATAL_ERROR "PEDIGREE_TARGET_PAGE_SIZE must be a power of two.")
endif ()
if (PEDIGREE_ARCH_TARGET STREQUAL "X64" AND
    NOT PEDIGREE_TARGET_PAGE_SIZE EQUAL 4096)
    message(FATAL_ERROR "The x86-64 backend requires 4096-byte base pages.")
endif ()
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND PEDIGREE_LINKERSCRIPT)
    configure_file(
        ${PEDIGREE_LINKERSCRIPT}
        ${CMAKE_CURRENT_BINARY_DIR}/hosted-kernel.ld
        @ONLY)
    set(PEDIGREE_LINKERSCRIPT ${CMAKE_CURRENT_BINARY_DIR}/hosted-kernel.ld)
endif ()

set(PEDIGREE_DTRELR_ENABLED FALSE)
if (PEDIGREE_DTRELR AND
    (PEDIGREE_ARCH_TARGET STREQUAL "X64" OR
     PEDIGREE_ARCH_TARGET STREQUAL "HOSTED"))
    set(PEDIGREE_DTRELR_ENABLED TRUE)
endif ()
set(PEDIGREE_COVERAGE FALSE CACHE BOOL "Build utilities with coverage instrumentation and reporting.")

# Build artifact controls.
set(PEDIGREE_LIVECD FALSE CACHE BOOL "Build a live CD version of the ISO image, which includes a hard disk image and relevant customizations.")
set(PEDIGREE_IMAGESDIR "${CMAKE_SOURCE_DIR}/images/local" CACHE PATH "Directory containing the directory tree to build disk images from.")
set(PEDIGREE_GO_CANARY "" CACHE FILEPATH "Prebuilt pedigree/amd64 Go canary to install as /usr/bin/go-canary.")

set(_PEDIGREE_DISTRIBUTION_ARTIFACTS_DEFAULT OFF)
if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "TARGET" AND
    NOT PEDIGREE_SELF_HOSTED)
    set(_PEDIGREE_DISTRIBUTION_ARTIFACTS_DEFAULT ON)
endif ()
option(PEDIGREE_BUILD_KEYMAPS
    "Compile keyboard layouts for distribution images."
    ${_PEDIGREE_DISTRIBUTION_ARTIFACTS_DEFAULT})
option(PEDIGREE_BUILD_TRANSLATIONS
    "Compile message catalogs for distribution images."
    ${_PEDIGREE_DISTRIBUTION_ARTIFACTS_DEFAULT})

set(_PEDIGREE_HDD_IMAGE_DEFAULT OFF)
set(_PEDIGREE_ISO_DEFAULT OFF)
option(PEDIGREE_BUILD_HDD_IMAGE
    "Build a bootable hard disk image."
    ${_PEDIGREE_HDD_IMAGE_DEFAULT})
option(PEDIGREE_BUILD_ISO
    "Build a bootable ISO image."
    ${_PEDIGREE_ISO_DEFAULT})
option(PEDIGREE_BUILD_UEFI
    "Build a FAT32 UEFI boot image and native UEFI loader."
    ON)
set(PEDIGREE_UEFI_GRUB_MKSTANDALONE "" CACHE FILEPATH
    "Path to grub-mkstandalone used to build the UEFI recovery menu.")
option(PEDIGREE_BUILD_UEFI_GRUB
    "Build a standalone UEFI GRUB recovery menu for the UEFI image."
    OFF)

if (PEDIGREE_BUILD_HDD_IMAGE OR PEDIGREE_BUILD_ISO)
    message(FATAL_ERROR
        "Legacy HDD and ISO packaging is not supported by the UEFI-only "
        "target. Build the uefi-image target instead.")
endif ()

if (PEDIGREE_SELF_HOSTED AND
    (PEDIGREE_BUILD_HDD_IMAGE OR PEDIGREE_BUILD_ISO))
    message(FATAL_ERROR
        "HDD and ISO packaging are not supported by the Pedigree-native "
        "build profile. Build boot-artifacts and deploy them separately.")
endif ()
if (PEDIGREE_SELF_HOSTED AND PEDIGREE_STATIC_DRIVERS)
    message(FATAL_ERROR
        "Static-driver builds are not supported by the Pedigree-native "
        "profile yet. Use the default dynamic modules and initrd.")
endif ()
if (PEDIGREE_SELF_HOSTED AND PEDIGREE_BUILD_KEYMAPS)
    message(FATAL_ERROR
        "Compiled distribution keymaps are not supported by the "
        "Pedigree-native profile yet. Use the checked-in keymap sources "
        "with PEDIGREE_BUILD_KEYMAPS=OFF.")
endif ()

set(_PEDIGREE_REGENERATE_KEYMAP_SOURCES_DEFAULT ON)
if (PEDIGREE_SELF_HOSTED)
    set(_PEDIGREE_REGENERATE_KEYMAP_SOURCES_DEFAULT OFF)
endif ()
option(PEDIGREE_REGENERATE_KEYMAP_SOURCES
    "Regenerate the checked-in keymap parser and lexer with Bison and Flex."
    ${_PEDIGREE_REGENERATE_KEYMAP_SOURCES_DEFAULT})

# All nasm on x86-based systems
enable_language(ASM_NASM ASM)

# Fix up nasm's output format explicitly. Hosted Darwin is a Mach-O process;
# all freestanding and Linux-hosted targets continue to use ELF objects.
if (NOT PEDIGREE_NASM_FORMAT)
    set(PEDIGREE_NASM_FORMAT elf64)
endif ()
set(PEDIGREE_NASM_FLAGS
    "-DPEDIGREE_TARGET_PAGE_SIZE=${PEDIGREE_TARGET_PAGE_SIZE}")
if (PEDIGREE_NASM_GLOBAL_PREFIX)
    string(APPEND PEDIGREE_NASM_FLAGS
        " --gprefix ${PEDIGREE_NASM_GLOBAL_PREFIX}")
endif ()
set(CMAKE_ASM_NASM_COMPILE_OBJECT
    "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> -f${PEDIGREE_NASM_FORMAT} ${PEDIGREE_NASM_FLAGS} -o <OBJECT> <SOURCE>")

cmake_host_system_information(RESULT PEDIGREE_BUILD_HOSTNAME QUERY FQDN)

if (NOT DEFINED PEDIGREE_REVISION OR NOT PEDIGREE_REVISION)
    execute_process(
        COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short=12 HEAD
        RESULT_VARIABLE PEDIGREE_GIT_RESULT
        OUTPUT_VARIABLE PEDIGREE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (PEDIGREE_GIT_RESULT OR NOT PEDIGREE_REVISION)
        set(PEDIGREE_REVISION "unknown")
    endif ()
endif ()
if (NOT DEFINED PEDIGREE_BUILDTIME OR NOT PEDIGREE_BUILDTIME)
    string(TIMESTAMP PEDIGREE_BUILDTIME "%Y-%m-%d %H:%M:%S UTC" UTC)
endif ()
if (NOT DEFINED PEDIGREE_USER OR NOT PEDIGREE_USER)
    if (DEFINED ENV{USER} AND NOT "$ENV{USER}" STREQUAL "")
        set(PEDIGREE_USER "$ENV{USER}")
    else ()
        set(PEDIGREE_USER "unknown")
    endif ()
endif ()
if (NOT PEDIGREE_BUILD_HOSTNAME)
    set(PEDIGREE_BUILD_HOSTNAME "${CMAKE_HOST_SYSTEM_NAME}")
endif ()

# Set up some general default flags for the entire project.
# NOTE: don't include optimization flags - they need to be added elsewhere based on input flags.
set(GENERIC_COMPILE_FLAGS "-fvisibility=hidden" "-pipe")
set(GENERIC_COMPILE_CFLAGS "")
set(GENERIC_COMPILE_CXXFLAGS "")
set(GENERIC_LINKFLAGS "-fvisibility=hidden")

if (NOT PEDIGREE_WARNINGS)
    set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -Werror)
endif ()

if (PEDIGREE_LTO)
    set(GENERIC_COMPILE_FLAGS ${GENERIC_COMPILE_FLAGS} -flto)
    set(GENERIC_LINKFLAGS ${GENERIC_LINKFLAGS} -flto)
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
set(GENERIC_WARNING_CXXFLAGS "-Wsign-promo" "-Woverloaded-virtual" "-Wnon-virtual-dtor" "-Wctor-dtor-privacy" "-Wabi" "-Wno-ctor-dtor-privacy" ${COMPILER_SPECIFIC_WARNINGS_CXX})
set(GENERIC_WARNING_DISABLES "-Wno-unused" "-Wno-unused-variable" "-Wno-conversion" "-Wno-format" "-Wno-error=disabled-optimization" "-Wno-error=deprecated-declarations" "-Wno-abi" "-Wno-address-of-packed-member" ${COMPILER_SPECIFIC_WARNING_DISABLES})

# Prepare flags for all builds across the entire project.
set(GENERIC_CFLAGS
    ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CFLAGS} ${GENERIC_WARNING_FLAGS}
    ${GENERIC_WARNING_CFLAGS} ${GENERIC_WARNING_DISABLES})
set(GENERIC_CXXFLAGS ${GENERIC_COMPILE_FLAGS} ${GENERIC_COMPILE_CXXFLAGS}
    ${GENERIC_WARNING_FLAGS} ${GENERIC_WARNING_CXXFLAGS}
    ${GENERIC_WARNING_DISABLES})

# Set up include directories for most targets. The generated config.h is a
# normal project header, so keep the build directory on the include path.
include_directories(${CMAKE_CURRENT_BINARY_DIR} src src/system/include)

# The Darwin ABI shim includes the generated config header. Apply it only to
# project targets, after compiler identification has finished, so a fresh
# toolchain probe does not depend on a not-yet-generated build file.
if (PEDIGREE_HOSTED_DARWIN)
    add_compile_options(-include ${PEDIGREE_HOSTED_DARWIN_ABI_HEADER})
endif ()

if (NOT PEDIGREE_SELF_HOSTED)
    include(CTest)
    enable_testing()
endif ()

set(_PEDIGREE_NEEDS_PYTHON FALSE)
if (NOT PEDIGREE_SELF_HOSTED AND
    (BUILD_TESTING OR PEDIGREE_BUILD_HDD_IMAGE))
    set(_PEDIGREE_NEEDS_PYTHON TRUE)
endif ()
if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "TARGET" AND
    PEDIGREE_ARCH_TARGET STREQUAL "X64" AND PEDIGREE_BUILD_UEFI)
    set(_PEDIGREE_NEEDS_PYTHON TRUE)
endif ()
if (_PEDIGREE_NEEDS_PYTHON)
    if (NOT Python3_EXECUTABLE AND EXISTS "${PYTHON_EXECUTABLE}")
        set(Python3_EXECUTABLE "${PYTHON_EXECUTABLE}")
    endif ()
    find_package(Python3 3 REQUIRED COMPONENTS Interpreter)
    set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
endif ()

if (NOT PEDIGREE_SELF_HOSTED AND BUILD_TESTING)
    add_test(
        NAME lifetime-escape-audit
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/audit-lifetime-escapes.py
            --root ${CMAKE_SOURCE_DIR}
            --inventory
                ${CMAKE_SOURCE_DIR}/docs/lifetime-escape-inventory.json)
    add_test(
        NAME target-page-assumption-audit
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/check-target-page-assumptions.py)
    add_test(
        NAME cmake-toolchain-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_cmake_toolchains.py -v)
    add_test(
        NAME hosted-entrypoint-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_hosted_entrypoints.py -v)
    add_test(
        NAME create-db-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_create_db.py -v)
    add_test(
        NAME create-initrd-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_create_initrd.py -v)
    add_test(
        NAME musl-syscall-routing-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_musl_syscall_routing.py -v)
    add_test(
        NAME posix-syscall-translation-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_posix_syscall_translation.py -v)
    add_test(
        NAME musl-sdk-manifest-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_musl_sdk_manifest.py -v)
    add_test(
        NAME musl-startfile-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_musl_startfiles.py -v)
    add_test(
        NAME musl-package-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_musl_package.py -v)
    add_test(
        NAME pedigree-log-boundary-tests
        COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_pedigree_log.py -v)
endif ()

# Pedigree-ABI code must compile against the selected libc SDK rather
# than whichever headers happen to be installed with the compiler or running
# system. Consumers opt into this interface so host-side generators remain on
# the build host's libc ABI.
if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "TARGET")
    set(PEDIGREE_MUSL_SDK_ROOT "${CMAKE_BINARY_DIR}/musl")
    set(PEDIGREE_MUSL_PREFIX_ROOT "${PEDIGREE_MUSL_SDK_ROOT}/usr")
    include(PedigreeMuslLink)
    pedigree_define_musl_sdk_headers()
endif ()

# These tools execute on the build machine. Native Pedigree target builds add
# them directly; ordinary host builds receive them through src/buildutil.
if (PEDIGREE_SELF_HOSTED)
    add_subdirectory(src/buildutil/configdb)
    add_subdirectory(src/buildutil/bootstrap)
endif ()

# The keymap compiler and user application share this parser. Native Pedigree
# builds consume the checked-in generated sources by default so the initial
# bootstrap does not require Bison or Flex.
set(_PEDIGREE_NEEDS_LIBKEYMAP FALSE)
if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "HOST_TOOLS" OR
    PEDIGREE_BUILD_USER_DIR)
    set(_PEDIGREE_NEEDS_LIBKEYMAP TRUE)
endif ()
if (_PEDIGREE_NEEDS_LIBKEYMAP)
    if (PEDIGREE_REGENERATE_KEYMAP_SOURCES)
        find_package(BISON REQUIRED)
        find_package(FLEX REQUIRED)
        BISON_TARGET(KeymapParser
            ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/parser.y
            ${CMAKE_BINARY_DIR}/keymap-parser.c)
        FLEX_TARGET(KeymapScanner
            ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/lexer.l
            ${CMAKE_BINARY_DIR}/keymap-lexer.c)
        ADD_FLEX_BISON_DEPENDENCY(KeymapScanner KeymapParser)
        set(_PEDIGREE_KEYMAP_PARSER_SOURCES
            ${BISON_KeymapParser_OUTPUTS}
            ${FLEX_KeymapScanner_OUTPUTS})
    else ()
        set(_PEDIGREE_KEYMAP_PARSER_SOURCES
            ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/parser.c
            ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/lexer.c)
    endif ()

    add_library(libkeymap STATIC ${_PEDIGREE_KEYMAP_PARSER_SOURCES})
    target_include_directories(libkeymap PRIVATE
        ${CMAKE_SOURCE_DIR}/src/user/applications/keymap)
    if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "TARGET")
        add_dependencies(libkeymap libc)
        if (PEDIGREE_ARCH_TARGET STREQUAL "X64")
            target_link_libraries(libkeymap PRIVATE pedigree_musl_headers)
        endif ()
    endif ()
endif ()

if (PEDIGREE_EFFECTIVE_BUILD_ROLE STREQUAL "TARGET")
    set(POSITION_INDEPENDENT_CODE FALSE)

    # Don't affect build utilities with kernel optimization flag choices.
    if (PEDIGREE_OPTIMIZE)
        if (PEDIGREE_OPTIMIZE_SIZE)
            set(_PEDIGREE_TARGET_OPTIMIZATION -Os)
        else ()
            set(_PEDIGREE_TARGET_OPTIMIZATION -O3)
        endif ()
    else ()
        set(_PEDIGREE_TARGET_OPTIMIZATION -O0)
    endif ()
    # These language lists already contain a snapshot of the common flags.
    list(APPEND GENERIC_CFLAGS ${_PEDIGREE_TARGET_OPTIMIZATION})
    list(APPEND GENERIC_CXXFLAGS ${_PEDIGREE_TARGET_OPTIMIZATION})

    if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
        set(PEDIGREE_HOSTED TRUE)
        if (PEDIGREE_HOSTED_DYNAMIC_MODULES)
            set(PEDIGREE_STATIC_DRIVERS FALSE)
        else ()
            set(PEDIGREE_STATIC_DRIVERS TRUE)
        endif ()
    endif ()

    # Host utilities execute on the build machine. Cross builds own a nested
    # native build by default; specialized multi-configuration workflows can
    # still provide a shared export explicitly.
    set(PEDIGREE_HOST_TOOLS_MODE "NESTED" CACHE STRING
        "Host utility provider for target builds: NESTED or IMPORTED.")
    set_property(CACHE PEDIGREE_HOST_TOOLS_MODE PROPERTY STRINGS
        NESTED IMPORTED)
    string(TOUPPER "${PEDIGREE_HOST_TOOLS_MODE}"
        PEDIGREE_HOST_TOOLS_MODE)
    if (NOT PEDIGREE_HOST_TOOLS_MODE MATCHES "^(NESTED|IMPORTED)$")
        message(FATAL_ERROR
            "PEDIGREE_HOST_TOOLS_MODE must be NESTED or IMPORTED.")
    endif ()

    set(_PEDIGREE_REQUIRED_HOST_UTILITIES)
    if (NOT PEDIGREE_SELF_HOSTED)
        list(APPEND _PEDIGREE_REQUIRED_HOST_UTILITIES
            host-pedigree-configdb)
    endif ()
    if (NOT PEDIGREE_SELF_HOSTED AND NOT PEDIGREE_STATIC_DRIVERS)
        list(APPEND _PEDIGREE_REQUIRED_HOST_UTILITIES
            host-pedigree-initrd-builder)
    endif ()
    if (PEDIGREE_BUILD_KEYMAPS AND PEDIGREE_BUILD_USER_DIR)
        list(APPEND _PEDIGREE_REQUIRED_HOST_UTILITIES host-keymap)
    endif ()
    if (PEDIGREE_STATIC_DRIVERS)
        list(APPEND _PEDIGREE_REQUIRED_HOST_UTILITIES host-headerify)
    endif ()
    if (PEDIGREE_BUILD_HDD_IMAGE OR PEDIGREE_BUILD_UEFI)
        list(APPEND _PEDIGREE_REQUIRED_HOST_UTILITIES host-ext2img)
    endif ()
    list(REMOVE_DUPLICATES _PEDIGREE_REQUIRED_HOST_UTILITIES)

    if (_PEDIGREE_REQUIRED_HOST_UTILITIES AND
        PEDIGREE_HOST_TOOLS_MODE STREQUAL "NESTED")
        include(PedigreeHostTools)
        pedigree_add_nested_host_tools(
            REQUIRED_TARGETS ${_PEDIGREE_REQUIRED_HOST_UTILITIES})
    elseif (_PEDIGREE_REQUIRED_HOST_UTILITIES)
        set(IMPORT_EXECUTABLES "IMPORTFILE-NOTFOUND" CACHE FILEPATH
            "Exports file from a host-tools build.")
        if (NOT EXISTS "${IMPORT_EXECUTABLES}")
            message(FATAL_ERROR
                "The selected products require host utilities "
                "(${_PEDIGREE_REQUIRED_HOST_UTILITIES}). Set "
                "IMPORT_EXECUTABLES to HostUtilities.cmake from a "
                "HOST_TOOLS build.")
        endif ()
        include("${IMPORT_EXECUTABLES}")

        foreach (_PEDIGREE_HOST_UTILITY IN LISTS
                 _PEDIGREE_REQUIRED_HOST_UTILITIES)
            if (NOT TARGET ${_PEDIGREE_HOST_UTILITY})
                message(FATAL_ERROR
                    "${IMPORT_EXECUTABLES} does not define required target "
                    "${_PEDIGREE_HOST_UTILITY}. Reconfigure and rebuild the "
                    "HOST_TOOLS tree, or use "
                    "-DPEDIGREE_HOST_TOOLS_MODE=NESTED.")
            endif ()
        endforeach ()
    endif ()

    if (PEDIGREE_BUILD_KEYMAPS)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/keymaps)

        function(pedigree_keymap name source)
            add_custom_command(OUTPUT
                ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc
                COMMAND $<TARGET_FILE:host-keymap> compile ${source}
                COMMAND ${CMAKE_COMMAND} -E rm -f ${source}.h
                COMMAND ${CMAKE_COMMAND} -E rename ${source}.kmc
                    ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc
                DEPENDS host-keymap ${source}
                VERBATIM)
            add_custom_target(keymap-${name} ALL DEPENDS
                ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc)

            list(APPEND PEDIGREE_KEYMAPS
                ${CMAKE_BINARY_DIR}/keymaps/${name}.kmc)
            list(APPEND PEDIGREE_KEYMAPS_TARGETS keymap-${name})
            set(PEDIGREE_KEYMAPS ${PEDIGREE_KEYMAPS} PARENT_SCOPE)
            set(PEDIGREE_KEYMAPS_TARGETS
                ${PEDIGREE_KEYMAPS_TARGETS} PARENT_SCOPE)
        endfunction()
    else ()
        function(pedigree_keymap name source)
        endfunction()
    endif ()

    # Copy over all our definitions.
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/config.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/config.h)

    # Target artifacts do not use the build host's utility ABI.
    add_definitions(-DUTILITY_LINUX=0 -DPEDIGREE_BENCHMARK=0)

    # Build the configuration database with an executable that matches the
    # platform on which the build itself is running.
    if (PEDIGREE_SELF_HOSTED)
        set(_PEDIGREE_CONFIG_DB_COMMAND
            $<TARGET_FILE:pedigree-configdb>
            ${CMAKE_CURRENT_BINARY_DIR}/config.db)
        set(_PEDIGREE_CONFIG_DB_DEPENDENCIES pedigree-configdb)
    elseif (TARGET host-pedigree-configdb)
        set(_PEDIGREE_CONFIG_DB_COMMAND
            $<TARGET_FILE:host-pedigree-configdb>
            ${CMAKE_CURRENT_BINARY_DIR}/config.db)
        set(_PEDIGREE_CONFIG_DB_DEPENDENCIES host-pedigree-configdb)
    else ()
        message(FATAL_ERROR
            "No configuration database generator is available.")
    endif ()
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/config.db
        COMMAND ${_PEDIGREE_CONFIG_DB_COMMAND}
        ${CMAKE_SOURCE_DIR}/src/system/kernel/schema
        ${CMAKE_SOURCE_DIR}/src/modules/drivers/x86/vbe/schema
        ${CMAKE_SOURCE_DIR}/src/modules/system/splash/schema
        DEPENDS
        ${CMAKE_SOURCE_DIR}/src/system/kernel/schema
        ${CMAKE_SOURCE_DIR}/src/modules/drivers/x86/vbe/schema
        ${CMAKE_SOURCE_DIR}/src/modules/system/splash/schema
        ${_PEDIGREE_CONFIG_DB_DEPENDENCIES}
        VERBATIM)
    add_custom_target(configdb ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/config.db)

    # Embed config database in a header for static driver builds
    if (PEDIGREE_STATIC_DRIVERS)
        # generate kernel as an embedded file
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/embedconfigdb.c
            COMMAND $<TARGET_FILE:host-headerify>
            -i ${CMAKE_CURRENT_BINARY_DIR}/config.db
            -o ${CMAKE_BINARY_DIR}/embedconfigdb.c
            -v embeddedConfigDb
            DEPENDS configdb host-headerify)
        add_custom_target(embedconfigdb ALL DEPENDS ${CMAKE_BINARY_DIR}/embedconfigdb.c)
    endif ()

    add_subdirectory(external/libvterm)
    add_subdirectory(src/modules)
    add_subdirectory(src/system/kernel)
    target_link_libraries(kernel PRIVATE pedigree-vterm-kernel)
    if (PEDIGREE_BUILD_TRANSLATIONS)
        find_package(Gettext REQUIRED)
        add_subdirectory(src/po)
    endif ()

    if (PEDIGREE_BUILD_USER_DIR)
        add_subdirectory(src/user)
    elseif (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
            PEDIGREE_HOSTED_SMOKE_TESTS)
        add_subdirectory(src/user/hosted-smoke)
    endif ()

    if (PEDIGREE_GO_CANARY)
        if (NOT PEDIGREE_ARCH_TARGET STREQUAL "X64")
            message(FATAL_ERROR "PEDIGREE_GO_CANARY is only supported for native amd64 builds.")
        endif ()
        if (NOT IS_ABSOLUTE "${PEDIGREE_GO_CANARY}")
            message(FATAL_ERROR "PEDIGREE_GO_CANARY must be an absolute path.")
        endif ()
        if (NOT EXISTS "${PEDIGREE_GO_CANARY}" OR IS_DIRECTORY "${PEDIGREE_GO_CANARY}")
            message(FATAL_ERROR "PEDIGREE_GO_CANARY does not name a file: ${PEDIGREE_GO_CANARY}")
        endif ()

        set(PEDIGREE_GO_CANARY_IMAGE "${CMAKE_BINARY_DIR}/src/user/go-canary")
        add_custom_command(OUTPUT "${PEDIGREE_GO_CANARY_IMAGE}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/src/user"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PEDIGREE_GO_CANARY}" "${PEDIGREE_GO_CANARY_IMAGE}"
            DEPENDS "${PEDIGREE_GO_CANARY}"
            VERBATIM)
        add_custom_target(go-canary-image
            DEPENDS "${PEDIGREE_GO_CANARY_IMAGE}")
        list(APPEND PEDIGREE_APPS "${PEDIGREE_GO_CANARY_IMAGE}")
        list(APPEND PEDIGREE_APPS_TARGETS go-canary-image)
    endif ()

    # This is the portable self-hosting product boundary. Packaging it into a
    # disk image, ISO, or an installed /boot tree is a separate operation.
    set(_PEDIGREE_BOOT_ARTIFACT_DEPENDENCIES
        kernel configdb ${PEDIGREE_APPS_TARGETS}
        ${PEDIGREE_LIBS_TARGETS})
    if (NOT PEDIGREE_STATIC_DRIVERS)
        list(APPEND _PEDIGREE_BOOT_ARTIFACT_DEPENDENCIES initrd)
    endif ()
    if (PEDIGREE_BUILD_USER_DIR)
        list(APPEND _PEDIGREE_BOOT_ARTIFACT_DEPENDENCIES libc)
        if (TARGET pedigree-c-sdk)
            list(APPEND _PEDIGREE_BOOT_ARTIFACT_DEPENDENCIES pedigree-c-sdk)
        endif ()
    endif ()
    list(REMOVE_DUPLICATES _PEDIGREE_BOOT_ARTIFACT_DEPENDENCIES)
    add_custom_target(boot-artifacts DEPENDS
        ${_PEDIGREE_BOOT_ARTIFACT_DEPENDENCIES})

    if (PEDIGREE_BUILD_UEFI AND PEDIGREE_ARCH_TARGET STREQUAL "X64")
        find_program(UEFI_CLANG NAMES clang REQUIRED)
        set(UEFI_LOADER_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/uefi-loader.obj)
        set(UEFI_LOADER ${CMAKE_CURRENT_BINARY_DIR}/PEDIGREE-BOOTX64.EFI)
        add_custom_command(OUTPUT ${UEFI_LOADER_OBJECT}
            COMMAND ${UEFI_CLANG} -target x86_64-pc-windows-msvc
            -ffreestanding -fno-builtin -fno-stack-protector
            -fno-asynchronous-unwind-tables -fno-unwind-tables -O2 -c
            ${CMAKE_SOURCE_DIR}/src/system/boot/uefi/loader.c
            -o ${UEFI_LOADER_OBJECT}
            DEPENDS ${CMAKE_SOURCE_DIR}/src/system/boot/uefi/loader.c
                    ${CMAKE_SOURCE_DIR}/src/system/boot/uefi/exit_boot_services.h
                    ${CMAKE_SOURCE_DIR}/src/system/boot/uefi/load_options.h
                    ${CMAKE_SOURCE_DIR}/src/system/boot/uefi/framebuffer.h)
        add_custom_command(OUTPUT ${UEFI_LOADER}
            COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/uefi_pe.py
            ${UEFI_LOADER_OBJECT} ${UEFI_LOADER}
            DEPENDS ${UEFI_LOADER_OBJECT} ${CMAKE_SOURCE_DIR}/scripts/uefi_pe.py)
        add_custom_target(uefi-loader DEPENDS ${UEFI_LOADER})

        set(UEFI_GRUB_ARGUMENT)
        set(UEFI_BOOT_DEPENDENCIES uefi-loader ${UEFI_LOADER})
        if (PEDIGREE_BUILD_UEFI_GRUB)
            if (NOT PEDIGREE_UEFI_GRUB_MKSTANDALONE)
                find_program(PEDIGREE_UEFI_GRUB_MKSTANDALONE
                    NAMES grub-mkstandalone x86_64-elf-grub-mkstandalone)
            endif ()
            if (NOT PEDIGREE_UEFI_GRUB_MKSTANDALONE)
                message(FATAL_ERROR
                    "PEDIGREE_BUILD_UEFI_GRUB requires grub-mkstandalone. "
                    "Install GRUB's UEFI tools or set "
                    "PEDIGREE_UEFI_GRUB_MKSTANDALONE to its path.")
            endif ()

            set(UEFI_GRUB ${CMAKE_CURRENT_BINARY_DIR}/GRUBX64.EFI)
            set(UEFI_GRUB_CONFIG ${CMAKE_CURRENT_BINARY_DIR}/grub.cfg)
            add_custom_command(OUTPUT ${UEFI_GRUB} ${UEFI_GRUB_CONFIG}
                COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/create_grub_uefi.py
                --grub-mkstandalone ${PEDIGREE_UEFI_GRUB_MKSTANDALONE}
                --stub-config ${CMAKE_SOURCE_DIR}/images/grub/grub-uefi-stub.cfg
                --output ${UEFI_GRUB}
                COMMAND ${CMAKE_COMMAND} -E copy
                    ${CMAKE_SOURCE_DIR}/images/grub/grub-uefi.cfg ${UEFI_GRUB_CONFIG}
                DEPENDS ${CMAKE_SOURCE_DIR}/scripts/create_grub_uefi.py
                    ${CMAKE_SOURCE_DIR}/images/grub/grub-uefi.cfg
                    ${CMAKE_SOURCE_DIR}/images/grub/grub-uefi-stub.cfg)
            add_custom_target(uefi-grub DEPENDS ${UEFI_GRUB} ${UEFI_GRUB_CONFIG})
            list(APPEND UEFI_GRUB_ARGUMENT --grub ${UEFI_GRUB})
            list(APPEND UEFI_GRUB_ARGUMENT --grub-config ${UEFI_GRUB_CONFIG})
            list(APPEND UEFI_BOOT_DEPENDENCIES uefi-grub ${UEFI_GRUB})
        endif ()

        set(UEFI_ROOT_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/pedigree-uefi-root.img)
        add_custom_command(OUTPUT ${UEFI_ROOT_IMAGE}
            COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/create_diskimage.py
            ${UEFI_ROOT_IMAGE}
            $<TARGET_FILE:host-ext2img>
            ${PEDIGREE_IMAGESDIR}
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/images/base
            ${KERNEL_FILENAME}
            $<$<NOT:$<BOOL:${PEDIGREE_STATIC_DRIVERS}>>:${CMAKE_BINARY_DIR}/src/modules/initrd.tar>
            $<$<BOOL:${PEDIGREE_STATIC_DRIVERS}>:__noinitrd__>
            ${CMAKE_CURRENT_BINARY_DIR}/config.db
            ${CMAKE_SOURCE_DIR}/images/grub/menu-hdd.lst
            ${PEDIGREE_MUSL_SDK_ROOT}
            ${PEDIGREE_C_SDK_ROOT}
            ${CMAKE_BINARY_DIR}
            ${PEDIGREE_KERNEL_MODULES}
            ${PEDIGREE_APPS}
            ${PEDIGREE_LIBS}
            ${PEDIGREE_SUBSYSTEM_LIBS}
            ${PEDIGREE_C_SDK_LIBRARY}
            DEPENDS
                host-ext2img
                kernel configdb ${PEDIGREE_APPS_TARGETS}
                ${PEDIGREE_LIBS_TARGETS}
                ${PEDIGREE_SUBSYSTEM_LIBS}
                pedigree-c-sdk
                ${CMAKE_SOURCE_DIR}/scripts/create_diskimage.py
                ${CMAKE_SOURCE_DIR}/images/grub/menu-hdd.lst)

        set(UEFI_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/pedigree-uefi.img)
        add_custom_command(OUTPUT ${UEFI_IMAGE}
            COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/create_uefi_image.py
            --image ${UEFI_IMAGE}
            --efi ${UEFI_LOADER}
            ${UEFI_GRUB_ARGUMENT}
            --kernel ${CMAKE_BINARY_DIR}/src/system/kernel/kernel
            --initrd ${CMAKE_BINARY_DIR}/src/modules/initrd.tar.uncomp
            --config ${CMAKE_CURRENT_BINARY_DIR}/config.db
            --root ${UEFI_ROOT_IMAGE}
            DEPENDS ${UEFI_BOOT_DEPENDENCIES} ${UEFI_ROOT_IMAGE}
                kernel initrd configdb
            ${CMAKE_BINARY_DIR}/src/system/kernel/kernel
            ${CMAKE_BINARY_DIR}/src/modules/initrd.tar.uncomp
            ${CMAKE_CURRENT_BINARY_DIR}/config.db
                ${CMAKE_SOURCE_DIR}/scripts/create_uefi_image.py
                ${CMAKE_SOURCE_DIR}/images/grub/grub-uefi.cfg
                ${CMAKE_SOURCE_DIR}/images/grub/grub-uefi-stub.cfg)
        add_custom_target(uefi-image ALL DEPENDS ${UEFI_IMAGE})
    endif ()

    if (PEDIGREE_BUILD_HDD_IMAGE)
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
            ${PEDIGREE_MUSL_SDK_ROOT}  # => musl SDK root
            ${PEDIGREE_C_SDK_ROOT}  # => Pedigree C SDK root
            ${CMAKE_BINARY_DIR}  # => build directory
            ${PEDIGREE_KERNEL_MODULES}  # => other files to inject to image
            ${PEDIGREE_APPS}
            ${PEDIGREE_LIBS}
            ${PEDIGREE_SUBSYSTEM_LIBS}
            DEPENDS
            kernel
            $<$<NOT:$<BOOL:${PEDIGREE_STATIC_DRIVERS}>>:initrd>
            ${KERNEL_FILENAME}
            $<$<NOT:$<BOOL:${PEDIGREE_STATIC_DRIVERS}>>:${CMAKE_BINARY_DIR}/src/modules/initrd.tar>
            ${PEDIGREE_KERNEL_MODULE_TARGETS}
            configdb ${CMAKE_CURRENT_BINARY_DIR}/config.db
            ${CMAKE_SOURCE_DIR}/images/grub/menu-hdd.lst
            ${CMAKE_SOURCE_DIR}/images/base/etc/passwd
            ${CMAKE_SOURCE_DIR}/images/base/etc/group
            ${CMAKE_SOURCE_DIR}/images/base/etc/shadow
            libc ${PEDIGREE_APPS_TARGETS} ${PEDIGREE_LIBS_TARGETS}
            pedigree-c-sdk
            ${PEDIGREE_C_SDK_HEADERS}
            ${PEDIGREE_C_SDK_LIBRARY}
            ${PEDIGREE_KEYMAPS_TARGETS} ${PEDIGREE_KEYMAPS}
            host-ext2img
            ${CMAKE_SOURCE_DIR}/scripts/create_diskimage.py)
        add_custom_target(hddimage ALL DEPENDS
            ${CMAKE_CURRENT_BINARY_DIR}/hdd.img)
    endif ()

    # Build ISO file for live CD.
    if (PEDIGREE_BUILD_ISO)
        if (NOT PEDIGREE_TARGET_LIVECD)
            message(FATAL_ERROR
                "PEDIGREE_BUILD_ISO is not supported by this target profile.")
        endif ()
        if (PEDIGREE_LIVECD AND NOT PEDIGREE_BUILD_HDD_IMAGE)
            message(FATAL_ERROR
                "PEDIGREE_LIVECD requires PEDIGREE_BUILD_HDD_IMAGE.")
        endif ()
        find_program(MKISOFS NAMES mkisofs genisoimage xorriso REQUIRED)

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
            boot/kernel=${CMAKE_BINARY_DIR}/src/system/kernel/kernel
            boot/initrd.tar=${CMAKE_BINARY_DIR}/src/modules/initrd.tar
            boot/config.db=${CMAKE_CURRENT_BINARY_DIR}/config.db
            ${ISO_EXTRAFILES}
            DEPENDS kernel initrd configdb ${PEDIGREE_KERNEL_MODULE_TARGETS}
            ${CMAKE_BINARY_DIR}/src/system/kernel/kernel
            ${CMAKE_BINARY_DIR}/src/modules/initrd.tar
            ${CMAKE_CURRENT_BINARY_DIR}/config.db
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

    # This bundled GoogleTest revision still probes with FindPythonInterp,
    # which CMake 3.27 removed. Its optional Python tests can use the
    # interpreter already selected above without invoking the legacy module.
    set(CMAKE_DISABLE_FIND_PACKAGE_PythonInterp TRUE)
    set(PYTHONINTERP_FOUND "${Python3_Interpreter_FOUND}")
    add_subdirectory(external/googletest)
    unset(CMAKE_DISABLE_FIND_PACKAGE_PythonInterp)
    unset(PYTHONINTERP_FOUND)
    add_subdirectory(src/buildutil)
endif ()

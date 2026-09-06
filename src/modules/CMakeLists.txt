include(PedigreeMuslLink)
if (PEDIGREE_ARCH_TARGET STREQUAL "X64")
    link_libraries(pedigree_musl_headers)
endif ()

set(CMAKE_C_COMPILER ${PEDIGREE_C_COMPILER})
set(CMAKE_CXX_COMPILER ${PEDIGREE_CXX_COMPILER})
if (NOT PEDIGREE_MODULE_OBJCOPY)
    set(PEDIGREE_MODULE_OBJCOPY ${CMAKE_OBJCOPY})
endif ()
if (NOT PEDIGREE_MODULE_STRIP)
    set(PEDIGREE_MODULE_STRIP ${CMAKE_STRIP})
endif ()
if (PEDIGREE_HOSTED_DARWIN)
    # CMake selected the Darwin platform for the Mach-O kernel. Its executable
    # creation flags are invalid GNU ld inputs for Pedigree ELF modules.
    set(CMAKE_EXECUTABLE_CREATE_C_FLAGS "")
    set(CMAKE_EXECUTABLE_CREATE_CXX_FLAGS "")
    set(CMAKE_C_LINK_FLAGS "")
    set(CMAKE_CXX_LINK_FLAGS "")

    if (NOT PEDIGREE_STATIC_DRIVERS)
        # Kernel assembly is Mach-O, but dynamically loaded modules remain
        # Pedigree ELF and must retain ELF symbol spelling.
        set(CMAKE_ASM_NASM_COMPILE_OBJECT
            "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> -felf64 -DPEDIGREE_TARGET_PAGE_SIZE=${PEDIGREE_TARGET_PAGE_SIZE} -o <OBJECT> <SOURCE>")
    endif ()
endif ()

# Preserve original linker flags
set(PRESERVED_SHARED_LINKER_FLAGS ${CMAKE_SHARED_LINKER_FLAGS})

set(PEDIGREE_POSIX_VERBOSE FALSE CACHE BOOL "Enable verbose logging for all POSIX syscalls.")
set(PEDIGREE_POSIX_FILE_VERBOSE FALSE CACHE BOOL "Enable verbose logging for POSIX file syscalls.")
set(PEDIGREE_POSIX_SYS_VERBOSE FALSE CACHE BOOL "Enable verbose logging for POSIX system syscalls.")
set(PEDIGREE_POSIX_THR_VERBOSE FALSE CACHE BOOL "Enable verbose logging for POSIX pthread syscalls.")
set(PEDIGREE_POSIX_NET_VERBOSE FALSE CACHE BOOL "Enable verbose logging for POSIX network syscalls.")
set(PEDIGREE_POSIX_SUBSYS_VERBOSE FALSE CACHE BOOL "Enable verbose logging for the PosixSubsystem class.")
set(PEDIGREE_POSIX_SIG_VERBOSE FALSE CACHE BOOL "Enable verbose logging for POSIX signal syscalls.")
set(PEDIGREE_POSIX_SIG_ULTRA_VERBOSE FALSE CACHE BOOL "Enable even more verbose logging for POSIX signal syscalls.")
set(PEDIGREE_POSIX_SYSCALL_VERBOSE FALSE CACHE BOOL "Enable logging of every incoming POSIX syscall (PID + syscall number).")
set(PEDIGREE_POSIX_POLL_VERBOSE FALSE CACHE BOOL "Enable logging for POSIX poll/select syscalls.")
set(PEDIGREE_POSIX_NOEFAULT FALSE CACHE BOOL "Disable checks for EFAULT conditions (unsafe - allows userspace to page fault the kernel).")

set(PEDIGREE_PCAP TRUE CACHE BOOL "Build the pcap kernel module which writes a pcap log to a serial port.")

set(MUSL_VERSION "1.2.6")
set(MUSL_NAME "musl-${MUSL_VERSION}")
set(MUSL_FILENAME "${MUSL_NAME}.tar.gz")
set(MUSL_SHA256 "d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a")
set(PEDIGREE_MUSL_PORT_REVISION 1)

add_definitions(-DIN_PEDIGREE_KERNEL)

if (NOT PEDIGREE_OVERRIDE_CXX_COMPILER_VERSION)
    set(PEDIGREE_OVERRIDE_CXX_COMPILER_VERSION ${CMAKE_CXX_COMPILER_VERSION})
endif ()

set(GENERIC_MODULE_FLAGS "-fno-common" "-ggdb" "-g3" "-gdwarf-2")
set(GENERIC_MODULE_CFLAGS "")
# NOTE: userspace code is allowed to use exceptions, so we only add -fno-exceptions
# for the module-specific build flags rather than these generic flags.
set(GENERIC_MODULE_CXXFLAGS "-fno-rtti" "-fno-asynchronous-unwind-tables")

if (PEDIGREE_ARCH_TARGET STREQUAL "X64" OR PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    set(ARCH_MODULE_FLAGS "-fno-omit-frame-pointer" "-m64" "-mno-red-zone" "-march=k8" "-mno-sse" "-mno-mmx" "-fPIC")
    set(ARCH_MODULE_LINKFLAGS "-m64")

    add_definitions(-DUSE_PIC_SYSCALLS)

    set(MUSL_LIBNAME
        ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/ld-musl-x86_64.so.1)
else ()
    message(SEND_ERROR "PEDIGREE_ARCH_TARGET is missing a target-specific flag section in the modules CMakeLists.txt")
endif ()

if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND PEDIGREE_TOOLCHAIN_ROOT)
    set(ARCH_MODULE_FLAGS ${ARCH_MODULE_FLAGS} "--sysroot=${PEDIGREE_TOOLCHAIN_ROOT}/x86_64-pedigree")
    set(ARCH_MODULE_LINKFLAGS ${ARCH_MODULE_LINKFLAGS} "--sysroot=${PEDIGREE_TOOLCHAIN_ROOT}/x86_64-pedigree")
endif ()
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    if (PEDIGREE_HOSTED_ASAN)
        list(APPEND ARCH_MODULE_FLAGS "-fsanitize=address")
        list(APPEND ARCH_MODULE_LINKFLAGS "-fsanitize=address")
    endif ()
endif ()

set(MODULE_C_FLAGS ${GENERIC_MODULE_FLAGS} ${ARCH_MODULE_FLAGS})
set(MODULE_ASM_FLAGS ${GENERIC_MODULE_FLAGS} ${ARCH_MODULE_FLAGS})
set(MODULE_CXX_FLAGS ${GENERIC_MODULE_FLAGS} ${GENERIC_MODULE_CXXFLAGS} ${ARCH_MODULE_FLAGS})

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    ${CMAKE_CURRENT_BINARY_DIR}/link.ld
    @ONLY)
set(PEDIGREE_MODULE_LINKSCRIPT "${CMAKE_CURRENT_BINARY_DIR}/link.ld")
set(PEDIGREE_MODULE_STATICLINKSCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/link_static.ld")
set(PEDIGREE_MODULE_FLAGS ${GENERIC_FLAGS} ${ARCH_MODULE_FLAGS} "-nostdlib")
set(PEDIGREE_MODULE_CFLAGS ${PEDIGREE_MODULE_FLAGS} ${GENERIC_CFLAGS})
# Modules use the kernel's nullable allocation ABI.
set(PEDIGREE_MODULE_CXXFLAGS ${PEDIGREE_MODULE_FLAGS} ${GENERIC_CXXFLAGS} "-fno-use-cxa-atexit" "-fno-exceptions" "-fcheck-new")
set(PEDIGREE_MODULE_LINKFLAGS "-nodefaultlibs" "-nostartfiles" ${ARCH_MODULE_LINKFLAGS})
if (PEDIGREE_STATIC_DRIVERS)
    set(CMAKE_SHARED_LINKER_FLAGS "-r -nostdlib")
    string(REPLACE "-shared" "" CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "${CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS}")
    string(REPLACE "-shared" "" CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS "${CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS}")

    # This linker script moves .init_array and .ctors to their own .module_ctors section, as
    # they need to be linked into the kernel and module constructors cannot run at the same
    # time as the kernel's constructors.
    set(PEDIGREE_MODULE_LINKFLAGS "${PEDIGREE_MODULE_LINKFLAGS}" "-Wl,-T,${PEDIGREE_MODULE_STATICLINKSCRIPT}")
else ()
    set(PEDIGREE_MODULE_LINKFLAGS
        "${PEDIGREE_MODULE_LINKFLAGS}"
        "-Wl,-shared"
        "-Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}"
        "-Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE}"
        "-Wl,-T,${PEDIGREE_MODULE_LINKSCRIPT}")
endif ()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/include)

# TODO: adjust so libc paths only get added for -user targets
if (PEDIGREE_SELF_HOSTED)
    link_directories(
        ${PEDIGREE_MUSL_PREFIX_ROOT}/lib
        ${PEDIGREE_NATIVE_RUNTIME_LIBRARY_DIRS}
        /usr/lib)
else ()
    link_directories(${PEDIGREE_MUSL_PREFIX_ROOT}/lib)
endif ()

set(CDI_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/include
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/include)

set(POSIX_LOGGING 0)
if (PEDIGREE_POSIX_VERBOSE)
    set(POSIX_LOGGING 0xffffffff)
else ()
    if (PEDIGREE_POSIX_FILE_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 1")
    endif ()
    if (PEDIGREE_POSIX_SYS_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 2")
    endif ()
    if (PEDIGREE_POSIX_THR_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 4")
    endif ()
    if (PEDIGREE_POSIX_NET_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 8")
    endif ()
    if (PEDIGREE_POSIX_SUBSYS_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 16")
    endif ()
    if (PEDIGREE_POSIX_SIG_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 32")
    endif ()
    if (PEDIGREE_POSIX_SIG_ULTRA_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 64")
    endif ()
    if (PEDIGREE_POSIX_SYSCALL_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 128")
    endif ()
    if (PEDIGREE_POSIX_POLL_VERBOSE)
        math(EXPR POSIX_LOGGING "${POSIX_LOGGING} + 256")
    endif ()
endif ()

add_definitions(-DPOSIX_LOG_FACILITIES=${POSIX_LOGGING})

function(pedigree_module name opts libs)
    set(PEDIGREE_MODULE_${name} TRUE CACHE BOOL "Build the ${name} kernel module.")

    # TODO: this doesn't correctly discover missing symbols if dependencies are
    # not listed in full - need to work on that.
    if (PEDIGREE_STATIC_DRIVERS)
        add_library(${name} SHARED ${ARGN})
    else ()
        add_executable(${name} ${ARGN})
        set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}.o")
    endif ()
    if (PEDIGREE_HOSTED_DARWIN)
        # The target is x86-64 Pedigree ELF, not a Darwin universal binary.
        set_target_properties(${name} PROPERTIES OSX_ARCHITECTURES "")
    endif ()
    target_compile_options(${name} PRIVATE
        ${opts}
        $<$<COMPILE_LANGUAGE:C>:${PEDIGREE_MODULE_CFLAGS}>
        $<$<COMPILE_LANGUAGE:CXX>:${PEDIGREE_MODULE_CXXFLAGS} -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables>)

    if (PEDIGREE_STATIC_DRIVERS)
        target_link_libraries(${name} PRIVATE ${PEDIGREE_MODULE_LINKFLAGS})
    elseif (PEDIGREE_HOSTED_DARWIN)
        # The Mach-O kernel cannot be an ELF link input. Modules deliberately
        # retain unresolved kernel references for the hosted ELF loader to
        # resolve through dlsym at runtime.
        target_sources(${name} PRIVATE $<TARGET_OBJECTS:module>)
        target_link_libraries(
            ${name} PRIVATE ${PEDIGREE_MODULE_LINKFLAGS} ${libs})
    else ()
        target_link_libraries(${name} PRIVATE ${PEDIGREE_MODULE_LINKFLAGS} ${libs} module kernel_shared)
    endif ()

    # Generic set of include directories for modules to find their own headers
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/system/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/${name}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/${PEDIGREE_DRIVERDIR}/${name})

    if (PEDIGREE_MODULE_${name})
        if (NOT PEDIGREE_STATIC_DRIVERS)
            list(APPEND PEDIGREE_KERNEL_MODULES $<TARGET_FILE:${name}>)
            set(PEDIGREE_KERNEL_MODULES ${PEDIGREE_KERNEL_MODULES} PARENT_SCOPE)
        endif ()

        list(APPEND PEDIGREE_KERNEL_MODULE_TARGETS ${name})

        set(PEDIGREE_KERNEL_MODULE_TARGETS ${PEDIGREE_KERNEL_MODULE_TARGETS} PARENT_SCOPE)
    endif ()

    # split debug info into a .debug file so it's accessible but not present in the module binary itself
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${PEDIGREE_MODULE_OBJCOPY} --only-keep-debug $<TARGET_FILE:${name}> ${CMAKE_CURRENT_BINARY_DIR}/${name}.debug
        COMMAND ${PEDIGREE_MODULE_STRIP} -g $<TARGET_FILE:${name}>
        COMMAND ${PEDIGREE_MODULE_OBJCOPY} --add-gnu-debuglink="${CMAKE_CURRENT_BINARY_DIR}/${name}.debug" $<TARGET_FILE:${name}>)

    if (PEDIGREE_STATIC_DRIVERS)
        set_target_properties(${name} PROPERTIES LINK_DEPENDS ${PEDIGREE_MODULE_STATICLINKSCRIPT})
    else()
        set_target_properties(${name} PROPERTIES LINK_DEPENDS ${PEDIGREE_MODULE_LINKSCRIPT})
    endif ()
endfunction()

# CDI is not part of the supported module set, but keep its independently
# buildable compatibility shims compiling so concurrency fixes cannot decay.
add_library(cdi-irq-compile-check OBJECT EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiIrq.cc)
if (PEDIGREE_HOSTED_DARWIN)
    set_target_properties(
        cdi-irq-compile-check PROPERTIES OSX_ARCHITECTURES "")
endif ()
if (PEDIGREE_ARCH_TARGET STREQUAL "X64")
    target_sources(cdi-irq-compile-check PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiCmos.cc)
endif ()
target_compile_options(cdi-irq-compile-check PRIVATE
    ${PEDIGREE_MODULE_CXXFLAGS} -fno-rtti -fno-exceptions
    -fno-asynchronous-unwind-tables)

if (NOT PEDIGREE_STATIC_DRIVERS)
    if (PEDIGREE_HOSTED_DARWIN)
        add_library(module OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/Module.cc)
    else ()
        add_library(module STATIC ${CMAKE_CURRENT_SOURCE_DIR}/Module.cc)
    endif ()
    if (PEDIGREE_HOSTED_DARWIN)
        set_target_properties(module PROPERTIES OSX_ARCHITECTURES "")
    endif ()
endif ()

if (PEDIGREE_STATIC_DRIVERS)
    set_source_files_properties(${CMAKE_BINARY_DIR}/embedconfigdb.c PROPERTIES GENERATED 1)
endif ()

pedigree_module(config "-w;-Os" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/config/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/config/Config.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/config/sqlite3/sqlite3.c
    $<$<BOOL:${PEDIGREE_STATIC_DRIVERS}>:${CMAKE_BINARY_DIR}/embedconfigdb.c>)
target_compile_definitions(config PUBLIC
    -DSQLITE_SYSTEM_MALLOC=1 -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0
    -DSQLITE_TEMP_STORE=3 -DSQLITE_DISABLE_LFS=1 -DSQLITE_OMIT_ALTER_TABLE=1
    -DSQLITE_OMIT_AUTHORIZATION=1 -DSQLITE_OMIT_AUTOINIT=1
    -DSQLITE_OMIT_AUTOVACUUM=1 -DSQLITE_OMIT_BUILTIN_TEST=1
    -DSQLITE_OMIT_COMPILEOPTION_DIAGS=1 -DSQLITE_OMIT_COMPLETE=1
    -DSQLITE_OMIT_DECLTYPE=1 -DSQLITE_OMIT_DEPRECATED=1 -DSQLITE_OMIT_EXPLAIN=1
    -DSQLITE_OMIT_FLAG_PRAGMAS=1 -DSQLITE_OMIT_FLOATING_POINT=1
    -DSQLITE_OMIT_INCRBLOB=1 -DSQLITE_OMIT_INTEGRITY_CHECK=1
    -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_LOCALTIME=1
    -DSQLITE_OMIT_LOOKASIDE=1 -DSQLITE_OMIT_MEMORYDB=1
    -DSQLITE_OMIT_PROGRESS_CALLBACK=1 -DSQLITE_OMIT_SCHEMA_PRAGMAS=1
    -DSQLITE_OMIT_SCHEMA_VERSION_PRAGMAS=1 -DSQLITE_OMIT_TCL_VARIABLE=1
    -DSQLITE_OMIT_TEMPDB=1 -DSQLITE_OMIT_TRACE=1 -DSQLITE_OMIT_UTF16=1
    -DSQLITE_OMIT_WAL=1)
target_compile_options(config PRIVATE -Wno-implicit-fallthrough)
add_dependencies(config libc)  # TODO: this shouldn't need headers.
if (PEDIGREE_STATIC_DRIVERS)
    add_dependencies(config embedconfigdb)
endif ()

pedigree_module(confignics "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/confignics/main.cc)
add_dependencies(confignics lwip)

pedigree_module(console "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/console/Console.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/console/ConsoleCommon.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/console/ConsoleMaster.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/console/ConsolePhysical.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/console/ConsoleSlave.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/console/TextIO.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/console/defaultCharacters.c)

pedigree_module(ext2 "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2Directory.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2File.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2Filesystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2Node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2Resize.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2Rename.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2Symlink.cc)

set(FAT_REGRESSION_SOURCES)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    list(APPEND FAT_REGRESSION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/fat-sector-regressions.cc)
endif ()

pedigree_module(fat "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatDirectory.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatFile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatFilesystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatSymlink.cc
    ${FAT_REGRESSION_SOURCES})

pedigree_module(filesystems "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/filesystems/main.cc)

pedigree_module(gfx-deps "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/gfx-deps/main.cc)

if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    if (PEDIGREE_HOSTED_DARWIN)
        pedigree_module(hosted-core-smoke "" ""
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/core-main.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/elf-validation-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/ata-pio-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/buffer-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/lifetime-lease-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/mutex-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/pagefault-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/payload-page-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/primitive-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/process-exit-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/requestqueue-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/signal-interruption-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/storage-page-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/timer-regressions.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/wait-regressions.cc)
        target_compile_definitions(
            hosted-core-smoke PRIVATE PEDIGREE_HOSTED_CORE_SMOKE=1)
    else ()
    pedigree_module(hosted-smoke "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/main.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/elf-validation-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/ata-pio-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/buffer-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/cache-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/input-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/interrupt-manager-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/lifetime-lease-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/log-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/mutex-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/network-filter-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/pagefault-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/payload-page-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/pipe-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/primitive-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/process-exit-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/ps2-controller-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/ps2mouse-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/requestqueue-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/scheduler-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/signal-interruption-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/timer-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/usb-callback-delivery-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/usb-hcd-port-change-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/usb-transfer-lifecycle-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/vfs-callback-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/wait-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiIrq.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ps2mouse/Ps2MouseCallbackRegistry.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/Filter.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/UnixFilesystem.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/hosted-unix-regressions.cc)
    endif ()
endif ()

if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    NOT PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    pedigree_module(hosted-syscall-smoke "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/access-syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/advisory-lock-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/child-resource-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/clone-routing-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/dup3-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/eventfd-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/file-contract-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/futex-robust-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/system-usercopy-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/vm-permission-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/exit-status-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/inotify-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/mmap-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/ppoll-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/positional-io-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/positional-vector-io-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/pselect-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/process-query-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/resource-syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/rt-sigsuspend-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/scalar-io-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/scm-rights-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/scm-stream-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/sleep-clock-syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/static-syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/thread-signal-syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/termios-syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/time-syscall-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/unix-stream-interruption-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/usercopy-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/vector-io-regressions.cc)
endif ()

if (NOT PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_CONCURRENCY_SMOKE_TESTS)
    pedigree_module(concurrency-smoke "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/system/concurrency-smoke/main.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/concurrency-smoke/memory-region-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/concurrency-smoke/slam-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/concurrency-smoke/textio-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/concurrency-smoke/tlb-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/network-filter-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/vfs-callback-regressions.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/Filter.cc)
    # Keep the test fixture from exporting the live network module's singleton.
    target_compile_definitions(
        concurrency-smoke PRIVATE NetworkFilter=ConcurrencySmokeNetworkFilter)
endif ()

if (PEDIGREE_WITH_INIT)
pedigree_module(init "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/init/main.cc)
endif ()

pedigree_module(iso9660 "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/iso9660/Iso9660Directory.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/iso9660/Iso9660File.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/iso9660/Iso9660Filesystem.cc)

set (LINKER_ASM "${CMAKE_CURRENT_SOURCE_DIR}/system/linker/${PEDIGREE_LINKERDIR}/asm-${PEDIGREE_LINKERDIR}.s")
if (NOT EXISTS ${LINKER_ASM})
    set (LINKER_ASM "")
endif ()

set(LINKER_REGRESSION_SOURCES)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_SMOKE_TESTS)
    list(APPEND LINKER_REGRESSION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/page-content-regressions.cc)
endif ()

pedigree_module(linker "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/DynamicLinker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/${PEDIGREE_LINKERDIR}/DynamicLinker.cc
    ${LINKER_ASM}
    ${LINKER_REGRESSION_SOURCES})

if (LINKER_REGRESSION_SOURCES)
    target_compile_definitions(
        linker PRIVATE PEDIGREE_HOSTED_PAGE_CONTENT_REGRESSIONS=1)
endif ()

if (PEDIGREE_ARCH_TARGET STREQUAL "X64" OR PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/${PEDIGREE_LINKERDIR}/asm-${PEDIGREE_LINKERDIR}.s
        PROPERTIES LANGUAGE ASM_NASM)
endif ()

pedigree_module(lodisk "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lodisk/LoDisk.cc)

pedigree_module(lwip "-w" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/lwip.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/sys_arch.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/api_lib.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/api_msg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/err.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/netbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/netdb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/netifapi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/sockets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/api/tcpip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/autoip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/dhcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/etharp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/icmp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/igmp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/ip4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/ip4_addr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv4/ip4_frag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/dhcp6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/ethip6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/icmp6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/inet6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/ip6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/ip6_addr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/ip6_frag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/mld6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ipv6/nd6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/def.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/dns.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/inet_chksum.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/ip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/memp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/netif.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/pbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/raw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/sys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/tcp_in.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/tcp_out.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/timeouts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/core/udp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/netif/ethernet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/netif/ethernetif.c)
# lwip doesn't seem to offer an easy way to handle visibility, so we just use
# default visibility rather than hiding everything by default.
target_compile_options(lwip PRIVATE -fvisibility=default)
add_dependencies(lwip libc)  # TODO: modify arch.h to remove header dependency

pedigree_module(mountroot "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/mountroot/main.cc)

set(NETWORK_STACK_REGRESSION_SOURCES)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    NOT PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    list(APPEND NETWORK_STACK_REGRESSION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/network-stack-regressions.cc)
endif ()

pedigree_module(network-stack "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/Filter.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/NetworkStack.cc
    ${NETWORK_STACK_REGRESSION_SOURCES})
add_dependencies(network-stack lwip)
if (NETWORK_STACK_REGRESSION_SOURCES)
    target_compile_definitions(
        network-stack PRIVATE PEDIGREE_HOSTED_NETWORK_REGRESSION=1)
endif ()

pedigree_module(nics "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/nics/main.cc)

if (PEDIGREE_PCAP)
pedigree_module(pcap "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/pcap/main.cc)
endif ()

pedigree_module(preload "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/preload/main.cc)

pedigree_module(ramfs "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ramfs/RamFs.cc)

set(RAWFS_REGRESSION_SOURCES)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    list(APPEND RAWFS_REGRESSION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/rawfs-contract-regressions.cc)
endif ()

pedigree_module(rawfs "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/rawfs/RawFs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/rawfs/RawFsDir.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/rawfs/RawFsFile.cc
    ${RAWFS_REGRESSION_SOURCES})

pedigree_module(splash "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/splash/main.cc)

pedigree_module(status_server "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/status_server/main.cc)
add_dependencies(status_server lwip)

set(USB_REGRESSION_SOURCES)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    list(APPEND USB_REGRESSION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/usb-endpoint-halt-regressions.cc)
endif ()

pedigree_module(usb "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/UsbDevice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/UsbHub.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/UsbPnP.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/main.cc
    ${USB_REGRESSION_SOURCES})

pedigree_module(users "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/users/Group.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/users/User.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/users/UserManager.cc)

pedigree_module(vfs "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Directory.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/File.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/FileEvent.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Filesystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/LockedFile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/MemoryMappedFile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/MemoryMappedFile-remap.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/MemoryMappedFile-advice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Pipe.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Symlink.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/VFS.cc)

if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS AND
    NOT PEDIGREE_HOSTED_DARWIN)
    add_dependencies(hosted-smoke config users vfs fat rawfs usb)
endif ()

pedigree_module(3c90x "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/3c90x/3Com90x.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/3c90x/main.cc)

if (PEDIGREE_ARCH_TARGET STREQUAL "X64")
    pedigree_module(ata "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/AtaController.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/AtaDisk.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/BusMasterIde.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/IsaAtaController.cc
        $<$<BOOL:${PEDIGREE_MACHINE_HASPCI}>:${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/PciAtaController.cc>
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/main.cc)
endif ()

pedigree_module(dm9601 "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/dm9601/Dm9601.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/dm9601/main.cc)

if (PEDIGREE_MACHDIR STREQUAL "mach_pc")
    set(DMA_EXTRA_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/dma/x86/X86IsaDma.cc)
else ()
    set(DMA_EXTRA_SRCS)
endif ()

pedigree_module(dma "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/dma/IsaDma.cc
    ${DMA_EXTRA_SRCS})

pedigree_module(ftdi "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ftdi/FtdiSerialDevice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ftdi/main.cc)

pedigree_module(hid "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/hid/HidReport.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/hid/HidUtils.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/hid/main.cc)

if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    pedigree_module(diskimage "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/hosted/diskimage/DiskImage.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/hosted/diskimage/main.cc)
endif ()

pedigree_module(partition "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/Partition.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/apple.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/msdos.cc)

set(SCSI_REGRESSION_SOURCES)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    list(APPEND SCSI_REGRESSION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/scsi-write-regressions.cc)
endif ()

pedigree_module(scsi "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/ScsiCommands.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/ScsiController.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/ScsiDisk.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/main.cc
    ${SCSI_REGRESSION_SOURCES})

pedigree_module(usb-hcd "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hcd/Ehci.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hcd/Ohci.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hcd/Uhci.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hcd/main.cc)

pedigree_module(usb-hid "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hid/UsbHumanInterfaceDevice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hid/main.cc)

pedigree_module(usb-hub "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hub/UsbHubDevice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-hub/main.cc)

set(USB_MASS_STORAGE_REGRESSION_SOURCES)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS)
    list(APPEND USB_MASS_STORAGE_REGRESSION_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/usb-bot-regressions.cc)
endif ()

pedigree_module(usb-mass-storage "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-mass-storage/UsbMassStorageDevice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-mass-storage/main.cc
    ${USB_MASS_STORAGE_REGRESSION_SOURCES})

if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_HOSTED_DYNAMIC_MODULES AND PEDIGREE_HOSTED_SMOKE_TESTS AND
    NOT PEDIGREE_HOSTED_DARWIN)
    add_dependencies(hosted-smoke scsi usb-mass-storage)
endif ()

if (PEDIGREE_DRIVERDIR STREQUAL "x86")
    # pedigree_module(cdi "" ""
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiDisk.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiIrq.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiNet.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiCmos.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/_list.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/cdi.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/compat.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/dma.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/lists.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/misc.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/net.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/pci.cc
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/storage.cc)
    # target_include_directories(cdi PRIVATE ${CDI_INCLUDE_DIRS})

    # pedigree_module(e1000 "-w" ""
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/e1000/device.c
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/e1000/main.c)
    # target_include_directories(e1000 PRIVATE ${CDI_INCLUDE_DIRS})

    # pedigree_module(pcnet "-w" ""
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/pcnet/pcnet.c
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/pcnet/main.c)
    # target_include_directories(pcnet PRIVATE ${CDI_INCLUDE_DIRS})

    # pedigree_module(rtl8139 "-w" ""
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/rtl8139/rtl8139.c
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/rtl8139/main.c)
    # target_include_directories(rtl8139 PRIVATE ${CDI_INCLUDE_DIRS})

    # pedigree_module(sis900 "-w" ""
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/device.c
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/eeprom.c
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/main.c
    #     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/sis900_io.c)
    # target_include_directories(sis900 PRIVATE ${CDI_INCLUDE_DIRS})

    pedigree_module(ib700_wdt "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ib700_wdt/main.cc)

    pedigree_module(ne2k "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ne2k/Ne2k.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ne2k/main.cc)

    pedigree_module(pci "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/pci/pci.cc)

    pedigree_module(ps2mouse "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ps2mouse/Ps2MouseCallbackRegistry.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ps2mouse/Ps2Mouse.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ps2mouse/main.cc)

    pedigree_module(vbe "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/vbe/VbeDisplay.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/vbe/vbe.cc)

    pedigree_module(vmware-gfx "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/vmware-gfx/main.cc)
endif ()

add_library(vdso SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/glue-infoblock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vdso.ld)
if (PEDIGREE_ARCH_TARGET STREQUAL "X64" OR PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    target_compile_options(vdso PRIVATE "-mcmodel=small")
endif ()
target_link_libraries(vdso PRIVATE gcc
    "-Wl,--soname,linux-vdso.so.1" "-Wl,--hash-style=both" "-Wl,-Bsymbolic"
    "-Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}"
    "-Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE}"
    "-nostdlib"
    "-T${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vdso.ld"
    "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/glue-infoblock.ld")
add_dependencies(vdso libc)

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vdso.h
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=$<TARGET_FILE:vdso>"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/vdso.h"
        "-DPAGE_SIZE=${PEDIGREE_TARGET_PAGE_SIZE}"
        -P ${CMAKE_SOURCE_DIR}/build-etc/cmake/GenerateVdsoHeader.cmake
    DEPENDS vdso
        ${CMAKE_SOURCE_DIR}/build-etc/cmake/GenerateVdsoHeader.cmake
    VERBATIM)
add_custom_target(vdso-header DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vdso.h)

pedigree_module(posix "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/console-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/DevFs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/epoll-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/eventfd-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/FileDescriptor.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/file-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/IoEvent.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/inotify-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/sysv-semaphore-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/sysv-shm-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/sysv-message-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/mqueue-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/mqueue-state.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/mqueue-netlink.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/linux-amd64-signal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/net-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/pipe-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PollEvent.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/poll-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/posix.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PosixProcess.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PosixSubsystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PosixSyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/ProcFs.cc
    $<$<BOOL:${PEDIGREE_MACHINE_HASPS2}>:${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PsAuxFile.cc>
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/pthread-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/select-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/clock-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/queued-signal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/pending-signal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/signalfd-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/timerfd-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vm-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/posix-timer-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/signal-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/system-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/UnixFilesystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/util.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/VirtualTerminal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vsyscall-${PEDIGREE_ASMDIR}.s
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/sigret-${PEDIGREE_ASMDIR}.s
    ${CMAKE_CURRENT_BINARY_DIR}/vdso.h)
if (NOT PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" AND
    PEDIGREE_VM_OWNERSHIP_SMOKE_TESTS)
    target_sources(posix PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/system/hosted-smoke/vm-permission-regressions.cc)
endif ()
target_include_directories(posix PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/include
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/syscalls)
target_compile_definitions(posix PRIVATE
    -D_POSIX_C_SOURCE=200112L -D_XOPEN_SOURCE=600)

if (PEDIGREE_ARCH_TARGET STREQUAL "X64" OR PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vsyscall-${PEDIGREE_ASMDIR}.s
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/sigret-${PEDIGREE_ASMDIR}.s
        PROPERTIES LANGUAGE ASM_NASM)
endif ()

pedigree_module(native "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/kernel/NativeIpc.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/kernel/NativeSyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/kernel/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/kernel/native-base.cc)
target_include_directories(native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/include
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/kernel/include)
add_dependencies(native libc)

if (NOT PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    add_library(native-user SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/Object.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/cppsupport.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/demo.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/native-protocol.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/config/Config.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/graphics/Graphics.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/input/Input.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/user/ipc/Ipc.cc)
    target_include_directories(native-user PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/include)
    target_link_libraries(native-user PRIVATE -nostdlib gcc)
    target_link_options(native-user PRIVATE
        -Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}
        -Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE})
    pedigree_use_musl_sdk_startfiles(native-user)
    if (PEDIGREE_DTRELR_ENABLED)
        target_link_options(native-user PRIVATE -Wl,-z,pack-relative-relocs)
    endif ()
    set_target_properties(native-user PROPERTIES OUTPUT_NAME "pedigree")
    add_dependencies(native-user libc)
endif ()

pedigree_module(pedigree-c "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/PedigreeCSyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-c.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-syscalls.cc)
target_include_directories(native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c)

set(PEDIGREE_C_USER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-c-syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-log.c)
if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    add_library(pedigree-c-user OBJECT ${PEDIGREE_C_USER_SOURCES})
else ()
    add_library(pedigree-c-user SHARED ${PEDIGREE_C_USER_SOURCES})
    set_target_properties(pedigree-c-user PROPERTIES OUTPUT_NAME "pedigree-c")
endif ()
if (PEDIGREE_HOSTED_DARWIN)
    set_target_properties(pedigree-c-user PROPERTIES OSX_ARCHITECTURES "")
endif ()
target_include_directories(pedigree-c-user
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/include)
if (NOT PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    target_link_options(pedigree-c-user PRIVATE
        -Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}
        -Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE})
    pedigree_use_musl_sdk_startfiles(pedigree-c-user)
    if (PEDIGREE_DTRELR_ENABLED)
        target_link_options(pedigree-c-user PRIVATE -Wl,-z,pack-relative-relocs)
    endif ()
endif ()
add_dependencies(pedigree-c-user libc)

if (PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    set(PEDIGREE_SUBSYSTEM_LIBS PARENT_SCOPE)
else ()
    set(PEDIGREE_C_SDK_ROOT "${CMAKE_BINARY_DIR}/pedigree-c-sdk")
    set(PEDIGREE_C_SDK_HEADERS
        "${PEDIGREE_C_SDK_ROOT}/usr/include/pedigree/fb.h"
        "${PEDIGREE_C_SDK_ROOT}/usr/include/pedigree/log.h")
    set(PEDIGREE_C_SDK_LIBRARY
        "${PEDIGREE_C_SDK_ROOT}/usr/lib/libpedigree-c.so")
    add_custom_target(pedigree-c-sdk
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${PEDIGREE_C_SDK_ROOT}/usr/include/pedigree"
            "${PEDIGREE_C_SDK_ROOT}/usr/lib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/include/pedigree/fb.h"
            "${PEDIGREE_C_SDK_ROOT}/usr/include/pedigree/fb.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/include/pedigree/log.h"
            "${PEDIGREE_C_SDK_ROOT}/usr/include/pedigree/log.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:pedigree-c-user>
            "${PEDIGREE_C_SDK_LIBRARY}"
        DEPENDS
            pedigree-c-user
            ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/include/pedigree/fb.h
            ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/include/pedigree/log.h
        BYPRODUCTS
            ${PEDIGREE_C_SDK_HEADERS}
            "${PEDIGREE_C_SDK_LIBRARY}"
        VERBATIM)
    set(PEDIGREE_C_SDK_ROOT "${PEDIGREE_C_SDK_ROOT}" PARENT_SCOPE)
    set(PEDIGREE_C_SDK_HEADERS "${PEDIGREE_C_SDK_HEADERS}" PARENT_SCOPE)
    set(PEDIGREE_C_SDK_LIBRARY "${PEDIGREE_C_SDK_LIBRARY}" PARENT_SCOPE)
    set(PEDIGREE_SUBSYSTEM_LIBS $<TARGET_FILE:native-user> PARENT_SCOPE)
endif ()

if (PEDIGREE_STATIC_DRIVERS)
    add_library(staticmodules OBJECT Module.cc)
else ()
    set(PEDIGREE_INITRD_ENTRIES)
    foreach (module_target IN LISTS PEDIGREE_KERNEL_MODULE_TARGETS)
        list(APPEND PEDIGREE_INITRD_ENTRIES
            "$<TARGET_FILE_NAME:${module_target}>=$<TARGET_FILE:${module_target}>")
    endforeach ()

    if (PEDIGREE_SELF_HOSTED)
        set(_PEDIGREE_INITRD_COMMAND
            $<TARGET_FILE:pedigree-initrd-builder>)
        set(_PEDIGREE_INITRD_DEPENDENCIES pedigree-initrd-builder)
    elseif (TARGET host-pedigree-initrd-builder)
        set(_PEDIGREE_INITRD_COMMAND
            $<TARGET_FILE:host-pedigree-initrd-builder>)
        set(_PEDIGREE_INITRD_DEPENDENCIES
            host-pedigree-initrd-builder)
    else ()
        message(FATAL_ERROR "No initrd generator is available.")
    endif ()

    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar
            ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar.uncomp
            ${CMAKE_CURRENT_BINARY_DIR}/initrd.manifest
        COMMAND ${_PEDIGREE_INITRD_COMMAND}
            --output ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar
            --uncompressed ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar.uncomp
            --manifest ${CMAKE_CURRENT_BINARY_DIR}/initrd.manifest
            ${PEDIGREE_INITRD_ENTRIES}
        DEPENDS
            ${PEDIGREE_KERNEL_MODULE_TARGETS}
            ${_PEDIGREE_INITRD_DEPENDENCIES}
        VERBATIM)
    add_custom_target(initrd ALL DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar
        ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar.uncomp
        ${CMAKE_CURRENT_BINARY_DIR}/initrd.manifest)
endif ()

set(PEDIGREE_MUSL_ARCHIVE
    "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_FILENAME}" CACHE FILEPATH
    "Local musl source archive used to build Pedigree's libc.")
get_filename_component(MUSL_ARCHIVE "${PEDIGREE_MUSL_ARCHIVE}" ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}")
set(PEDIGREE_MUSL_CONFIG_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/musl-config")
file(MAKE_DIRECTORY "${PEDIGREE_MUSL_CONFIG_DIR}")
if (PEDIGREE_MUSL_ARCH_TARGET STREQUAL "HOSTED")
    set(PEDIGREE_MUSL_CONFIG_HOSTED 1)
    set(PEDIGREE_MUSL_CONFIG_X64 0)
else ()
    set(PEDIGREE_MUSL_CONFIG_HOSTED 0)
    set(PEDIGREE_MUSL_CONFIG_X64 1)
endif ()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/config.h.in
    ${PEDIGREE_MUSL_CONFIG_DIR}/config.h)
set(PEDIGREE_MUSL_PORT_INPUTS
    ${CMAKE_SOURCE_DIR}/build-etc/cmake/GenerateMuslSdkManifest.cmake
    ${CMAKE_SOURCE_DIR}/build-etc/cmake/PedigreeMuslSdkManifest.cmake
    ${CMAKE_SOURCE_DIR}/build-etc/cmake/ValidateMuslSdkManifest.cmake
    ${CMAKE_SOURCE_DIR}/build-etc/toolchain/musl-sdk-manifest-v1.schema.json
    ${CMAKE_SOURCE_DIR}/build-etc/toolchain/musl-1.2.6-cve-2026-40200-qsort.patch
    ${CMAKE_SOURCE_DIR}/build-etc/toolchain/musl-1.2.6-cve-2026-6042-iconv.patch
    ${CMAKE_SOURCE_DIR}/scripts/build-musl-${PEDIGREE_MUSLARCH}.sh)
if (PEDIGREE_MUSL_ARCH_TARGET STREQUAL "HOSTED")
    list(APPEND PEDIGREE_MUSL_PORT_INPUTS
        ${PEDIGREE_MUSL_CONFIG_DIR}/config.h
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/clone-hosted-amd64.musl-s
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/glue-musl.c
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/syscall_arch.h
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/syscall_cp-${PEDIGREE_MUSLARCH}.musl-s
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/syscalls/posix-syscall.h
        ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/syscalls/posixSyscallNumbers.h
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/processor/Syscalls.h
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/processor/syscall-stubs.h
        ${CMAKE_SOURCE_DIR}/src/system/include/pedigree/kernel/processor/hosted/syscall-stubs.h)
endif ()

set(_PEDIGREE_MUSL_PORT_HASH_INPUT "")
foreach (_PEDIGREE_MUSL_PORT_INPUT IN LISTS PEDIGREE_MUSL_PORT_INPUTS)
    file(SHA256 "${_PEDIGREE_MUSL_PORT_INPUT}"
        _PEDIGREE_MUSL_PORT_INPUT_HASH)
    string(APPEND _PEDIGREE_MUSL_PORT_HASH_INPUT
        "${_PEDIGREE_MUSL_PORT_INPUT_HASH}\n")
endforeach ()
string(SHA256 PEDIGREE_MUSL_REVISION
    "${_PEDIGREE_MUSL_PORT_HASH_INPUT}")

execute_process(
    COMMAND "${PEDIGREE_C_COMPILER}" -dumpmachine
    RESULT_VARIABLE _PEDIGREE_MUSL_COMPILER_TARGET_RESULT
    OUTPUT_VARIABLE PEDIGREE_MUSL_COMPILER_TARGET
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND "${PEDIGREE_C_COMPILER}" -dumpfullversion -dumpversion
    RESULT_VARIABLE _PEDIGREE_MUSL_COMPILER_VERSION_RESULT
    OUTPUT_VARIABLE PEDIGREE_MUSL_COMPILER_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if (NOT _PEDIGREE_MUSL_COMPILER_TARGET_RESULT EQUAL 0 OR
    NOT _PEDIGREE_MUSL_COMPILER_VERSION_RESULT EQUAL 0 OR
    NOT PEDIGREE_MUSL_COMPILER_TARGET OR NOT PEDIGREE_MUSL_COMPILER_VERSION)
    message(FATAL_ERROR
        "Could not identify the compiler used to build the musl SDK: "
        "${PEDIGREE_C_COMPILER}")
endif ()

if (PEDIGREE_MUSL_ARCH_TARGET STREQUAL "HOSTED")
    set(PEDIGREE_MUSL_PROFILE hosted)
else ()
    set(PEDIGREE_MUSL_PROFILE target)
endif ()
set(PEDIGREE_MUSL_ABI pedigree-linux-amd64-v1)
string(CONCAT _PEDIGREE_MUSL_BUILD_ID_INPUT
    "${MUSL_VERSION}\n${MUSL_SHA256}\n${PEDIGREE_MUSL_PORT_REVISION}\n"
    "${PEDIGREE_MUSL_REVISION}\n${PEDIGREE_MUSL_ARCH_TARGET}\n"
    "${PEDIGREE_MUSL_ABI}\n${PEDIGREE_TARGET_PAGE_SIZE}\n"
    "${PEDIGREE_DTRELR_ENABLED}\n${PEDIGREE_MUSL_COMPILER_TARGET}\n"
    "${PEDIGREE_MUSL_COMPILER_VERSION}\n")
string(SHA256 PEDIGREE_MUSL_BUILD_ID
    "${_PEDIGREE_MUSL_BUILD_ID_INPUT}")
set(PEDIGREE_MUSL_MANIFEST
    ${PEDIGREE_MUSL_SDK_ROOT}/usr/share/pedigree/libc/manifest.json)

set(MUSL_DOWNLOAD "${MUSL_ARCHIVE}.download")
set(MUSL_DTRELR_STATE "${CMAKE_CURRENT_BINARY_DIR}/musl-dtrelr.state")
set(MUSL_PAGE_SIZE_STATE "${CMAKE_CURRENT_BINARY_DIR}/musl-page-size.state")
set(MUSL_PORT_STATE "${CMAKE_CURRENT_BINARY_DIR}/musl-port.state")
set(MUSL_DERIVATION_STATE
    "${CMAKE_CURRENT_BINARY_DIR}/musl-derivation.state")
file(GENERATE OUTPUT "${MUSL_DTRELR_STATE}"
    CONTENT "PEDIGREE_DTRELR=$<BOOL:${PEDIGREE_DTRELR_ENABLED}>\n")
file(GENERATE OUTPUT "${MUSL_PAGE_SIZE_STATE}"
    CONTENT "PEDIGREE_TARGET_PAGE_SIZE=${PEDIGREE_TARGET_PAGE_SIZE}\n")
file(GENERATE OUTPUT "${MUSL_PORT_STATE}"
    CONTENT "PEDIGREE_MUSL_PORT_REVISION=${PEDIGREE_MUSL_PORT_REVISION}\n")
file(GENERATE OUTPUT "${MUSL_DERIVATION_STATE}"
    CONTENT "PEDIGREE_MUSL_BUILD_ID=${PEDIGREE_MUSL_BUILD_ID}\n")

if (EXISTS "${MUSL_ARCHIVE}")
    file(SHA256 "${MUSL_ARCHIVE}" MUSL_ACTUAL_SHA256)
    if (NOT "${MUSL_ACTUAL_SHA256}" STREQUAL "${MUSL_SHA256}")
        message(FATAL_ERROR
            "Cached musl archive has SHA256 ${MUSL_ACTUAL_SHA256}; expected ${MUSL_SHA256}: ${MUSL_ARCHIVE}")
    endif ()
else ()
    if (PEDIGREE_SELF_HOSTED)
        message(FATAL_ERROR
            "Self-hosted builds do not download dependencies during configure. "
            "Provide the verified musl archive with "
            "-DPEDIGREE_MUSL_ARCHIVE=/path/to/${MUSL_FILENAME}")
    endif ()

    file(REMOVE "${MUSL_DOWNLOAD}")
    file(DOWNLOAD
        "https://www.musl-libc.org/releases/${MUSL_FILENAME}"
        "${MUSL_DOWNLOAD}"
        STATUS MUSL_DOWNLOAD_STATUS)
    list(GET MUSL_DOWNLOAD_STATUS 0 MUSL_DOWNLOAD_STATUS_CODE)
    list(GET MUSL_DOWNLOAD_STATUS 1 MUSL_DOWNLOAD_STATUS_MESSAGE)
    if (NOT MUSL_DOWNLOAD_STATUS_CODE EQUAL 0)
        file(REMOVE "${MUSL_DOWNLOAD}")
        message(FATAL_ERROR
            "Failed to download ${MUSL_FILENAME}: ${MUSL_DOWNLOAD_STATUS_MESSAGE}")
    endif ()

    file(SHA256 "${MUSL_DOWNLOAD}" MUSL_ACTUAL_SHA256)
    if (NOT "${MUSL_ACTUAL_SHA256}" STREQUAL "${MUSL_SHA256}")
        file(REMOVE "${MUSL_DOWNLOAD}")
        message(FATAL_ERROR
            "Downloaded musl archive has SHA256 ${MUSL_ACTUAL_SHA256}; expected ${MUSL_SHA256}")
    endif ()

    file(RENAME "${MUSL_DOWNLOAD}" "${MUSL_ARCHIVE}")
endif ()

# The SDK provenance is computed while CMake configures. Make every input that
# contributes to it part of CMake's own regeneration graph so an incremental
# build cannot rebuild libc with stale manifest data.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MUSL_ARCHIVE}"
    "${PEDIGREE_C_COMPILER}"
    ${PEDIGREE_MUSL_PORT_INPUTS})

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}"
    COMMAND ${CMAKE_COMMAND} -E tar xzf "${MUSL_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure"
    DEPENDS
        "${MUSL_ARCHIVE}"
        "${MUSL_PORT_STATE}"
        ${PEDIGREE_MUSL_PORT_INPUTS}
)
add_custom_target(musl_extract ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure")

set(PEDIGREE_MUSL_TOOL_ENV)
if (PEDIGREE_SELF_HOSTED)
    # A compiler hosted on Pedigree is already targeting Pedigree, so native
    # tool names are sufficient even when no target-prefixed aliases exist.
    list(APPEND PEDIGREE_MUSL_TOOL_ENV
        PEDIGREE_USE_PATH_TOOLCHAIN=1
        PEDIGREE_CROSS_COMPILE_PREFIX=
        "PEDIGREE_NATIVE_TOOL_ROOT=${PEDIGREE_NATIVE_TOOL_ROOT}")
    if (CMAKE_OBJDUMP AND NOT CMAKE_OBJDUMP MATCHES "-NOTFOUND$")
        list(APPEND PEDIGREE_MUSL_TOOL_ENV PEDIGREE_OBJDUMP=${CMAKE_OBJDUMP})
    endif ()
    if (CMAKE_READELF AND NOT CMAKE_READELF MATCHES "-NOTFOUND$")
        list(APPEND PEDIGREE_MUSL_TOOL_ENV PEDIGREE_READELF=${CMAKE_READELF})
    endif ()
endif ()

set(_PEDIGREE_MUSL_BUILD_COMMAND
    ${CMAKE_COMMAND} -E env
    CC=${PEDIGREE_C_COMPILER} LD=${CMAKE_C_COMPILER}
    COMPILER_TARGET=${PEDIGREE_COMPILER_TARGET}
    PEDIGREE_DTRELR=$<BOOL:${PEDIGREE_DTRELR_ENABLED}>
    PEDIGREE_TARGET_PAGE_SIZE=${PEDIGREE_TARGET_PAGE_SIZE}
    PEDIGREE_CONFIG_INCLUDE_DIR=${PEDIGREE_MUSL_CONFIG_DIR}
    PEDIGREE_TOOLCHAIN_ROOT=${PEDIGREE_TOOLCHAIN_ROOT}
    CMAKE_COMMAND=${CMAKE_COMMAND}
    MUSL_VERSION=${MUSL_VERSION}
    MUSL_UPSTREAM_SHA256=${MUSL_SHA256}
    PEDIGREE_MUSL_PORT_REVISION=${PEDIGREE_MUSL_PORT_REVISION}
    PEDIGREE_MUSL_REVISION=${PEDIGREE_MUSL_REVISION}
    PEDIGREE_MUSL_BUILD_ID=${PEDIGREE_MUSL_BUILD_ID}
    PEDIGREE_MUSL_ABI=${PEDIGREE_MUSL_ABI}
    PEDIGREE_MUSL_PROFILE=${PEDIGREE_MUSL_PROFILE}
    PEDIGREE_MUSL_COMPILER_TARGET=${PEDIGREE_MUSL_COMPILER_TARGET}
    PEDIGREE_MUSL_COMPILER_VERSION=${PEDIGREE_MUSL_COMPILER_VERSION}
    PEDIGREE_MUSL_MANIFEST_GENERATOR=${CMAKE_SOURCE_DIR}/build-etc/cmake/GenerateMuslSdkManifest.cmake
    PEDIGREE_MUSL_MANIFEST_VALIDATOR=${CMAKE_SOURCE_DIR}/build-etc/cmake/ValidateMuslSdkManifest.cmake
    ${PEDIGREE_MUSL_TOOL_ENV}
    SRCDIR=${CMAKE_SOURCE_DIR}
    TARGETDIR=${PEDIGREE_MUSL_SDK_ROOT}
    ARCH_TARGET=${PEDIGREE_MUSL_ARCH_TARGET}
    ${CMAKE_SOURCE_DIR}/scripts/build-musl-${PEDIGREE_MUSLARCH}.sh)

# Build libc. The target also invokes this command after checking the primary
# output; the script exits immediately for a complete matching SDK and repairs
# missing secondary package files otherwise.
add_custom_command(
    OUTPUT
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libc.so
    ${MUSL_LIBNAME}
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/crt1.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/crti.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/crtn.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/rcrt1.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/Scrt1.o
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libc.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libcrypt.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libdl.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libm.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/librt.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libpthread.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libresolv.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libutil.a
    ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libxnet.a
    ${PEDIGREE_MUSL_MANIFEST}
    COMMAND ${_PEDIGREE_MUSL_BUILD_COMMAND}
    DEPENDS
    ${CMAKE_TOOLCHAIN_FILE}
    ${MUSL_DTRELR_STATE}
    ${MUSL_PAGE_SIZE_STATE}
    ${MUSL_PORT_STATE}
    ${MUSL_DERIVATION_STATE}
    ${PEDIGREE_MUSL_PORT_INPUTS}
    ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}
)

add_custom_target(libc ALL
    COMMAND ${_PEDIGREE_MUSL_BUILD_COMMAND}
    DEPENDS ${PEDIGREE_MUSL_PREFIX_ROOT}/lib/libc.so
    COMMENT "Validating the musl SDK"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}
    VERBATIM)
add_dependencies(pedigree_musl_headers libc)

if (BUILD_TESTING AND Python3_EXECUTABLE AND
    PEDIGREE_MUSL_ARCH_TARGET STREQUAL "X64")
    add_test(
        NAME musl-source-purity-tests
        COMMAND ${CMAKE_COMMAND} -E env
            PEDIGREE_MUSL_ARCHIVE=${MUSL_ARCHIVE}
            PEDIGREE_MUSL_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}
            ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_musl_source_purity.py -v)
endif ()

set(PEDIGREE_KERNEL_MODULES ${PEDIGREE_KERNEL_MODULES} PARENT_SCOPE)
set(PEDIGREE_KERNEL_MODULE_TARGETS ${PEDIGREE_KERNEL_MODULE_TARGETS} PARENT_SCOPE)

# Restore original linker flags
# set(CMAKE_SHARED_LINKER_FLAGS ${PRESERVED_SHARED_LINKER_FLAGS})

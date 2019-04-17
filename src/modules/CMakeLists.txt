
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

set(MUSL_NAME "musl-1.1.14")
set(MUSL_FILENAME "${MUSL_NAME}.tar.gz")

add_definitions(-DIN_PEDIGREE_KERNEL)

# TODO: handle static drivers
# TODO: x64-specific
set(PEDIGREE_MODULE_LINKSCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/link.ld")
set(PEDIGREE_MODULE_FLAGS ${GENERIC_FLAGS} "-fPIC" "-fno-omit-frame-pointer" "-nostdlib" "-m64"
    "-mno-red-zone" "-march=k8" "-mno-sse" "-mno-mmx")
set(PEDIGREE_MODULE_CFLAGS ${PEDIGREE_MODULE_FLAGS} ${GENERIC_CFLAGS})
set(PEDIGREE_MODULE_CXXFLAGS ${PEDIGREE_MODULE_FLAGS} ${GENERIC_CXXFLAGS} "-fno-use-cxa-atexit")
set(PEDIGREE_MODULE_LINKFLAGS "-nodefaultlibs" "-nostartfiles" "-Wl,-shared" "-Wl,-T,${PEDIGREE_MODULE_LINKSCRIPT}")

add_definitions(-DUSE_PIC_SYSCALLS)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/include)

# TODO: adjust so musl paths only get added for -user targets
link_directories(${CMAKE_BINARY_DIR}/musl/lib)

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
    add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}.o")
    target_compile_options(${name} PRIVATE
        ${opts}
        $<$<COMPILE_LANGUAGE:C>:${PEDIGREE_MODULE_CFLAGS}>
        $<$<COMPILE_LANGUAGE:CXX>:${PEDIGREE_MODULE_CXXFLAGS} -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables>)
    target_link_libraries(${name} PRIVATE ${PEDIGREE_MODULE_LINKFLAGS} ${libs} module kernel_shared)

    # Generic set of include directories for modules to find their own headers
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/system/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/${name}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/${PEDIGREE_DRIVERDIR}/${name})

    if (PEDIGREE_MODULE_${name})
        list(APPEND PEDIGREE_KERNEL_MODULES $<TARGET_FILE:${name}>)
        list(APPEND PEDIGREE_KERNEL_MODULE_TARGETS ${name})

        set(PEDIGREE_KERNEL_MODULES ${PEDIGREE_KERNEL_MODULES} PARENT_SCOPE)
        set(PEDIGREE_KERNEL_MODULE_TARGETS ${PEDIGREE_KERNEL_MODULE_TARGETS} PARENT_SCOPE)
    endif ()

    # split debug info into a .debug file so it's accessible but not present in the module binary itself
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:${name}> ${CMAKE_CURRENT_BINARY_DIR}/${name}.debug
        COMMAND ${CMAKE_STRIP} -g $<TARGET_FILE:${name}>
        COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink="${CMAKE_CURRENT_BINARY_DIR}/${name}.debug" $<TARGET_FILE:${name}>)
endfunction()

add_library(module STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Module.cc)

pedigree_module(config "-w;-Os" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/config/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/config/Config.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/config/sqlite3/sqlite3.c)
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
add_dependencies(config libc)  # TODO: this shouldn't need headers.

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
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ext2/Ext2Symlink.cc)

pedigree_module(fat "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatDirectory.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatFile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatFilesystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/fat/FatSymlink.cc)

pedigree_module(filesystems "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/filesystems/main.cc)

pedigree_module(gfx-deps "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/gfx-deps/main.cc)

if (PEDIGREE_WITH_INIT)
pedigree_module(init "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/init/main.cc)
endif ()

pedigree_module(iso9660 "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/iso9660/Iso9660Directory.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/iso9660/Iso9660File.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/iso9660/Iso9660Filesystem.cc)

pedigree_module(linker "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/DynamicLinker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/${PEDIGREE_LINKERDIR}/DynamicLinker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/${PEDIGREE_LINKERDIR}/asm-${PEDIGREE_LINKERDIR}.s)

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/system/linker/${PEDIGREE_LINKERDIR}/asm-${PEDIGREE_LINKERDIR}.s
    PROPERTIES LANGUAGE ASM_NASM)

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

pedigree_module(network-stack "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/Filter.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/NetworkStack.cc)
add_dependencies(network-stack lwip)

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

pedigree_module(rawfs "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/rawfs/RawFs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/rawfs/RawFsDir.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/rawfs/RawFsFile.cc)

pedigree_module(splash "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/splash/main.cc)

pedigree_module(status_server "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/status_server/main.cc)
add_dependencies(status_server lwip)

pedigree_module(usb "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/UsbDevice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/UsbHub.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/UsbPnP.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/usb/main.cc)

pedigree_module(users "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/users/Group.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/users/User.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/users/UserManager.cc)

pedigree_module(vfs "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Directory.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/File.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Filesystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/LockedFile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/MemoryMappedFile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Pipe.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/Symlink.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/vfs/VFS.cc)

pedigree_module(3c90x "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/3c90x/3Com90x.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/3c90x/main.cc)

pedigree_module(ata "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/AtaController.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/AtaDisk.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/BusMasterIde.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/IsaAtaController.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/PciAtaController.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/ata/main.cc)

pedigree_module(cdi "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiDisk.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/CdiNet.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/_list.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/cdi.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/compat.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/dma.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/lists.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/misc.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/net.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/pci.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/storage.cc)
target_include_directories(cdi PRIVATE ${CDI_INCLUDE_DIRS})

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

# TODO: loopback would go here, was commented out in modules/SConscript

pedigree_module(partition "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/Partition.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/apple.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/partition/msdos.cc)

# TODO: rtl8139 would go here, was commented out in modules/SConscript

pedigree_module(scsi "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/ScsiCommands.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/ScsiController.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/ScsiDisk.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/scsi/main.cc)

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

pedigree_module(usb-mass-storage "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-mass-storage/UsbMassStorageDevice.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/usb-mass-storage/main.cc)

pedigree_module(e1000 "-w" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/e1000/device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/e1000/main.c)
target_include_directories(e1000 PRIVATE ${CDI_INCLUDE_DIRS})

pedigree_module(pcnet "-w" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/pcnet/pcnet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/pcnet/main.c)
target_include_directories(pcnet PRIVATE ${CDI_INCLUDE_DIRS})

pedigree_module(rtl8139 "-w" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/rtl8139/rtl8139.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/rtl8139/main.c)
target_include_directories(rtl8139 PRIVATE ${CDI_INCLUDE_DIRS})

pedigree_module(sis900 "-w" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/eeprom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/sis900/sis900_io.c)
target_include_directories(sis900 PRIVATE ${CDI_INCLUDE_DIRS})

if (PEDIGREE_DRIVERDIR STREQUAL "x86")
    pedigree_module(ib700_wdt "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ib700_wdt/main.cc)

    pedigree_module(ne2k "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ne2k/Ne2k.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/ne2k/main.cc)

    pedigree_module(pci "" ""
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/x86/pci/pci.cc)

    pedigree_module(ps2mouse "" ""
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
target_compile_options(vdso PRIVATE "-mcmodel=small")
target_link_libraries(vdso PRIVATE
    "-Wl,--soname,linux-vdso.so.1" "-Wl,--hash-style=both" "-Wl,-Bsymbolic"
    "-nostdlib"
    "-T${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vdso.ld"
    "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/glue-infoblock.ld")
add_dependencies(vdso libc)

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vdso.h
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/genvdso.py $<TARGET_FILE:vdso> ${CMAKE_CURRENT_BINARY_DIR}/vdso.h
    DEPENDS vdso)

pedigree_module(posix "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/console-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/DevFs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/FileDescriptor.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/file-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/IoEvent.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/net-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/pipe-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PollEvent.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/poll-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/posix.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PosixProcess.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PosixSubsystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PosixSyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/ProcFs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/PsAuxFile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/pthread-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/select-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/signal-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/system-syscalls.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/UnixFilesystem.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/util.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/VirtualTerminal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vsyscall-amd64.s
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/sigret-${PEDIGREE_ASMDIR}.s
    ${CMAKE_CURRENT_BINARY_DIR}/vdso.h)
target_include_directories(posix PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/syscalls)

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vsyscall-amd64.s
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/sigret-${PEDIGREE_ASMDIR}.s
    PROPERTIES LANGUAGE ASM_NASM)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/native/include
    ${CMAKE_SOURCE_DIR}/images/local/support/gcc/include/c++/${CMAKE_CXX_COMPILER_VERSION}
    ${CMAKE_SOURCE_DIR}/images/local/support/gcc/include/c++/${CMAKE_CXX_COMPILER_VERSION}/${PEDIGREE_COMPILER_TARGET})
target_link_libraries(native-user PRIVATE -nostdlib gcc)
set_target_properties(native-user PROPERTIES OUTPUT_NAME "pedigree")
add_dependencies(native-user libc)

pedigree_module(pedigree-c "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/PedigreeCSyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-c.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-syscalls.cc)
target_include_directories(native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c)

add_library(pedigree-c-user SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-c-syscalls.c)
set_target_properties(pedigree-c-user PROPERTIES OUTPUT_NAME "pedigree-c")
add_dependencies(pedigree-c-user libc)

set(PEDIGREE_SUBSYSTEM_LIBS $<TARGET_FILE:native-user> $<TARGET_FILE:pedigree-c-user> PARENT_SCOPE)

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar
    COMMAND ${CMAKE_COMMAND} -E tar "-czPf" ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar -- ${PEDIGREE_KERNEL_MODULES}
    DEPENDS ${PEDIGREE_KERNEL_MODULE_TARGETS})
add_custom_target(initrd ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar)

file(DOWNLOAD "https://www.musl-libc.org/releases/${MUSL_FILENAME}" ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_FILENAME})

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure
    COMMAND ${CMAKE_COMMAND} -E tar xzf ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_FILENAME}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_FILENAME}
)
add_custom_target(musl_extract ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure)

# Build libc
add_custom_command(
    OUTPUT
    ${CMAKE_BINARY_DIR}/musl/lib/libc.so
    ${CMAKE_BINARY_DIR}/musl/lib/ld-musl-x86_64.so.1
    ${CMAKE_BINARY_DIR}/musl/lib/crt1.o
    ${CMAKE_BINARY_DIR}/musl/lib/crti.o
    ${CMAKE_BINARY_DIR}/musl/lib/crtn.o
    ${CMAKE_BINARY_DIR}/musl/lib/rcrt1.o
    ${CMAKE_BINARY_DIR}/musl/lib/Scrt1.o
    ${CMAKE_BINARY_DIR}/musl/lib/libc.a
    ${CMAKE_BINARY_DIR}/musl/lib/libcrypt.a
    ${CMAKE_BINARY_DIR}/musl/lib/libdl.a
    ${CMAKE_BINARY_DIR}/musl/lib/libm.a
    ${CMAKE_BINARY_DIR}/musl/lib/librt.a
    ${CMAKE_BINARY_DIR}/musl/lib/libpthread.a
    ${CMAKE_BINARY_DIR}/musl/lib/libresolv.a
    ${CMAKE_BINARY_DIR}/musl/lib/libutil.a
    ${CMAKE_BINARY_DIR}/musl/lib/libxnet.a
    COMMAND
    ${CMAKE_COMMAND} -E env
    CC=${CMAKE_C_COMPILER} LD=${CMAKE_C_COMPILER}
    COMPILER_TARGET=${PEDIGREE_COMPILER_TARGET}
    SRCDIR=${CMAKE_SOURCE_DIR}
    TARGETDIR=${CMAKE_BINARY_DIR}/musl
    ARCH_TARGET=${PEDIGREE_ARCH_TARGET}
    ${CMAKE_SOURCE_DIR}/scripts/build-musl.sh
    DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/fb.h
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/glue-musl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/klog.h
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/syscall_arch.h
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/syscall_cp-x86_64.musl-s
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/musl/ttyname.c
    ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}/configure
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${MUSL_NAME}
)

add_custom_target(libc ALL DEPENDS ${CMAKE_BINARY_DIR}/musl/lib/libc.so)

set(PEDIGREE_KERNEL_MODULES ${PEDIGREE_KERNEL_MODULES} PARENT_SCOPE)
set(PEDIGREE_KERNEL_MODULE_TARGETS ${PEDIGREE_KERNEL_MODULE_TARGETS} PARENT_SCOPE)

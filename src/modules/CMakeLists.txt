
add_definitions(-DIN_PEDIGREE_KERNEL)

# TODO: handle static drivers
set(PEDIGREE_MODULE_LINKSCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/link.ld")
set(PEDIGREE_MODULE_FLAGS ${GENERIC_FLAGS} "-fPIC" "-fno-omit-frame-pointer" "-nostdlib")
set(PEDIGREE_MODULE_CFLAGS ${PEDIGREE_MODULE_FLAGS} ${GENERIC_CFLAGS})
set(PEDIGREE_MODULE_CXXFLAGS ${PEDIGREE_MODULE_FLAGS} ${GENERIC_CXXFLAGS} "-fno-use-cxa-atexit")
set(PEDIGREE_MODULE_LINKFLAGS "-nodefaultlibs" "-nostartfiles" "-Wl,-shared" "-Wl,-T,${PEDIGREE_MODULE_LINKSCRIPT}")

add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:${PEDIGREE_MODULE_CFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${PEDIGREE_MODULE_CXXFLAGS}>")

add_definitions(-DUSE_PIC_SYSCALLS)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/system/lwip/include)

set(CDI_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/cdi/include
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/include)

function(pedigree_module name opts libs)
    add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}.o")
    target_compile_options(${name} PRIVATE
        ${opts}
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables>)
    target_link_libraries(${name} PRIVATE ${PEDIGREE_MODULE_LINKFLAGS} ${libs} module kernel_shared)

    # Generic set of include directories for modules to find their own headers
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/system/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/common/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/${name}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/${PEDIGREE_DRIVERDIR}/${name})

    list(APPEND PEDIGREE_KERNEL_MODULES $<TARGET_FILE:${name}>)
    list(APPEND PEDIGREE_KERNEL_MODULE_TARGETS ${name})

    set(PEDIGREE_KERNEL_MODULES ${PEDIGREE_KERNEL_MODULES} PARENT_SCOPE)
    set(PEDIGREE_KERNEL_MODULE_TARGETS ${PEDIGREE_KERNEL_MODULE_TARGETS} PARENT_SCOPE)
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

pedigree_module(confignics "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/confignics/main.cc)

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

pedigree_module(init "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/init/main.cc)

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

# TODO: lwip

pedigree_module(mountroot "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/mountroot/main.cc)

pedigree_module(network-stack "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/Filter.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/system/network-stack/NetworkStack.cc)

pedigree_module(nics "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/nics/main.cc)

pedigree_module(pcap "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/system/pcap/main.cc)

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

pedigree_module(e1000 "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/e1000/device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/e1000/main.c)
target_include_directories(e1000 PRIVATE ${CDI_INCLUDE_DIRS})

pedigree_module(pcnet "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/pcnet/pcnet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/pcnet/main.c)
target_include_directories(pcnet PRIVATE ${CDI_INCLUDE_DIRS})

pedigree_module(rtl8139 "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/rtl8139/rtl8139.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/cdi/rtl8139/main.c)
target_include_directories(rtl8139 PRIVATE ${CDI_INCLUDE_DIRS})

pedigree_module(sis900 "" ""
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
    ${CMAKE_CURRENT_BINARY_DIR}/vdso.h)
target_include_directories(posix PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/syscalls)

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/posix/vsyscall-amd64.s
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
target_link_libraries(native-user PRIVATE -nostdlib)
set_target_properties(native-user PROPERTIES OUTPUT_NAME "native")

pedigree_module(pedigree-c "" ""
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/PedigreeCSyscallManager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-c.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-syscalls.cc)
target_include_directories(native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c)

add_library(pedigree-c-user SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys/pedigree-c/pedigree-c-syscalls.c)
set_target_properties(pedigree-c-user PROPERTIES OUTPUT_NAME "pedigree-c")

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar
    COMMAND ${CMAKE_COMMAND} -E tar "-czPf" ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar -- ${PEDIGREE_KERNEL_MODULES}
    DEPENDS ${PEDIGREE_KERNEL_MODULE_TARGETS})
add_custom_target(initrd ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar)


# TODO: x64-specific
set(GENERIC_BOOT_FLAGS
    "-fno-builtin" "-nostdlib" "-ffreestanding" "-fno-common" "-fno-omit-frame-pointer"
    "-ggdb" "-g3" "-gdwarf-2" "--sysroot=${CMAKE_SYSROOT}")
set(GENERIC_BOOT_CFLAGS "")
set(GENERIC_BOOT_CXXFLAGS
    "-fno-exceptions" "-fno-rtti" "-fno-asynchronous-unwind-tables"
    "-fno-use-cxa-atexit")
set(GENERIC_BOOT_LINKFLAGS
    "-nostdlib" "-nostartfiles" "-Wl,--discard-all" "-static-libgcc" "--sysroot=${CMAKE_SYSROOT}")
set(GENERIC_BOOT_LINKFLAGS_NOLINKSCRIPT ${GENERIC_BOOT_LINKFLAGS})
set(ARCH_BOOT_LINKSCRIPT_FILENAME "${CMAKE_CURRENT_SOURCE_DIR}/link-arm-beagle.ld")
set(ARCH_BOOT_LINKSCRIPT "-T${ARCH_BOOT_LINKSCRIPT_FILENAME}")

# TODO: for non-v7 targets we need to adjust this
set(ARCH_BOOT_FLAGS "-mabi=aapcs" "-mapcs-frame" "-mcpu=cortex-a8" "-mtune=cortex-a8" "-mfpu=vfp")
set(ARCH_BOOT_LINKFLAGS "")

set(BOOT_CFLAGS
    ${GENERIC_CFLAGS} ${GENERIC_BOOT_FLAGS} ${GENERIC_BOOT_CFLAGS} ${ARCH_BOOT_FLAGS})
set(BOOT_CXXFLAGS
    ${GENERIC_CXXFLAGS} ${GENERIC_BOOT_FLAGS} ${GENERIC_BOOT_CXXFLAGS} ${ARCH_BOOT_FLAGS})

add_definitions(
    -DIN_PEDIGREE_BOOT -DDEBUGGER_QWERTY -DBOOT_STANDALONE)

add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:${BOOT_CFLAGS}>"
    "$<$<COMPILE_LANGUAGE:ASM>:${BOOT_CFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${BOOT_CXXFLAGS}>")

# generate kernel as an embedded file
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedkernel.c
    COMMAND $<TARGET_FILE:host-headerify>
    -i $<TARGET_FILE:kernel>
    -o ${CMAKE_CURRENT_BINARY_DIR}/embedkernel.c
    -v embeddedKernel
    DEPENDS kernel)
add_custom_target(embedkernel ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/embedkernel.c)

# build the boot shim
add_executable(beagleboot main_beagle.cc Elf32.cc support.cc arm_support.s ${CMAKE_CURRENT_BINARY_DIR}/embedkernel.c)
target_link_libraries(beagleboot PRIVATE ${GENERIC_LINKFLAGS} ${GENERIC_BOOT_LINKFLAGS} ${ARCH_BOOT_LINKFLAGS} ${ARCH_BOOT_LINKSCRIPT})
set_target_properties(beagleboot PROPERTIES LINK_DEPENDS ${ARCH_BOOT_LINKSCRIPT_FILENAME})

# build the final image
# TODO: we need to find the mkimage binary to make this more reliable
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/beagleboot.flt
    COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:beagleboot>
    ${CMAKE_CURRENT_BINARY_DIR}/beagleboot.flt
    DEPENDS beagleboot)
add_custom_target(beagleboot_flat ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/beagleboot.flt)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/beagle_uImage
    COMMAND mkimage -A arm -O linux -T kernel -C none -a 0x87000000 -e 0x87000000 -n pedigree
    -d ${CMAKE_CURRENT_BINARY_DIR}/beagleboot.flt ${CMAKE_BINARY_DIR}/beagle_uImage
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/beagleboot.flt)
add_custom_target(uimage ALL DEPENDS ${CMAKE_BINARY_DIR}/beagle_uImage)

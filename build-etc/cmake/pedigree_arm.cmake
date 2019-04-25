set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 1)

# CMAKE_TRY_COMPILE_TARGET_TYPE is new in 3.6 and newer, so we need this little
# song and dance to make sure older versions of cmake still work with this
# toolchain file.
if (${CMAKE_VERSION} VERSION_LESS 3.6.0)
    include(CMakeForceCompiler)
    cmake_force_c_compiler(${CMAKE_SOURCE_DIR}/compilers/dir/bin/arm-pedigree-gcc GNU)
    cmake_force_cxx_compiler(${CMAKE_SOURCE_DIR}/compilers/dir/bin/arm-pedigree-g++ GNU)

    set(CMAKE_OBJCOPY ${CMAKE_SOURCE_DIR}/compilers/dir/bin/arm-pedigree-objcopy)
    set(CMAKE_STRIP ${CMAKE_SOURCE_DIR}/compilers/dir/bin/arm-pedigree-strip)
else ()
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_C_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/arm-pedigree-gcc)
    set(CMAKE_CXX_COMPILER ${CMAKE_SOURCE_DIR}/compilers/dir/bin/arm-pedigree-g++)
endif ()

set(CMAKE_SYSROOT "${CMAKE_SOURCE_DIR}/compilers/dir/arm-pedigree")

set(CMAKE_FIND_ROOT_PATH ${CMAKE_SOURCE_DIR}/compilers/dir ${CMAKE_SOURCE_DIR}/compilers/dir/bin ${CMAKE_SOURCE_DIR}/compilers/dir/arm-pedigree)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(TARGET_SUPPORTS_SHARED_LIBS TRUE)

set(PEDIGREE_COMPILER_TARGET arm-pedigree)
set(PEDIGREE_ARCH_TARGET arm)
set(PEDIGREE_LINKERSCRIPT "${CMAKE_SOURCE_DIR}/src/system/kernel/link-arm-beagle.ld")
set(PEDIGREE_ARCHDIR "armv7")
set(PEDIGREE_MACHDIR "arm_beagle")  # ??
set(PEDIGREE_LINKERDIR "arm")
set(PEDIGREE_DRIVERDIR "arm")
# TODO: normalize on this so the number of definitions above can be reduced
set(PEDIGREE_ASMDIR "arm")
set(GRUB FALSE)

# Definitions for ARMv7 across the entire source tree.
# NOTE: we emulate Multiboot for the boot -> kernel interface.
add_definitions(-DARM_COMMON=1 -DARMV7=1 -DARM_BEAGLE=1 -DBITS_32=1 -DBITS_64=0
    -DTHREADS=1 -DKERNEL_STANDALONE=1 -DMULTIBOOT=1 -DTARGET_IS_LITTLE_ENDIAN=1
    -DKERNEL_NEEDS_ADDRESS_SPACE_SWITCH=1 -DKERNEL_PROCESSOR_NO_PORT_IO=1
    -DSTATIC_DRIVERS=1 -DTARGET_HAS_NO_ATOMICS=0)

# Hack around the shared library bits on the Linux platform.
set(__LINUX_COMPILER_GNU 1)  # don't add -rdynamic

macro(__linux_compiler_gnu lang)
    set(CMAKE_${lang}_COMPILER_PREDEFINES_COMMAND "${CMAKE_${lang}_COMPILER}" "-dM" "-E" "-c" "${CMAKE_ROOT}/Modules/CMakeCXXCompilerABI.cpp")
endmacro()

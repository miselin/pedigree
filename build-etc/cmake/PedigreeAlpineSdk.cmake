include_guard(GLOBAL)

function(pedigree_use_alpine_sdk)
    if (PEDIGREE_ARCH_TARGET STREQUAL "X64")
        set(architecture x86_64)
        set(loader_arch x86_64)
        set(elf_class 02)
        set(elf_machine 3e00)
    elseif (PEDIGREE_ARCH_TARGET STREQUAL "ARM64")
        set(architecture aarch64)
        set(loader_arch aarch64)
        set(elf_class 02)
        set(elf_machine b700)
    elseif (PEDIGREE_ARCH_TARGET STREQUAL "ARMV7")
        set(architecture armv7)
        set(loader_arch armhf)
        set(elf_class 01)
        set(elf_machine 2800)
    else ()
        message(FATAL_ERROR "No Alpine SDK for ${PEDIGREE_ARCH_TARGET}")
    endif ()

    set(instruction
        "Run scripts/alpine/build.sh ${architecture} first, or set PEDIGREE_TARGET_SYSROOT to a prepared ${architecture} sysroot.")
    if (NOT IS_ABSOLUTE "${PEDIGREE_TARGET_SYSROOT}" OR
        NOT IS_DIRECTORY "${PEDIGREE_TARGET_SYSROOT}")
        message(FATAL_ERROR "Alpine SDK is missing: ${PEDIGREE_TARGET_SYSROOT}. ${instruction}")
    endif ()

    set(root "${PEDIGREE_TARGET_SYSROOT}")
    set(loader "lib/ld-musl-${loader_arch}.so.1")
    foreach(relative
        "${loader}"
        usr/include/errno.h usr/include/stdio.h usr/include/stdlib.h
        usr/include/stdint.h usr/include/unistd.h usr/include/bits/syscall.h
        usr/include/sys/syscall.h
        usr/lib/crt1.o usr/lib/Scrt1.o usr/lib/rcrt1.o usr/lib/crti.o usr/lib/crtn.o
        usr/lib/libc.a usr/lib/libm.a usr/lib/libpthread.a usr/lib/libdl.a usr/lib/librt.a)
        if (NOT EXISTS "${root}/${relative}" OR IS_DIRECTORY "${root}/${relative}")
            message(FATAL_ERROR "Alpine SDK is missing ${relative}. ${instruction}")
        endif ()
        file(SIZE "${root}/${relative}" size)
        if (size EQUAL 0)
            message(FATAL_ERROR "Alpine SDK file is empty: ${relative}. ${instruction}")
        endif ()
    endforeach ()

    file(READ "${root}/${loader}" header LIMIT 20 HEX)
    string(LENGTH "${header}" header_length)
    if (NOT header_length EQUAL 40)
        message(FATAL_ERROR "Alpine SDK loader has a truncated ELF header. ${instruction}")
    endif ()
    string(SUBSTRING "${header}" 0 14 actual_ident)
    string(SUBSTRING "${header}" 36 4 actual_machine)
    if (NOT actual_ident STREQUAL "7f454c46${elf_class}0101" OR
        NOT actual_machine STREQUAL elf_machine)
        message(FATAL_ERROR "Alpine SDK loader is not ${architecture} ELF. ${instruction}")
    endif ()

    if (NOT IS_SYMLINK "${root}/usr/lib/libc.so")
        message(FATAL_ERROR "Alpine SDK usr/lib/libc.so must link to ${loader}. ${instruction}")
    endif ()
    file(READ_SYMLINK "${root}/usr/lib/libc.so" libc_target)
    file(REAL_PATH "${root}/usr/lib/libc.so" resolved_libc)
    file(REAL_PATH "${root}/${loader}" resolved_loader)
    if (IS_ABSOLUTE "${libc_target}" OR
        NOT resolved_libc STREQUAL resolved_loader OR
        IS_SYMLINK "${root}/${loader}")
        message(FATAL_ERROR "Alpine SDK libc.so must resolve to its in-root ${loader}. ${instruction}")
    endif ()

    set(PEDIGREE_MUSL_SDK_ROOT "${root}" PARENT_SCOPE)
    set(PEDIGREE_MUSL_PREFIX_ROOT "${root}/usr" PARENT_SCOPE)
    set(PEDIGREE_MUSL_DYNAMIC_LOADER "/${loader}" PARENT_SCOPE)
endfunction()

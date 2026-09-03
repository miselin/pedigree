include_guard(GLOBAL)

macro(__pedigree_compiler_gnu lang)
    set(CMAKE_${lang}_VERBOSE_LINK_FLAG "-Wl,-v")

    set(CMAKE_${lang}_USING_LINKER_SYSTEM "")
    set(CMAKE_${lang}_USING_LINKER_LLD "-fuse-ld=lld")
    set(CMAKE_${lang}_USING_LINKER_BFD "-fuse-ld=bfd")
    set(CMAKE_${lang}_USING_LINKER_GOLD "-fuse-ld=gold")
    if (NOT CMAKE_${lang}_COMPILER_ID STREQUAL "GNU" OR
        CMAKE_${lang}_COMPILER_VERSION VERSION_GREATER_EQUAL "12.1")
        set(CMAKE_${lang}_USING_LINKER_MOLD "-fuse-ld=mold")
    endif ()
endmacro()

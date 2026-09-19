# Vendored from https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz.
# Kernel and userspace consumers need separate archives because interrupts can
# overwrite the x86-64 red zone and kernel modules cannot use SIMD registers.
set(LIBVTERM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/keyboard.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mouse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pen.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/screen.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/unicode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vterm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pedigree_vsnprintf.c)

foreach(vterm_target pedigree-vterm pedigree-vterm-kernel)
    add_library(${vterm_target} STATIC ${LIBVTERM_SOURCES})
    set_target_properties(${vterm_target} PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
    target_compile_features(${vterm_target} PRIVATE c_std_99)
    target_compile_options(${vterm_target} PRIVATE -fno-strict-aliasing)
    target_include_directories(${vterm_target}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # The hosted kernel and its archive use the host libc ABI.
    if(NOT PEDIGREE_ARCH_TARGET STREQUAL "HOSTED" OR
       NOT vterm_target STREQUAL "pedigree-vterm-kernel")
        target_link_libraries(${vterm_target} PUBLIC pedigree_musl_headers)
    endif()
endforeach()

set_target_properties(pedigree-vterm PROPERTIES OUTPUT_NAME vterm)
set_target_properties(pedigree-vterm-kernel PROPERTIES OUTPUT_NAME vterm-kernel)
if(PEDIGREE_ARCH_TARGET STREQUAL "X64" OR PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")
    target_compile_options(pedigree-vterm-kernel PRIVATE -mno-red-zone -mno-sse -mno-mmx)
endif()

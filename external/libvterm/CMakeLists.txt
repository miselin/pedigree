# Vendored from https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz.
# Keep this as a target-local C99 archive: both freestanding kernel modules and
# userspace libraries consume the same parser and screen implementation.
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

add_library(pedigree-vterm STATIC ${LIBVTERM_SOURCES})
set_target_properties(pedigree-vterm PROPERTIES
    OUTPUT_NAME vterm
    POSITION_INDEPENDENT_CODE TRUE)
target_compile_features(pedigree-vterm PRIVATE c_std_99)
target_compile_options(pedigree-vterm PRIVATE -fno-strict-aliasing)
target_include_directories(pedigree-vterm
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pedigree-vterm PUBLIC pedigree_musl_headers)

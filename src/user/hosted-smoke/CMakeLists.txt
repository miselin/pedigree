set(CMAKE_C_COMPILER ${PEDIGREE_C_COMPILER})

link_directories(${PEDIGREE_MUSL_PREFIX_ROOT}/lib)
include_directories(${PEDIGREE_MUSL_PREFIX_ROOT}/include)

if (PEDIGREE_HOSTED_DARWIN)
    # These programs are Pedigree ELF even though the hosted kernel is Mach-O.
    set(CMAKE_EXECUTABLE_CREATE_C_FLAGS "")
    set(CMAKE_C_LINK_FLAGS "")
endif ()

function(hosted_smoke_app target output source)
    add_executable(${target} ${source})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${output}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/user)
    if (APPLE)
        set_target_properties(${target} PROPERTIES OSX_ARCHITECTURES "")
    endif ()
    target_compile_options(${target} PRIVATE -fPIE)
    target_link_libraries(${target} PRIVATE pedigree-c-user gcc -pie)
    target_link_options(${target} PRIVATE
        -Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}
        -Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE})
    if (PEDIGREE_DTRELR_ENABLED)
        target_link_options(${target} PRIVATE -Wl,-z,pack-relative-relocs)
    endif ()
    add_dependencies(${target} libc)

    list(APPEND PEDIGREE_APPS $<TARGET_FILE:${target}>)
    list(APPEND PEDIGREE_APPS_TARGETS ${target})
    set(PEDIGREE_APPS ${PEDIGREE_APPS} PARENT_SCOPE)
    set(PEDIGREE_APPS_TARGETS ${PEDIGREE_APPS_TARGETS} PARENT_SCOPE)
endfunction()

hosted_smoke_app(
    hosted-smoke-init init
    ${CMAKE_CURRENT_SOURCE_DIR}/init.c)
hosted_smoke_app(
    hosted-smoke-command hosted-smoke-command
    ${CMAKE_CURRENT_SOURCE_DIR}/command.c)

set(PEDIGREE_APPS ${PEDIGREE_APPS} PARENT_SCOPE)
set(PEDIGREE_APPS_TARGETS ${PEDIGREE_APPS_TARGETS} PARENT_SCOPE)

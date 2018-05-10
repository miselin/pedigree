
link_directories(
    ${CMAKE_SOURCE_DIR}/images/local/libraries)

include_directories(
    ${CMAKE_SOURCE_DIR}/src/modules/subsys/native/include
    ${CMAKE_SOURCE_DIR}/images/local/include
    ${CMAKE_SOURCE_DIR}/images/local/libraries/glib-2.0/include
    ${CMAKE_SOURCE_DIR}/images/local/include/glib-2.0
    ${CMAKE_SOURCE_DIR}/images/local/include/pango-1.0
    ${CMAKE_SOURCE_DIR}/images/local/include/cairo
    ${CMAKE_SOURCE_DIR}/images/local/include/freetype2
    ${CMAKE_SOURCE_DIR}/images/local/support/gcc/include/c++/${CMAKE_CXX_COMPILER_VERSION}
    ${CMAKE_SOURCE_DIR}/images/local/support/gcc/include/c++/${CMAKE_CXX_COMPILER_VERSION}/${PEDIGREE_COMPILER_TARGET})

function(pedigree_app name posix native pedigreec libs)
    add_executable(app-${name} ${ARGN})
    set_target_properties(app-${name} PROPERTIES OUTPUT_NAME "${name}")
    set(TARGET_LIBS ${libs})
    if (native)
        set(TARGET_LIBS ${TARGET_LIBS} native-user pedigree-c-user)
    endif ()
    if (pedigreec AND NOT native)
        set(TARGET_LIBS ${TARGET_LIBS} pedigree-c-user)
    endif ()

    target_link_libraries(app-${name} PRIVATE ${TARGET_LIBS})
    target_include_directories(app-${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/applications/${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/applications/${name}/include)
    add_dependencies(app-${name} libc)

    list(APPEND PEDIGREE_APPS $<TARGET_FILE:app-${name}>)
    list(APPEND PEDIGREE_APPS_TARGETS app-${name})

    set(PEDIGREE_APPS ${PEDIGREE_APPS} PARENT_SCOPE)
    set(PEDIGREE_APPS_TARGETS ${PEDIGREE_APPS_TARGETS} PARENT_SCOPE)
endfunction()

function(pedigree_lib name libs)
    add_library(lib${name} SHARED ${ARGN})
    set_target_properties(lib${name} PROPERTIES OUTPUT_NAME "${name}")
    target_link_libraries(lib${name} PRIVATE ${libs})
    target_include_directories(lib${name}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libraries/lib${name}/src
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/libraries/lib${name}/include)
    add_dependencies(lib${name} libc)

    list(APPEND PEDIGREE_LIBS $<TARGET_FILE:lib${name}>)
    list(APPEND PEDIGREE_LIBS_TARGETS lib${name})

    set(PEDIGREE_LIBS ${PEDIGREE_LIBS} PARENT_SCOPE)
    set(PEDIGREE_LIBS_TARGETS ${PEDIGREE_LIBS_TARGETS} PARENT_SCOPE)
endfunction()

# libraries

pedigree_lib(fb "" ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libfb/src/fb.cc)
pedigree_lib(tui "libui"
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libtui/src/environment.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libtui/src/Font.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libtui/src/Png.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libtui/src/Terminal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libtui/src/tui.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libtui/src/Xterm.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libtui/src/Xterm-colours.cc)
pedigree_lib(ui "native-user"
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libui/src/protocol.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libui/src/ShapeWidget.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libui/src/TextWidget.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libui/src/Widget.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libui/src/Window.cc)

# apps

set(PANGO_LIBS "pangocairo-1.0;pangoft2-1.0;pango-1.0;harfbuzz;fontconfig;gobject-2.0;glib-2.0;ffi")

pedigree_app(TUI ON ON OFF "libui;libtui;cairo;${PANGO_LIBS};freetype" ${CMAKE_CURRENT_SOURCE_DIR}/applications/TUI/main.cc)
pedigree_app(apptest ON ON OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/apptest/main.cc)
pedigree_app(cdhelp ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/cdhelp/main.c)
pedigree_app(config ON ON OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/config/main.cc)
pedigree_app(crashtest ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/crashtest/main.c)
pedigree_app(display ON OFF OFF-mode "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/display-mode/main.c)
# pedigree_app(fire ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/fire/fire.c)
pedigree_app(gears ON ON OFF "libui;OSMesa" ${CMAKE_CURRENT_SOURCE_DIR}/applications/gears/gears.cc)
pedigree_app(gfxcon ON ON OFF "libui;libfb;libtui;cairo;${PANGO_LIBS};freetype" ${CMAKE_CURRENT_SOURCE_DIR}/applications/gfxcon/main.cc)
pedigree_app(init ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/init/main.c)
pedigree_app(ipc-test-client ON ON OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-test-client/main.cc)
pedigree_app(ipc-test-server ON ON OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-test-server/main.cc)
pedigree_app(keymap ON OFF ON ""
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/main.c)
target_link_libraries(app-keymap PRIVATE libkeymap)
pedigree_app(live ON OFF ON "intl;dialog" ${CMAKE_CURRENT_SOURCE_DIR}/applications/live/live.cc)
pedigree_app(login ON OFF ON "intl" ${CMAKE_CURRENT_SOURCE_DIR}/applications/login/login.c)
pedigree_app(modlist ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/modlist/main.c)
pedigree_app(modload ON OFF ON ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/modload/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/modload/elf.c)
pedigree_app(modunload ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/modunload/main.c)
pedigree_app(mount ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/mount/main.c)
pedigree_app(net-test ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/net-test/net-test.c)
pedigree_app(nyancat ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/nyancat/nyancat.c)
pedigree_app(preloadd ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/preloadd/main.c)
pedigree_app(reboot ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/reboot/main.c)
pedigree_app(sudo ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/sudo/main.c)
pedigree_app(syscall-test ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/syscall-test/syscall-test.c)
pedigree_app(testsuite ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/fs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/mprotect.c)
pedigree_app(thread-test ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/thread-test/main.cc)
pedigree_app(tour ON OFF OFF "intl;dialog" ${CMAKE_CURRENT_SOURCE_DIR}/applications/tour/main.cc)
pedigree_app(uitest ON ON OFF "libui" ${CMAKE_CURRENT_SOURCE_DIR}/applications/uitest/main.cc)
pedigree_app(winman ON ON OFF "libui;libfb;png;cairo;${PANGO_LIBS};freetype"
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/Png.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/objects.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/util.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/winman.cc)
pedigree_app(which ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/which/main.cc)

pedigree_keymap(KeymapDeDe ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/keymaps/KeymapDeDe)
pedigree_keymap(KeymapEnUk ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/keymaps/KeymapEnUk)
pedigree_keymap(KeymapEnUs ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/keymaps/KeymapEnUs)

# Push lib and app list into parent scope.
set(PEDIGREE_APPS ${PEDIGREE_APPS} PARENT_SCOPE)
set(PEDIGREE_APPS_TARGETS ${PEDIGREE_APPS_TARGETS} PARENT_SCOPE)
set(PEDIGREE_LIBS ${PEDIGREE_LIBS} PARENT_SCOPE)
set(PEDIGREE_LIBS_TARGETS ${PEDIGREE_LIBS_TARGETS} PARENT_SCOPE)
set(PEDIGREE_KEYMAPS_TARGETS ${PEDIGREE_KEYMAPS_TARGETS} PARENT_SCOPE)

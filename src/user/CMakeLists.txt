# Use the Pedigree compiler regardless of the system target
include(PedigreeMuslLink)
if (PEDIGREE_ARCH_TARGET STREQUAL "X64")
    link_libraries(pedigree_musl_headers)
endif ()

set(CMAKE_C_COMPILER ${PEDIGREE_C_COMPILER})
set(CMAKE_CXX_COMPILER ${PEDIGREE_CXX_COMPILER})

if (PEDIGREE_SELF_HOSTED)
    set(PEDIGREE_USER_LIBRARY_DIRS
        ${PEDIGREE_MUSL_PREFIX_ROOT}/lib
        ${PEDIGREE_NATIVE_RUNTIME_LIBRARY_DIRS}
        /usr/lib)
    set(PEDIGREE_USER_INCLUDE_DIRS
        ${PEDIGREE_MUSL_PREFIX_ROOT}/include
        ${CMAKE_SOURCE_DIR}/src/modules/subsys/native/include
        /usr/lib/glib-2.0/include
        /usr/include/glib-2.0
        /usr/include/pango-1.0
        /usr/include/cairo
        /usr/include/freetype2)
    set(PEDIGREE_USER_RPATH_LINK_DIRS
        ${PEDIGREE_NATIVE_RUNTIME_LIBRARY_DIRS}
        /usr/lib)
    list(JOIN PEDIGREE_USER_RPATH_LINK_DIRS ":" PEDIGREE_USER_RPATH_LINK)
else ()
    set(PEDIGREE_USER_LIBRARY_DIRS
        ${PEDIGREE_MUSL_PREFIX_ROOT}/lib
        ${PEDIGREE_TOOLCHAIN_ROOT}/${PEDIGREE_COMPILER_TARGET}/lib
        ${CMAKE_SOURCE_DIR}/images/local/libraries)
    set(PEDIGREE_USER_INCLUDE_DIRS
        ${PEDIGREE_MUSL_PREFIX_ROOT}/include
        ${CMAKE_SOURCE_DIR}/src/modules/subsys/native/include
        ${CMAKE_SOURCE_DIR}/images/local/include
        ${CMAKE_SOURCE_DIR}/images/local/libraries/glib-2.0/include
        ${CMAKE_SOURCE_DIR}/images/local/include/glib-2.0
        ${CMAKE_SOURCE_DIR}/images/local/include/pango-1.0
        ${CMAKE_SOURCE_DIR}/images/local/include/cairo
        ${CMAKE_SOURCE_DIR}/images/local/include/freetype2)
    set(PEDIGREE_USER_RPATH_LINK ${CMAKE_SOURCE_DIR}/images/local/libraries)
endif ()

link_directories(${PEDIGREE_USER_LIBRARY_DIRS})
include_directories(${PEDIGREE_USER_INCLUDE_DIRS})

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

    # Using rpath-link option to help find the libraries at link time. At
    # runtime these will be found by the dynamic linker automatically.
    target_link_libraries(app-${name} PRIVATE
        ${TARGET_LIBS} gcc
        -Wl,-rpath-link=${PEDIGREE_USER_RPATH_LINK})
    target_link_options(app-${name} PRIVATE
        -Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}
        -Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE})
    pedigree_use_musl_sdk_startfiles(app-${name})
    if (PEDIGREE_DTRELR_ENABLED)
        target_link_options(app-${name} PRIVATE -Wl,-z,pack-relative-relocs)
    endif ()
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
    target_link_libraries(lib${name} PRIVATE ${libs} gcc)
    target_link_options(lib${name} PRIVATE
        -Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}
        -Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE})
    pedigree_use_musl_sdk_startfiles(lib${name})
    if (PEDIGREE_DTRELR_ENABLED)
        target_link_options(lib${name} PRIVATE -Wl,-z,pack-relative-relocs)
    endif ()
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

pedigree_lib(fb "pedigree-c-user" ${CMAKE_CURRENT_SOURCE_DIR}/libraries/libfb/src/fb.cc)
pedigree_lib(tui "libui;pedigree-c-user"
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

pedigree_app(TUI ON ON OFF "libui;libtui;cairo;${PANGO_LIBS};freetype;stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/TUI/main.cc)
pedigree_app(apptest ON ON OFF "stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/apptest/main.cc)
pedigree_app(config ON ON OFF "stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/config/main.cc)
pedigree_app(crashtest ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/crashtest/main.c)
pedigree_app(display ON OFF OFF-mode "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/display-mode/main.c)
# pedigree_app(fire ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/fire/fire.c)
pedigree_app(gears ON ON OFF "libui;OSMesa;stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/gears/gears.cc)
pedigree_app(gfxcon ON ON OFF "libui;libfb;libtui;cairo;${PANGO_LIBS};freetype;stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/gfxcon/main.cc)
pedigree_app(init ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/init/main.c)
pedigree_app(inotify-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/inotify-test/main.c)
pedigree_app(ipc-test-client ON ON OFF "stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-test-client/main.cc)
pedigree_app(ipc-test-server ON ON OFF "stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-test-server/main.cc)
pedigree_app(keymap ON OFF ON ""
    ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/main.c)
target_link_libraries(app-keymap PRIVATE libkeymap)
pedigree_app(live ON OFF ON "intl;dialog;stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/live/live.cc)
pedigree_app(login ON OFF ON "intl" ${CMAKE_CURRENT_SOURCE_DIR}/applications/login/login.c)
pedigree_app(modlist ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/modlist/main.c)
pedigree_app(modload ON OFF ON ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/modload/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/modload/elf.c)
pedigree_app(modunload ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/modunload/main.c)
pedigree_app(mount ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/mount/main.c)
pedigree_app(net-test ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/net-test/net-test.c)
pedigree_app(nyancat ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/nyancat/nyancat.c)
pedigree_app(preloadd ON OFF OFF "pedigree-c-user" ${CMAKE_CURRENT_SOURCE_DIR}/applications/preloadd/main.c)
pedigree_app(ipc-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-contract-test/messages.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-contract-test/semaphores.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-contract-test/mqueues.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ipc-contract-test/shared-memory.c)
pedigree_app(private-signal-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/private-signal-test/main.c)
pedigree_app(signal-timer-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/signal-timer-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/signal-timer-contract-test/signals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/signal-timer-contract-test/timers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/signal-timer-contract-test/clock.c)
pedigree_app(reboot ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/reboot/main.c)
pedigree_app(event-descriptor-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/event-descriptor-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/event-descriptor-contract-test/signalfd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/event-descriptor-contract-test/timerfd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/event-descriptor-contract-test/clock.c)
pedigree_app(vm-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/vm-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/vm-contract-test/remap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/vm-contract-test/remap-file.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/vm-contract-test/remap-shm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/vm-contract-test/residency.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/vm-contract-test/discard.c)

pedigree_app(file-lock-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-lock-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-lock-contract-test/flock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-lock-contract-test/records.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-lock-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-lock-contract-test/blocking.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-lock-contract-test/creation.c)
pedigree_app(file-resize-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-resize-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-resize-contract-test/shared.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-resize-contract-test/private.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-resize-contract-test/seals.c)

pedigree_app(memory-lock-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memory-lock-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memory-lock-contract-test/limits.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memory-lock-contract-test/ranges.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memory-lock-contract-test/managed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memory-lock-contract-test/raw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memory-lock-contract-test/lifetime.c)
pedigree_app(remap-file-pages-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/remap-file-pages-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/remap-file-pages-contract-test/fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/remap-file-pages-contract-test/offsets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/remap-file-pages-contract-test/admission.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/remap-file-pages-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/remap-file-pages-contract-test/locks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/remap-file-pages-contract-test/resize.c)

pedigree_app(xattr-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/values.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/admission.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/permissions.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/concurrency.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/events.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/ext2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/xattr-contract-test/persistence.c)

pedigree_app(fanotify-handle-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/handles.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/admission.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/events.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/queue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/waits.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/fanotify-handle-contract-test/persistence.c)

pedigree_app(memfd-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memfd-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memfd-contract-test/creation.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memfd-contract-test/seals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memfd-contract-test/mappings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memfd-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/memfd-contract-test/races.c)
pedigree_app(uts-namespace-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/uts-namespace-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/uts-namespace-contract-test/fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/uts-namespace-contract-test/names.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/uts-namespace-contract-test/membership.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/uts-namespace-contract-test/proc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/uts-namespace-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/uts-namespace-contract-test/admission.c)

pedigree_app(child-wait-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/selectors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/events.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/copyout.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/errors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/child-wait-contract-test/interruption.c)

pedigree_app(file-sync-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/file-sync-contract-test/main.c)
pedigree_app(global-sync-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/global-sync-contract-test/main.c)
pedigree_app(session-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/session-contract-test/main.c)
pedigree_app(accounting-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/accounting-contract-test/main.c)
pedigree_app(vhangup-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/vhangup-contract-test/main.c)
pedigree_app(quota-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/quota-contract-test/main.c)
pedigree_app(swap-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/swap-contract-test/main.c)
pedigree_app(sysinfo-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/sysinfo-contract-test/main.c)
pedigree_app(personality-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/personality-contract-test/main.c)
pedigree_app(execveat-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/execveat-contract-test/main.c)
if (TARGET init-module-contract-fixture)
    pedigree_app(init-module-contract-test ON OFF OFF ""
        ${CMAKE_CURRENT_SOURCE_DIR}/applications/init-module-contract-test/main.c)
    add_dependencies(app-init-module-contract-test init-module-contract-fixture)
endif ()
pedigree_app(recvmmsg-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/recvmmsg-contract-test/main.c)
pedigree_app(rename-flags-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/rename-flags-contract-test/main.c)
pedigree_app(filesystem-allocation-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/filesystem-allocation-contract-test/main.c)
pedigree_app(clock-adjust-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/clock-adjust-contract-test/main.c)
pedigree_app(module-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/module-contract-test/main.c)

pedigree_app(ptrace-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/tracee.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/registers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/inspect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/signals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/ownership.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/lifecycle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/errors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/sentinel.S)

if (PEDIGREE_ARCH_TARGET STREQUAL "X64")
    # Capture the first resumed instruction before a loader or CRT changes it.
    add_executable(app-ptrace-entry-test
        ${CMAKE_CURRENT_SOURCE_DIR}/applications/ptrace-contract-test/exec-entry.S)
    set_target_properties(app-ptrace-entry-test PROPERTIES
        OUTPUT_NAME "ptrace-entry-test" LINKER_LANGUAGE C)
    target_link_options(app-ptrace-entry-test PRIVATE
        -nostdlib -static -no-pie -Wl,-e,tc_exec_entry
        -Wl,-z,max-page-size=${PEDIGREE_TARGET_PAGE_SIZE}
        -Wl,-z,common-page-size=${PEDIGREE_TARGET_PAGE_SIZE})
    list(APPEND PEDIGREE_APPS $<TARGET_FILE:app-ptrace-entry-test>)
    list(APPEND PEDIGREE_APPS_TARGETS app-ptrace-entry-test)
endif ()

pedigree_app(scheduling-contract-test ON OFF ON ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/placement.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/wakeups.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/lifecycle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/permissions.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/scheduling-contract-test/input.c)

pedigree_app(process-memory-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/process-memory-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/process-memory-contract-test/copies.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/process-memory-contract-test/mappings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/process-memory-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/process-memory-contract-test/credentials.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/process-memory-contract-test/filesystem.c)
pedigree_app(pipe-transfer-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/pipe-transfer-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/pipe-transfer-contract-test/splice.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/pipe-transfer-contract-test/tee.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/pipe-transfer-contract-test/vmsplice.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/pipe-transfer-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/pipe-transfer-contract-test/readiness.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/pipe-transfer-contract-test/concurrency.c)

pedigree_app(transfer-contract-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/transfer-contract-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/transfer-contract-test/file-copy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/transfer-contract-test/offsets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/transfer-contract-test/stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/transfer-contract-test/lifetime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/transfer-contract-test/races.c)
pedigree_app(signal-exec-test ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/signal-exec-test/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/signal-exec-test/signals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/signal-exec-test/exec.c)
pedigree_app(sudo ON OFF ON "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/sudo/main.c)
pedigree_app(syscall-test ON OFF OFF "" ${CMAKE_CURRENT_SOURCE_DIR}/applications/syscall-test/syscall-test.c)
pedigree_app(testsuite ON OFF OFF ""
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/syscall-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/vm-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/usercopy-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/socket-path-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/futex-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/robust-memory-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/fs-mutation-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/fs-persistence-contracts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/dup3.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/epoll-pty.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/exec.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/fs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/mmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/mprotect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/prctl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/process.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/resource.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/scm-rights.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/scm-rights-stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/testsuite/unix-stream-interruption.c)
pedigree_app(thread-test ON OFF OFF "pedigree-c-user;stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/thread-test/main.cc)
pedigree_app(tour ON OFF OFF "intl;dialog;stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/tour/main.cc)
pedigree_app(ttyterm ON ON OFF "stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/ttyterm/ttyterm.cc)
pedigree_app(uitest ON ON OFF "libui;stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/uitest/main.cc)
pedigree_app(winman ON ON OFF "libui;libfb;png;cairo;${PANGO_LIBS};freetype;stdc++"
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/Png.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/objects.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/util.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/applications/winman/winman.cc)
pedigree_app(which ON OFF OFF "stdc++" ${CMAKE_CURRENT_SOURCE_DIR}/applications/which/main.cc)

if (PEDIGREE_BUILD_KEYMAPS)
    pedigree_keymap(KeymapDeDe ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/keymaps/KeymapDeDe)
    pedigree_keymap(KeymapEnUk ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/keymaps/KeymapEnUk)
    pedigree_keymap(KeymapEnUs ${CMAKE_SOURCE_DIR}/src/user/applications/keymap/keymaps/KeymapEnUs)
endif ()

add_custom_target(user-apps
    DEPENDS ${PEDIGREE_APPS_TARGETS} ${PEDIGREE_LIBS_TARGETS})

# Push lib and app list into parent scope.
set(PEDIGREE_APPS ${PEDIGREE_APPS} PARENT_SCOPE)
set(PEDIGREE_APPS_TARGETS ${PEDIGREE_APPS_TARGETS} PARENT_SCOPE)
set(PEDIGREE_LIBS ${PEDIGREE_LIBS} PARENT_SCOPE)
set(PEDIGREE_LIBS_TARGETS ${PEDIGREE_LIBS_TARGETS} PARENT_SCOPE)
set(PEDIGREE_KEYMAPS_TARGETS ${PEDIGREE_KEYMAPS_TARGETS} PARENT_SCOPE)

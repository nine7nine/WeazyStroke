cmake_minimum_required(VERSION 3.18)
project(easystroke-wayland LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# AddressSanitizer + UndefinedBehaviorSanitizer. On by default for dev builds;
# turn off for release packaging with -DENABLE_ASAN=OFF.
option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(INPUT REQUIRED IMPORTED_TARGET libinput libudev libevdev)
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)
# GTK4 + layer-shell only for the standalone overlay renderer (eswl-overlay).
# The core engine and daemon link none of this.
pkg_check_modules(OVERLAY REQUIRED IMPORTED_TARGET gtk4 gtk4-layer-shell-0)
# GTK4 alone for the config GUI (eswl-config).
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
# GIO/GDBus for the system-tray helper (eswl-tray) -- StatusNotifierItem over
# DBus. No GTK; just glib/gio, so the tray stays light.
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

# Core engine: recognition + input backends. Deliberately self-contained --
# only libc/libstdc++ and libinput/libudev. No Boost, no GUI toolkit.
add_library(eswl_core STATIC
    src/stroke.c
    src/gesture.cc
    src/gesture_recognizer.cc
    src/button_map.cc
    src/uinput_injector.cc
    src/libinput_source.cc
    src/json.cc
    src/gesture_config.cc
    src/process.cc
    src/process_overlay.cc
    src/process_tray.cc
    src/keymap.cc
    src/evdev_source.cc
)
target_include_directories(eswl_core PUBLIC src)
target_link_libraries(eswl_core PUBLIC PkgConfig::INPUT PkgConfig::XKB)

add_executable(eswl-daemon src/main.cc)
target_link_libraries(eswl-daemon PRIVATE eswl_core)

# Standalone trail renderer. Talks to the daemon over a pipe; kept out of the
# warnings/ASan loops below since it is mostly GTK glue (GTK headers are noisy
# under -Wpedantic, and ASan reports benign one-shot GTK allocations on exit).
add_executable(eswl-overlay src/overlay_main.cc)
target_link_libraries(eswl-overlay PRIVATE PkgConfig::OVERLAY)

# Config GUI. Reuses the config/JSON layer directly (not the ASan'd eswl_core,
# to avoid a sanitizer mismatch and GTK leak noise).
add_executable(eswl-config src/eswl_config_main.cc src/gesture_config.cc src/json.cc)
target_include_directories(eswl-config PRIVATE src)
target_link_libraries(eswl-config PRIVATE PkgConfig::GTK4)

# System-tray helper. GIO/GDBus only (StatusNotifierItem); no GTK.
add_executable(eswl-tray src/eswl_tray_main.cc)
target_link_libraries(eswl-tray PRIVATE PkgConfig::GIO)

enable_testing()
set(ESWL_TESTS recognition json config keymap trigger_gate multitemplate gate touch_gate)
set(ESWL_APP_TARGETS eswl-daemon)
foreach(t ${ESWL_TESTS})
    add_executable(eswl_${t}_test tests/${t}_test.cc)
    target_link_libraries(eswl_${t}_test PRIVATE eswl_core)
    add_test(NAME ${t} COMMAND eswl_${t}_test)
    list(APPEND ESWL_APP_TARGETS eswl_${t}_test)
endforeach()

# Warnings on our own targets only (not the recycled C core or system libs).
set(ESWL_WARNINGS -Wall -Wextra -Wpedantic)
foreach(tgt ${ESWL_APP_TARGETS})
    target_compile_options(${tgt} PRIVATE ${ESWL_WARNINGS})
endforeach()
# GTK/GIO targets get -Wall -Wextra (no -Wpedantic; their macros are noisy).
foreach(tgt eswl-overlay eswl-config eswl-tray)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
endforeach()
# The C++ engine sources get the warnings; stroke.c is recycled verbatim and
# compiled as C, so keep -Wpedantic off it to avoid noise from upstream style.
target_compile_options(eswl_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>
    $<$<COMPILE_LANGUAGE:C>:-Wall>
)

if(ENABLE_ASAN)
    # ASan instruments globals/heap/stack (catches OOB array reads, use-after-free)
    # and UBSan catches undefined behaviour — including the GUI/overlay, so bugs
    # surface in dev instead of only in optimized release builds. Run the GTK apps
    # with ASAN_OPTIONS=detect_leaks=0 to silence GTK's benign one-shot allocations.
    set(ESWL_SAN -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    foreach(tgt eswl_core ${ESWL_APP_TARGETS} eswl-overlay eswl-config eswl-tray)
        target_compile_options(${tgt} PRIVATE ${ESWL_SAN})
        target_link_options(${tgt} PRIVATE ${ESWL_SAN})
    endforeach()
    message(STATUS "AddressSanitizer + UBSan: ENABLED (incl. GUI/overlay)")
endif()

install(TARGETS eswl-daemon eswl-overlay eswl-config eswl-tray DESTINATION bin/)
install(FILES data/weazystroke.svg DESTINATION share/icons/hicolor/scalable/apps/)
install(FILES data/weazystroke.desktop DESTINATION share/applications/)

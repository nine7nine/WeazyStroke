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

enable_testing()
set(ESWL_TESTS recognition json config keymap trigger_gate)
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
# The C++ engine sources get the warnings; stroke.c is recycled verbatim and
# compiled as C, so keep -Wpedantic off it to avoid noise from upstream style.
target_compile_options(eswl_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>
    $<$<COMPILE_LANGUAGE:C>:-Wall>
)

if(ENABLE_ASAN)
    set(ESWL_SAN -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    foreach(tgt eswl_core ${ESWL_APP_TARGETS})
        target_compile_options(${tgt} PRIVATE ${ESWL_SAN})
        target_link_options(${tgt} PRIVATE ${ESWL_SAN})
    endforeach()
    message(STATUS "AddressSanitizer + UBSan: ENABLED")
endif()

install(TARGETS eswl-daemon eswl-overlay DESTINATION bin/)

cmake_minimum_required(VERSION 3.20)
project(hdrplay C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)

# Three dependency stacks. All ship .pc files via Homebrew / apt.
#   ffmpeg      : libavformat libavcodec libavutil libswscale
#   libplacebo  : libplacebo (>= 6.x; 7.x preferred for current SDL3/HDR APIs)
#   SDL3        : sdl3 (>= 3.2)
pkg_check_modules(AVFORMAT   REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC    REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL     REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE    REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(PLACEBO    REQUIRED IMPORTED_TARGET libplacebo)
pkg_check_modules(SDL3       REQUIRED IMPORTED_TARGET sdl3)

# Vulkan loader / headers. On macOS this comes from MoltenVK via the
# Vulkan SDK (LunarG); on Linux it's the system loader.
find_package(Vulkan REQUIRED)

add_executable(hdrplay
    src/main.c
    src/decoder.c
    src/renderer.c
    src/hud.c
    src/diagnose.c
    src/analyze.c
    src/checks.c
    src/brightness.c
    src/probe.c
    src/stats.c
    src/layout.c
    src/source.c
)

target_link_libraries(hdrplay PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::PLACEBO
    PkgConfig::SDL3
    Vulkan::Vulkan
)

if(APPLE)
    find_library(IOKIT_LIB IOKit REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    find_library(APPSERVICES_LIB ApplicationServices REQUIRED)
    target_link_libraries(hdrplay PRIVATE
        ${IOKIT_LIB} ${COREFOUNDATION_LIB} ${APPSERVICES_LIB})

    # Bake the source tree's bundled ICD path into the binary. The
    # runtime probe in main.c is otherwise exe-relative, which breaks
    # the moment you copy hdrplay somewhere convenient (~/bin, /usr/
    # local/bin) without dragging third_party/ along — the loader then
    # has zero drivers and SDL fails with a confusing "Vulkan
    # Portability library doesn't implement VK_KHR_surface".
    #
    # Not a configure-time existence check on purpose: you may extract
    # MoltenVK-macos.tar after configuring. main.c access()-tests it.
    set(HDRPLAY_ICD_RELPATH
        "third_party/MoltenVK/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json")
    target_compile_definitions(hdrplay PRIVATE
        HDRPLAY_SOURCE_ICD="${CMAKE_CURRENT_SOURCE_DIR}/${HDRPLAY_ICD_RELPATH}")
endif()

target_include_directories(hdrplay PRIVATE src)

# ---------------------------------------------------------------------
# Tests.
#
# probe.c and stats.c are pure computation over in-memory AVFrames — no
# GPU, no window, no media files — so they can be exercised directly.
# The test includes probe.c as a translation unit to reach the statics
# (the EOTFs and the format predicates), which is the cheapest way to
# get exhaustive coverage of the transfer LUT without widening the
# public header for testing's sake.
#
#   cmake --build build && ctest --test-dir build --output-on-failure
enable_testing()

add_executable(test_stats EXCLUDE_FROM_ALL
    tests/test_stats.c
    src/stats.c
)
target_include_directories(test_stats PRIVATE src)
target_link_libraries(test_stats PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
)
if(NOT APPLE)
    target_link_libraries(test_stats PRIVATE m)
endif()

add_executable(test_layout EXCLUDE_FROM_ALL
    tests/test_layout.c
    src/layout.c
)
target_include_directories(test_layout PRIVATE src)
if(NOT APPLE)
    target_link_libraries(test_layout PRIVATE m)
endif()

add_executable(test_source EXCLUDE_FROM_ALL
    tests/test_source.c
    src/source.c
    src/stats.c
    src/probe.c
    src/decoder.c
)
target_include_directories(test_source PRIVATE src)
target_link_libraries(test_source PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
)
if(NOT APPLE)
    target_link_libraries(test_source PRIVATE m)
endif()

add_test(NAME stats  COMMAND test_stats)
add_test(NAME source COMMAND test_source)
add_test(NAME layout COMMAND test_layout)

# ctest won't build EXCLUDE_FROM_ALL targets on its own.
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_stats test_layout test_source)

# `cmake --install build --prefix ~` puts hdrplay on your PATH along
# with the MoltenVK ICD + dylib it needs, so the installed copy is
# self-contained. Beats hand-copying the binary, which also tends to
# go stale. The ICD lands next to the exe, matching the c_sibling
# candidate in main.c's probe.
include(GNUInstallDirs)
install(TARGETS hdrplay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(APPLE)
    set(HDRPLAY_ICD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/MoltenVK/MoltenVK/dynamic/dylib/macOS")
    if(EXISTS "${HDRPLAY_ICD_DIR}/MoltenVK_icd.json")
        install(FILES
            "${HDRPLAY_ICD_DIR}/MoltenVK_icd.json"
            "${HDRPLAY_ICD_DIR}/libMoltenVK.dylib"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

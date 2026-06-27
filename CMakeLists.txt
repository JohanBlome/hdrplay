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
    src/brightness.c
    src/probe.c
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
endif()

target_include_directories(hdrplay PRIVATE src)

cmake_minimum_required(VERSION 3.25)

project(TorrentControl
    VERSION 0.1.0
    DESCRIPTION "Windows torrent creator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TC_BUILD_TESTS "Build unit and integration tests" ON)
option(TC_BUILD_TOOLS "Build developer tools (tc-proof)" ON)
option(TC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TC_ENABLE_SANITIZERS "Instrument project code with ASan (also UBSan on Linux/Unix)" OFF)
option(TC_BUILD_FUZZERS "Build Clang libFuzzer developer targets" OFF)

if(TC_ENABLE_SANITIZERS)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(FATAL_ERROR "MSVC ASan requires RelWithDebInfo or Release (Debug /RTC is incompatible)")
        endif()
        add_compile_options(/fsanitize=address)
        add_link_options(/INCREMENTAL:NO)
    elseif(NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
        message(FATAL_ERROR "TC_ENABLE_SANITIZERS requires Windows MSVC or Linux/Unix GCC/Clang")
    endif()
endif()
if(TC_BUILD_FUZZERS)
    if(NOT TC_ENABLE_SANITIZERS OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "TC_BUILD_FUZZERS requires Clang and TC_ENABLE_SANITIZERS")
    endif()
    # Instrument project libraries as well as each driver, without supplying
    # the fuzzer main to ordinary tests/tools.
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# The Win32/WebView2 host is only meaningful on Windows. Everything else
# (core engine, bridge protocol, tools, tests) is platform independent so it
# can be built and tested headlessly on any platform.
if(WIN32)
    option(TC_BUILD_APP "Build the Win32 + WebView2 application" ON)
else()
    set(TC_BUILD_APP OFF)
endif()

# Pinned Windows toolchain: MSVC 14.44 (compiler 19.44, Visual Studio 2022
# 17.14). Other versions build but are not the tested configuration; CI sets
# TC_REQUIRE_PINNED_TOOLCHAIN=ON so a drifting runner image fails loudly.
option(TC_REQUIRE_PINNED_TOOLCHAIN "Fail when the compiler is not the pinned MSVC" OFF)
set(TC_PINNED_MSVC_VERSION "19.44")
if(MSVC AND NOT CMAKE_CXX_COMPILER_VERSION MATCHES "^${TC_PINNED_MSVC_VERSION}\\.")
    set(_tc_msg "MSVC ${CMAKE_CXX_COMPILER_VERSION} is not the pinned ${TC_PINNED_MSVC_VERSION}.x toolchain")
    if(TC_REQUIRE_PINNED_TOOLCHAIN)
        message(FATAL_ERROR "${_tc_msg}")
    else()
        message(WARNING "${_tc_msg}")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

find_package(LibtorrentRasterbar 2.1.2 EXACT CONFIG REQUIRED)
find_package(nlohmann_json 3.11 CONFIG REQUIRED)

add_subdirectory(src/core)
add_subdirectory(src/service)
add_subdirectory(src/bridge)

if(TC_BUILD_TOOLS)
    add_subdirectory(tools/tc-proof)
endif()

if(TC_BUILD_APP)
    add_subdirectory(src/app/windows)
endif()

if(TC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TC_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

cmake_minimum_required(VERSION 3.16)

project(PaperForge
    VERSION 0.1.0
    DESCRIPTION "PaperForge - structural academic paper editor (V1 core)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# P0-08: sanitizer builds. This is the toolchain the inline-math lifetime
# crash ("double free or corruption") is closed with; the plan requires a
# Linux GCC ASan+UBSan job in CI.
option(PF_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(PF_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
# E-01: warnings-as-errors gate. Off by default so local builds are not
# blocked while the existing warning backlog is cleared.
option(PF_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

if(PF_ENABLE_ASAN OR PF_ENABLE_UBSAN)
    set(_pf_sanitizers "")
    if(PF_ENABLE_ASAN)
        list(APPEND _pf_sanitizers "address")
    endif()
    if(PF_ENABLE_UBSAN)
        list(APPEND _pf_sanitizers "undefined")
    endif()
    string(REPLACE ";" "," _pf_sanitize_flag "${_pf_sanitizers}")
    message(STATUS "Sanitizers enabled: ${_pf_sanitize_flag}")
    add_compile_options(-fsanitize=${_pf_sanitize_flag}
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_pf_sanitize_flag})
endif()

if(PF_WARNINGS_AS_ERRORS)
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

find_package(Threads REQUIRED)

# Locate tectonic for the test suite.
find_program(PF_TECTONIC_EXE tectonic
    HINTS ${CMAKE_SOURCE_DIR}/tools/bin /usr/local/bin
)
set(TECTONIC_BIN "${PF_TECTONIC_EXE}" CACHE STRING "Path to tectonic binary")
message(STATUS "Tectonic: ${TECTONIC_BIN}")

# Enable CTest at the top level so `ctest` in the build dir discovers the
# suites registered by src/app (GUI tests) and tests/ (core tests).
enable_testing()

add_subdirectory(src)
add_subdirectory(tests)

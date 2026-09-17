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

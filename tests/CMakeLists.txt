# Standalone build for the game-independent core and its unit tests.
#   cmake -S tests -B build-tests && cmake --build build-tests && ./build-tests/fwl_core_tests
cmake_minimum_required(VERSION 3.21)
project(fwl_core_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(fwl_core_tests
    test_core.cpp
    ../src/core/InputSequence.cpp
    ../src/core/WindowAnalysis.cpp
    ../src/core/Project.cpp)
if(MSVC)
    target_compile_options(fwl_core_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(fwl_core_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)
endif()

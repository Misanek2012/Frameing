cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)  # Geode v5 static_asserts C++23
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(FrameWindowLab VERSION 0.1.0)

# Game-independent core (unit-tested separately, see tests/CMakeLists.txt)
# plus the Geode/GD integration layer.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp
    src/gd/*.cpp
)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Install the Geode CLI and run `geode sdk install` (v5.11.2 for GD 2.2081).")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

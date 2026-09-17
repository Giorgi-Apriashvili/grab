cmake_minimum_required(VERSION 3.28)

project(grab
    VERSION 0.1.0
    DESCRIPTION "Grab a file or folder from a Linux server by name: ssh+find to locate, rclone to download"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PROJECT_IS_TOP_LEVEL)
    option(GRAB_BUILD_TESTS "Build the grab unit tests (doctest via FetchContent)" ON)
else()
    option(GRAB_BUILD_TESTS "Build the grab unit tests (doctest via FetchContent)" OFF)
endif()

# ---------------------------------------------------------------------------
# Core library: everything except main(), so tests can link it.
# ---------------------------------------------------------------------------
add_library(grab_core STATIC
    src/util.cpp
    src/quote.cpp
    src/ini.cpp
    src/cli.cpp
    src/config.cpp
    src/remote.cpp
    src/listing.cpp
    src/rclone.cpp
    src/process.cpp)

target_include_directories(grab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(grab_core PUBLIC cxx_std_23)
target_compile_definitions(grab_core PUBLIC
    GRAB_VERSION="${PROJECT_VERSION}"
    $<$<PLATFORM_ID:Windows>:UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN>)

if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(grab_core PUBLIC /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(grab_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(grab src/main.cpp)
target_link_libraries(grab PRIVATE grab_core $<$<PLATFORM_ID:Windows>:shell32>)

install(TARGETS grab RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GRAB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

cmake_minimum_required(VERSION 3.28)

project(grab
    VERSION 0.1.0
    DESCRIPTION "Grab a file or folder from a Linux server by name: ssh+find to locate, rclone to download"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Install per user, no admin rights needed: %USERPROFILE%\programs\grab on Windows,
# ~/.local elsewhere. The presets pass the same value; this covers a plain `cmake -S . -B`.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(WIN32)
        set(CMAKE_INSTALL_PREFIX "$ENV{USERPROFILE}/programs/grab" CACHE PATH "Install prefix" FORCE)
    else()
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
    endif()
endif()
file(TO_CMAKE_PATH "${CMAKE_INSTALL_PREFIX}" CMAKE_INSTALL_PREFIX) # forward slashes throughout

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

# ---------------------------------------------------------------------------
# Install: <prefix>/bin/grab.exe plus the commented example config. On Windows the user
# PATH gains <prefix>/bin so `grab` works from any new terminal.
# ---------------------------------------------------------------------------
install(TARGETS grab RUNTIME DESTINATION bin)
install(FILES grab.conf.example DESTINATION .)

if(WIN32)
    option(GRAB_INSTALL_ADD_TO_PATH "On install, add <prefix>/bin to the user PATH" ON)
    if(GRAB_INSTALL_ADD_TO_PATH)
        # \${...} is evaluated at install time, so `cmake --install --prefix X` is honoured.
        install(CODE "
            execute_process(
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                        -File \"${CMAKE_CURRENT_SOURCE_DIR}/cmake/add_to_user_path.ps1\"
                        -Directory \"\${CMAKE_INSTALL_PREFIX}/bin\"
                RESULT_VARIABLE grab_path_rc
                OUTPUT_VARIABLE grab_path_out
                ERROR_VARIABLE grab_path_err)
            string(STRIP \"\${grab_path_out}\${grab_path_err}\" grab_path_msg)
            if(NOT grab_path_rc EQUAL 0)
                message(WARNING \"could not update the user PATH: \${grab_path_msg}\")
            else()
                message(STATUS \"\${grab_path_msg}\")
            endif()
        ")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GRAB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

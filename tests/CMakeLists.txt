include(FetchContent)

# doctest is header-only. Its own CMakeLists.txt targets a CMake version that CMake 4 no
# longer accepts, so point SOURCE_SUBDIR at a directory without one: the archive is fetched
# and extracted but never add_subdirectory()'d, and we expose the header ourselves.
FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    SOURCE_SUBDIR header-only-no-cmake)
FetchContent_MakeAvailable(doctest)

add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})

add_executable(grab_tests
    test_main.cpp
    test_util.cpp
    test_quote.cpp
    test_ini.cpp
    test_cli.cpp
    test_config.cpp
    test_remote.cpp
    test_listing.cpp
    test_rclone.cpp
    test_process.cpp
    test_json.cpp
    test_match.cpp
    test_engine.cpp
    test_gui_state.cpp
    test_servers.cpp
    test_update.cpp)

target_link_libraries(grab_tests PRIVATE grab_core doctest::doctest)

add_test(NAME grab_tests COMMAND grab_tests)

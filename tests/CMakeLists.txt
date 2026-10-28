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
    test_existing.cpp
    test_file_type.cpp
    test_gui_state.cpp
    test_servers.cpp
    test_server_ops.cpp
    test_fetch.cpp
    test_folder.cpp
    test_rate.cpp
    test_progress_line.cpp
    test_file_list.cpp
    test_secrets.cpp
    test_schedule.cpp
    test_verify.cpp
    test_browse.cpp
    test_space.cpp
    test_preview.cpp
    test_activity.cpp
    test_local_name.cpp
    test_rclone_encoding.cpp
    test_rclone_flags.cpp
    test_rclone_env.cpp
    test_ui_origin.cpp
    test_update.cpp
    test_ssh.cpp
    test_sftp.cpp
    test_ssh_pool.cpp
    test_ssh_find.cpp
    test_ssh_hash.cpp
    test_ssh_read.cpp
    test_play_stream.cpp
    test_preview_jobs.cpp
    test_body_stream.cpp
    test_fetch_grab.cpp
    test_folder_grab.cpp
    test_lookup_grab.cpp
    test_live_server.cpp)

# OpenSSL's MD5 for the checks' own sums (test_ssh_hash.cpp, test_live_server.cpp) and the local
# hash timing; made-up keys and rclone's obscuring for test_rclone_env.cpp.
target_link_libraries(grab_tests PRIVATE grab_core doctest::doctest OpenSSL::Crypto)
if(WIN32)
    # The programs' manifest (long paths), so the tests see paths as grab does.
    target_sources(grab_tests PRIVATE grab_tests.rc)
    set_source_files_properties(grab_tests.rc PROPERTIES OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/src/grab.manifest")
    target_link_options(grab_tests PRIVATE /MANIFEST:NO)
endif()

add_test(NAME grab_tests COMMAND grab_tests)

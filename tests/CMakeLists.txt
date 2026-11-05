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
    test_queue_view.cpp
    test_speed.cpp
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
    test_rclone_env.cpp
    test_rclone_conf.cpp
    test_rclone_import.cpp
    test_passwords.cpp
    test_server_conf.cpp
    test_ui_origin.cpp
    test_window_size.cpp
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
    test_live_server.cpp
    test_guard.cpp
    test_notices.cpp
    test_ssh_keys.cpp)

# OpenSSL's MD5 for the checks' own sums (test_ssh_hash.cpp, test_live_server.cpp) and the local
# hash timing.
target_link_libraries(grab_tests PRIVATE grab_core doctest::doctest OpenSSL::Crypto)
# grab.conf.example as shipped, compared with the example built into grab (test_config.cpp).
target_compile_definitions(grab_tests PRIVATE GRAB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(WIN32)
    # The programs' manifest (long paths), so the tests see paths as grab does.
    target_sources(grab_tests PRIVATE grab_tests.rc)
    set_source_files_properties(grab_tests.rc PROPERTIES OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/src/grab.manifest")
    target_link_options(grab_tests PRIVATE /MANIFEST:NO)
endif()

add_test(NAME grab_tests COMMAND grab_tests)

# The window's page logic (src/gui/ui) in a headless Microsoft Edge: tests/page/run_page_test.ps1
# with PowerShell 7 (see tests/page/README.md). It builds nothing. Label "page": `ctest -L page`
# runs it alone, `-LE page` leaves it out. Skipped, not failed, without pwsh (found here) or Edge
# (the script exits 77). RUN_SERIAL: its timing checks want the machine to themselves.
if(WIN32)
    find_program(GRAB_PWSH NAMES pwsh DOC "PowerShell 7, for the page-logic test (tests/page)")
    if(GRAB_PWSH)
        add_test(NAME page-logic
            COMMAND "${GRAB_PWSH}" -NoLogo -NoProfile -NonInteractive
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/page/run_page_test.ps1")
        set_tests_properties(page-logic PROPERTIES SKIP_RETURN_CODE 77)
    else()
        message(STATUS "page-logic test: PowerShell 7 (pwsh) not found; CTest will skip it")
        add_test(NAME page-logic COMMAND "${CMAKE_COMMAND}" -E echo
            "page-logic: SKIPPED: PowerShell 7 (pwsh) was not found when the build was configured")
        set_tests_properties(page-logic PROPERTIES SKIP_REGULAR_EXPRESSION "page-logic: SKIPPED")
    endif()
    set_tests_properties(page-logic PROPERTIES LABELS page TIMEOUT 300 RUN_SERIAL TRUE)
endif()

# rclone for the tests only (Windows): what grab is compared with, the rclone 1.75.1 grab 0.20 and
# earlier bundled (the same zip and SHA-256). The import's reader and reveal (test_rclone_env.cpp),
# the key logins against `rclone serve sftp` (test_ssh_keys.cpp) and the live
# `rclone hashsum` comparison (test_ssh_hash.cpp) run it from <build>/test-tools/rclone.exe, which
# is never staged, installed or zipped. To bump: take the zip's line from
# https://downloads.rclone.org/<version>/SHA256SUMS (PGP-signed). OFF for an offline build: those
# tests then skip (a failed download fails the configure, URL_HASH).
if(WIN32)
    option(GRAB_TEST_TOOLS "Download rclone for the tests into <build>/test-tools" ON)
endif()
if(WIN32 AND GRAB_TEST_TOOLS)
    FetchContent_Declare(rclone
        URL https://downloads.rclone.org/v1.75.1/rclone-v1.75.1-windows-amd64.zip
        URL_HASH SHA256=200EB602C126D82AA38B51E0F6B9AE837473FF99B51278D3F6F837574C494D6E
        SOURCE_SUBDIR no-cmake-here)
    FetchContent_MakeAvailable(rclone)
    set(GRAB_TEST_RCLONE "${CMAKE_BINARY_DIR}/test-tools/rclone.exe")
    add_custom_command(OUTPUT "${GRAB_TEST_RCLONE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test-tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${rclone_SOURCE_DIR}/rclone.exe" "${GRAB_TEST_RCLONE}"
        DEPENDS "${rclone_SOURCE_DIR}/rclone.exe"
        COMMENT "Copying rclone v1.75.1 for the tests into test-tools"
        VERBATIM)
    add_custom_target(grab_test_tools ALL DEPENDS "${GRAB_TEST_RCLONE}")
    add_dependencies(grab_tests grab_test_tools)
endif()

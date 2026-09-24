cmake_minimum_required(VERSION 3.28)

project(grab
    VERSION 0.3.0
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
    src/match.cpp
    src/listing.cpp
    src/rclone.cpp
    src/process.cpp
    src/json.cpp
    src/update.cpp
    src/engine.cpp
    src/gui_state.cpp
    src/servers.cpp
    src/server_ops.cpp
    src/conn_budget.cpp
    src/fetch.cpp
    src/folder.cpp)

target_include_directories(grab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# bcrypt: SHA-256 for verifying `grab update` downloads (Windows CNG, part of the OS).
target_link_libraries(grab_core PUBLIC $<$<PLATFORM_ID:Windows>:bcrypt>)
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
add_executable(grab src/main.cpp src/server_cli.cpp)
target_link_libraries(grab PRIVATE grab_core $<$<PLATFORM_ID:Windows>:shell32>)

if(WIN32)
    # Version info (Properties, Task Manager, notifications) for both programs; each .rc
    # includes the configured <build>/version.rc.
    configure_file(cmake/version.rc.in "${CMAKE_BINARY_DIR}/version.rc" @ONLY)
    target_sources(grab PRIVATE src/grab.rc)
    target_include_directories(grab PRIVATE "${CMAKE_BINARY_DIR}")
    set_source_files_properties(src/grab.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/version.rc;${CMAKE_CURRENT_SOURCE_DIR}/src/gui/grab.ico")
endif()

# ---------------------------------------------------------------------------
# Bundled rclone (Windows): a pinned, checksum-verified rclone.exe ships next to grab.exe,
# and grab prefers it over any rclone on PATH. rclone is MIT-licensed; its notice ships as
# licenses/rclone.txt. To bump: change the version, take the zip's line from
# https://downloads.rclone.org/<version>/SHA256SUMS (PGP-signed) and update the hash.
# ---------------------------------------------------------------------------
if(WIN32)
    option(GRAB_BUNDLE_RCLONE "Ship a pinned rclone.exe next to grab.exe" ON)
endif()
if(WIN32 AND GRAB_BUNDLE_RCLONE)
    include(FetchContent)
    set(GRAB_RCLONE_VERSION v1.75.1)
    FetchContent_Declare(rclone
        URL https://downloads.rclone.org/${GRAB_RCLONE_VERSION}/rclone-${GRAB_RCLONE_VERSION}-windows-amd64.zip
        URL_HASH SHA256=200EB602C126D82AA38B51E0F6B9AE837473FF99B51278D3F6F837574C494D6E
        SOURCE_SUBDIR no-cmake-here)
    FetchContent_MakeAvailable(rclone)
    set(GRAB_RCLONE_EXE "${rclone_SOURCE_DIR}/rclone.exe")

    # Next to the built grab.exe and grab-gui.exe, so development builds use it too.
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/rclone.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GRAB_RCLONE_EXE}" "${CMAKE_BINARY_DIR}/rclone.exe"
        DEPENDS "${GRAB_RCLONE_EXE}"
        COMMENT "Bundling rclone ${GRAB_RCLONE_VERSION}"
        VERBATIM)
    add_custom_target(bundled_rclone ALL DEPENDS "${CMAKE_BINARY_DIR}/rclone.exe")
    add_dependencies(grab bundled_rclone)
    install(PROGRAMS "${GRAB_RCLONE_EXE}" DESTINATION bin)
    install(FILES licenses/rclone.txt DESTINATION licenses)
endif()

# ---------------------------------------------------------------------------
# Install: <prefix>/bin/grab.exe plus the commented example config. On Windows the user
# PATH gains <prefix>/bin so `grab` works from any new terminal.
# ---------------------------------------------------------------------------
install(TARGETS grab RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# GUI (Windows): grab-gui.exe, a WebView2 window over the same engine.
# ---------------------------------------------------------------------------
if(WIN32)
    option(GRAB_BUILD_GUI "Build grab-gui.exe (WebView2)" ON)
endif()
if(WIN32 AND GRAB_BUILD_GUI)
    include(FetchContent)
    # The WebView2 SDK NuGet package is a zip: headers plus a static loader library, so no
    # WebView2Loader.dll has to ship next to the exe.
    set(GRAB_WEBVIEW2_VERSION 1.0.4191.47)
    FetchContent_Declare(webview2
        URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${GRAB_WEBVIEW2_VERSION}/microsoft.web.webview2.${GRAB_WEBVIEW2_VERSION}.nupkg
        URL_HASH SHA256=F492BBF547D0DA329553B6727435B677579B1E9F91CC9E4A1AD029366D5F23D0
        DOWNLOAD_NAME webview2-${GRAB_WEBVIEW2_VERSION}.zip
        SOURCE_SUBDIR no-cmake-here)
    FetchContent_MakeAvailable(webview2)
    set(GRAB_WEBVIEW2_ARCH x64)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(GRAB_WEBVIEW2_ARCH x86)
    endif()

    set(GRAB_GUI_UI_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/ui/index.html
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/ui/app.css
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/ui/app.js
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/grab.ico)
    add_executable(grab-gui WIN32
        src/gui/main_gui.cpp
        src/gui/app.cpp
        src/gui/tray.cpp
        src/gui/webview.cpp
        src/gui/resources.cpp
        src/gui/grab-gui.rc)
    # The .rc embeds the UI files and the icon; rebuild it whenever one of them changes.
    set_source_files_properties(src/gui/grab-gui.rc PROPERTIES
        OBJECT_DEPENDS "${GRAB_GUI_UI_FILES};${CMAKE_BINARY_DIR}/version.rc")
    target_include_directories(grab-gui PRIVATE src/gui "${CMAKE_BINARY_DIR}")
    target_include_directories(grab-gui SYSTEM PRIVATE ${webview2_SOURCE_DIR}/build/native/include)
    target_link_libraries(grab-gui PRIVATE
        grab_core
        ${webview2_SOURCE_DIR}/build/native/${GRAB_WEBVIEW2_ARCH}/WebView2LoaderStatic.lib
        shlwapi ole32 oleaut32 shell32 dwmapi version advapi32 user32)
    install(TARGETS grab-gui RUNTIME DESTINATION bin)
endif()
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
# Release artifacts: `cmake --build --preset <p> --target dist` writes to <build>/dist/
#   grab-<version>-windows-x64.zip        portable: grab.exe, grab-gui.exe, rclone.exe, example
#                                         config, README, LICENSE
#   grab-<version>-windows-x64-setup.exe  Inno Setup per-user installer, when ISCC is available
# ---------------------------------------------------------------------------
if(WIN32)
    set(GRAB_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
    set(GRAB_STAGE_DIR "${GRAB_DIST_DIR}/stage")
    set(GRAB_ZIP "${GRAB_DIST_DIR}/grab-${PROJECT_VERSION}-windows-x64.zip")
    set(GRAB_EXTRA_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/grab.conf.example"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    # Inno Setup wants native paths; the script is generated from installer/grab.iss.in.
    file(TO_NATIVE_PATH "${GRAB_DIST_DIR}" GRAB_DIST_DIR_NATIVE)
    file(TO_NATIVE_PATH "${GRAB_STAGE_DIR}" GRAB_STAGE_DIR_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/grab.ico" GRAB_ICON_NATIVE)
    configure_file(installer/grab.iss.in "${CMAKE_BINARY_DIR}/installer/grab.iss" @ONLY)

    find_program(GRAB_ISCC NAMES iscc ISCC
        PATHS "C:/Program Files (x86)/Inno Setup 6"
              "$ENV{ProgramFiles}/Inno Setup 6"
              "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
        DOC "Inno Setup compiler (ISCC.exe); enables the installer half of the dist target")

    # A release is always the full set, which `grab update` relies on (GRAB_RELEASE_PROGRAMS).
    if(NOT GRAB_BUNDLE_RCLONE OR NOT GRAB_BUILD_GUI)
        message(STATUS "dist: needs GRAB_BUNDLE_RCLONE and GRAB_BUILD_GUI, so the dist target is not available")
    else()
    set(GRAB_STAGED
        "${GRAB_STAGE_DIR}/grab.exe"
        "${GRAB_STAGE_DIR}/grab-gui.exe"
        "${GRAB_STAGE_DIR}/rclone.exe"
        "${GRAB_STAGE_DIR}/licenses/rclone.txt"
        "${GRAB_STAGE_DIR}/grab.conf.example"
        "${GRAB_STAGE_DIR}/README.md"
        "${GRAB_STAGE_DIR}/LICENSE")
    add_custom_command(OUTPUT ${GRAB_STAGED}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GRAB_STAGE_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:grab>" "$<TARGET_FILE:grab-gui>" "${GRAB_RCLONE_EXE}"
                ${GRAB_EXTRA_FILES} "${GRAB_STAGE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/licenses/rclone.txt"
                "${GRAB_STAGE_DIR}/licenses/rclone.txt"
        DEPENDS grab grab-gui "${GRAB_RCLONE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/licenses/rclone.txt"
                ${GRAB_EXTRA_FILES}
        COMMENT "Staging release files"
        VERBATIM)
    add_custom_command(OUTPUT "${GRAB_ZIP}"
        COMMAND ${CMAKE_COMMAND} -E tar cf "${GRAB_ZIP}" --format=zip --
                grab.exe grab-gui.exe rclone.exe licenses/rclone.txt grab.conf.example README.md LICENSE
        WORKING_DIRECTORY "${GRAB_STAGE_DIR}"
        DEPENDS ${GRAB_STAGED}
        COMMENT "Packing ${GRAB_ZIP}"
        VERBATIM)
    add_custom_target(dist DEPENDS "${GRAB_ZIP}")

    if(GRAB_ISCC)
        add_custom_command(TARGET dist POST_BUILD
            COMMAND "${GRAB_ISCC}" /Q "${CMAKE_BINARY_DIR}/installer/grab.iss"
            COMMENT "Building the Inno Setup installer into ${GRAB_DIST_DIR}"
            VERBATIM)
    else()
        add_custom_command(TARGET dist POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                    "dist: Inno Setup (ISCC.exe) not found, installer skipped; zip written to ${GRAB_DIST_DIR}"
            VERBATIM)
    endif()
    endif() # GRAB_BUNDLE_RCLONE and GRAB_BUILD_GUI
endif()

# The programs a release ships next to grab.exe. `grab update` completes a portable folder
# that lacks one (a v0.2.0 updater only extracted grab.exe).
set(GRAB_RELEASE_PROGRAMS grab.exe)
if(WIN32 AND GRAB_BUNDLE_RCLONE)
    list(APPEND GRAB_RELEASE_PROGRAMS rclone.exe)
endif()
if(WIN32 AND GRAB_BUILD_GUI)
    list(APPEND GRAB_RELEASE_PROGRAMS grab-gui.exe)
endif()
list(JOIN GRAB_RELEASE_PROGRAMS "," GRAB_RELEASE_PROGRAMS_CSV)
set_property(SOURCE src/update.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS "GRAB_RELEASE_PROGRAMS=\"${GRAB_RELEASE_PROGRAMS_CSV}\"")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GRAB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

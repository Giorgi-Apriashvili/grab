# Windows builds take OpenSSL from vcpkg in manifest mode (vcpkg.json, vcpkg-configuration.json):
# the vcpkg at VCPKG_ROOT, else the one Visual Studio ships (its "vcpkg package manager"
# component), so the presets keep working from a plain shell. Included before project(), where
# the toolchain file has to be set. The first configure builds OpenSSL (several minutes); vcpkg
# keeps the result in its binary cache (%LOCALAPPDATA%\vcpkg\archives) for every later build.
if(NOT CMAKE_HOST_WIN32 OR DEFINED CMAKE_TOOLCHAIN_FILE)
    return()
endif()

set(_grab_vcpkg "$ENV{VCPKG_ROOT}")
if(NOT _grab_vcpkg)
    set(_grab_pf86 "ProgramFiles(x86)")
    set(_grab_vswhere "$ENV{${_grab_pf86}}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(EXISTS "${_grab_vswhere}")
        execute_process(COMMAND "${_grab_vswhere}" -latest -products * -property installationPath
                        OUTPUT_VARIABLE _grab_vs OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_grab_vs AND EXISTS "${_grab_vs}/VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
            set(_grab_vcpkg "${_grab_vs}/VC/vcpkg")
        endif()
    endif()
endif()
if(NOT _grab_vcpkg)
    message(FATAL_ERROR "grab needs vcpkg for OpenSSL: install Visual Studio's \"vcpkg package manager\" "
                        "component, or set VCPKG_ROOT to a vcpkg checkout.")
endif()

file(TO_CMAKE_PATH "${_grab_vcpkg}" _grab_vcpkg)
set(CMAKE_TOOLCHAIN_FILE "${_grab_vcpkg}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg's toolchain file")
# Static libraries with the static C runtime (/MT, /MTd), as grab's own presets build.
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg triplet")

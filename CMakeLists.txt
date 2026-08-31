# Windows build harness for the NDI Output OFX plugin (spec decision 8:
# CMake + VS2022; docs/windows-port-spec.md). The Makefile remains the macOS
# harness; BUILD.md is the contract between the two.
#
# CPU-only for now — the CUDA module (ticket #22) will add
# enable_language(CUDA) and the kernel sources behind the same GPU-module
# contract the Metal module implements.
#
# Configure + build (see BUILD.md, Windows section):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#         -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
#   cmake --install build --config Release --prefix stage

cmake_minimum_required(VERSION 3.21)

# zlib must link STATICALLY. The plugin is a DLL living in a folder Windows
# never searches when resolving its load-time imports, so any dynamic
# dependency staged beside it (vcpkg's default z.dll) makes the whole plugin
# fail to load inside Resolve - the same loader trap the delay-loaded NDI DLL
# exists to dodge. zlib can simply be static; the NDI runtime cannot (the SDK
# license requires shipping the vendor DLL). static-md keeps the CRT dynamic,
# matching the /MD default of the rest of the build. Must be set before
# project() for the vcpkg toolchain to see it.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()

# C is enabled for the generated NDI stub (ndi_stub.c, CI mode below).
project(NDIOutput LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "CMake drives the Windows build only; use the Makefile on macOS "
        "(see BUILD.md).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Version guard -----------------------------------------------------------
# VERSION and the #defines in src/NDIOutputPlugin.cpp are synced by
# scripts/set_version.sh / increment_version.sh; the source is authoritative
# for the compiled plugin (defining kPluginVersionString here as well was the
# old scaffold's redefinition collision). Fail configure on drift instead.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" NDI_VERSION_FILE)
string(STRIP "${NDI_VERSION_FILE}" NDI_VERSION_FILE)
file(STRINGS "${CMAKE_SOURCE_DIR}/src/NDIOutputPlugin.cpp" NDI_VERSION_LINE
     REGEX "#define kPluginVersionString")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _unused "${NDI_VERSION_LINE}")
set(NDI_VERSION_SOURCE "${CMAKE_MATCH_1}")
if(NOT NDI_VERSION_FILE STREQUAL NDI_VERSION_SOURCE)
    message(FATAL_ERROR
        "VERSION file says ${NDI_VERSION_FILE} but src/NDIOutputPlugin.cpp "
        "defines kPluginVersionString ${NDI_VERSION_SOURCE}. Run "
        "scripts/set_version.sh to re-sync (never edit either by hand).")
endif()

# --- Dependencies ------------------------------------------------------------
# zlib via vcpkg (spec decision 8): pass the vcpkg toolchain file; vcpkg.json
# pins the dependency list.
find_package(ZLIB REQUIRED)

# NDI Advanced SDK: use the real installation when present (required for any
# binary that will actually stream; release builds must use an SDK <= 30 days
# old). Without it — hosted CI, where the access-gated SDK cannot live — fall
# back to the vendored MIT-licensed headers plus a stub import library built
# from the checked-in export list, which proves the plugin compiles and links
# without shipping a byte of NDI code.
set(NDI_SDK_PATH "C:/Program Files/NDI/NDI 6 Advanced SDK"
    CACHE PATH "Path to the NDI Advanced SDK")
set(NDI_DLL_NAME "Processing.NDI.Lib.Advanced.x64")

if(EXISTS "${NDI_SDK_PATH}/Include/Processing.NDI.Lib.h")
    message(STATUS "NDI: using installed Advanced SDK at ${NDI_SDK_PATH}")
    set(NDI_INCLUDE_DIR "${NDI_SDK_PATH}/Include")
    set(NDI_LINK_LIB "${NDI_SDK_PATH}/Lib/x64/${NDI_DLL_NAME}.lib")
    set(NDI_USING_STUB FALSE)
else()
    message(STATUS "NDI: SDK not found - linking against the stub import library (CI mode)")
    set(NDI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/ndi/include")
    set(NDI_USING_STUB TRUE)

    # Generate one empty function per exported name; the .def attached to the
    # stub target turns them into exports, and linking the target gives the
    # plugin a normal import library for ${NDI_DLL_NAME}.dll.
    set(NDI_DEF_FILE "${CMAKE_SOURCE_DIR}/third_party/ndi/${NDI_DLL_NAME}.def")
    file(STRINGS "${NDI_DEF_FILE}" NDI_DEF_LINES)
    set(NDI_STUB_SOURCE "// Generated from ${NDI_DLL_NAME}.def - do not edit.\n")
    set(NDI_IN_EXPORTS FALSE)
    foreach(_line IN LISTS NDI_DEF_LINES)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "EXPORTS")
            set(NDI_IN_EXPORTS TRUE)
        elseif(NDI_IN_EXPORTS AND _line MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            string(APPEND NDI_STUB_SOURCE "void ${_line}(void) {}\n")
        endif()
    endforeach()
    file(WRITE "${CMAKE_BINARY_DIR}/ndi_stub.c" "${NDI_STUB_SOURCE}")

    add_library(ndi_stub SHARED "${CMAKE_BINARY_DIR}/ndi_stub.c" "${NDI_DEF_FILE}")
    set_target_properties(ndi_stub PROPERTIES
        OUTPUT_NAME "${NDI_DLL_NAME}"
        PREFIX "")
endif()

# --- Shared compile settings -------------------------------------------------
# /utf-8: sources hold UTF-8 string literals (log emoji, path tests); without
# it MSVC decodes them through the local ANSI code page.
add_compile_options(/W3 /utf-8 /MP)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)

# --- The plugin --------------------------------------------------------------
add_library(NDIOutput SHARED src/NDIOutputPlugin.cpp)
set_target_properties(NDIOutput PROPERTIES
    OUTPUT_NAME "NDIOutput"
    SUFFIX ".ofx"
    PREFIX "")
target_include_directories(NDIOutput PRIVATE
    openfx/include
    src
    "${NDI_INCLUDE_DIR}")
target_link_libraries(NDIOutput PRIVATE ZLIB::ZLIB)

# Delay-load the NDI DLL (spec decision 12): Windows' loader never searches a
# DLL's own folder for load-time imports of another DLL, so a static import
# would make the plugin silently fail to load on machines without an NDI
# runtime. Delay-loading defers resolution to first call; before that call the
# plugin preloads the DLL from its own directory (src/NDIRuntimeLoader.h,
# ticket #21), and test_plugin_delayload below keeps the import delay-loaded
# forever.
if(NDI_USING_STUB)
    target_link_libraries(NDIOutput PRIVATE ndi_stub)
else()
    target_link_libraries(NDIOutput PRIVATE "${NDI_LINK_LIB}")
endif()
target_link_libraries(NDIOutput PRIVATE delayimp)
# /WX: LNK4199 ("/DELAYLOAD ignored; no imports found") is a silent revert to
# a load-time NDI import - exactly the plugin-vanishes failure. Make any
# linker warning on this target fatal so it can never ship quietly.
target_link_options(NDIOutput PRIVATE "/DELAYLOAD:${NDI_DLL_NAME}.dll" "/WX")

# --- Portable unit tests (LEARNINGS.md Tier 0; `make test` on macOS) --------
enable_testing()
set(NDI_PORTABLE_TESTS
    test_render_probe
    test_stream_resolution
    test_stereo_pair
    test_platform_paths
    test_ndi_loader
    test_stmap
    test_brawmap)
foreach(_t IN LISTS NDI_PORTABLE_TESTS)
    add_executable(${_t} tests/${_t}.cpp)
    target_include_directories(${_t} PRIVATE src)
    add_test(NAME ${_t} COMMAND ${_t})
    # Fixture paths in the tests are relative to the repo root.
    set_tests_properties(${_t} PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endforeach()
target_link_libraries(test_stmap PRIVATE ZLIB::ZLIB)
target_link_libraries(test_brawmap PRIVATE ZLIB::ZLIB)

# Delay-load smoke test (Windows-only, so not in the portable list): loads
# the freshly built .ofx with a System32-only import search - what Resolve's
# plugin scanner amounts to - and fails on any load-time import that would
# make the plugin silently vanish from the Effects Library. wmain needs the
# wide entry point.
add_executable(test_plugin_delayload tests/test_plugin_delayload.cpp)
target_include_directories(test_plugin_delayload PRIVATE openfx/include)
target_link_options(test_plugin_delayload PRIVATE "/ENTRY:wmainCRTStartup")
add_test(NAME test_plugin_delayload
         COMMAND test_plugin_delayload "$<TARGET_FILE:NDIOutput>")

# --- Install: the OFX bundle tree (spec decision 16) -------------------------
# NDIOutput.ofx.bundle/Contents/Win64/NDIOutput.ofx, installed under
# C:\Program Files\Common Files\OFX\Plugins (the flat install in the old
# scaffold was wrong). The NDI runtime DLL and its licenses file ride along
# when the real SDK is present (spec decisions 11 and 13); the installer
# ticket (#23) owns end-user packaging.
set(NDI_BUNDLE_BIN_DIR "NDIOutput.ofx.bundle/Contents/Win64")
install(TARGETS NDIOutput RUNTIME DESTINATION "${NDI_BUNDLE_BIN_DIR}")
if(NOT NDI_USING_STUB)
    install(FILES
        "${NDI_SDK_PATH}/Bin/x64/${NDI_DLL_NAME}.dll"
        "${NDI_SDK_PATH}/Bin/x64/Processing.NDI.Lib.Licenses.txt"
        DESTINATION "${NDI_BUNDLE_BIN_DIR}")
endif()

message(STATUS "NDI Output ${NDI_VERSION_FILE} - Windows CPU-only build "
               "(CUDA lands with ticket #22)")

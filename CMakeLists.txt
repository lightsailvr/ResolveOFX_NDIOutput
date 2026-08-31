# Windows build harness for the NDI Output OFX plugin (spec decision 8:
# CMake + VS2022 + CUDA Toolkit 12.9; docs/windows-port-spec.md). The
# Makefile remains the macOS harness; BUILD.md is the contract between the
# two.
#
# The CUDA module (ticket #22) implements the same GPU-module contract the
# Metal module does; -DNDI_ENABLE_CUDA=OFF builds the CPU-only plugin (no
# GPU-native path, no CUDA toolkit needed).
#
# Configure + build (see BUILD.md, Windows section). The -T option points the
# VS generator's CUDA toolset straight at the toolkit — needed whenever the
# CUDA VS integration was never copied into Visual Studio (an admin step the
# toolkit installer performs but a bare nvcc install does not):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#         -T "cuda=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9" ^
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

# --- CUDA (ticket #22; spec decisions 4, 5, 8) -------------------------------
# Architectures (spec decision 8): Pascal + Turing + Ampere as real targets
# plus Ampere PTX so newer GPUs (Ada, Hopper, ...) JIT forward. Must be set
# before enable_language(CUDA).
option(NDI_ENABLE_CUDA "Build the CUDA GPU-native pipeline" ON)
if(NDI_ENABLE_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES "60-real;75-real;86-real;86-virtual")
    endif()
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

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
# it MSVC decodes them through the local ANSI code page. MSVC flags are scoped
# to C/C++ — nvcc rejects them on its own command line — and reach CUDA host
# code via -Xcompiler.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/W3;/utf-8;/MP>")
add_compile_options("$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>")
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)

# Byte-identity flag (ticket #22): the CUDA kernels must produce bytes equal
# to the shared CPU references, so nvcc may not contract the multiply-adds
# MSVC leaves separate (MSVC generates no FMA at the default /arch). The
# kernels are memory-bound; the cost is noise. test_cuda_downscale is the
# enforcement.
set(NDI_CUDA_EXACT_MATH "$<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>")

# --- The plugin --------------------------------------------------------------
add_library(NDIOutput SHARED src/NDIOutputPlugin.cpp)
set_target_properties(NDIOutput PROPERTIES
    OUTPUT_NAME "NDIOutput"
    SUFFIX ".ofx"
    PREFIX "")
if(NDI_ENABLE_CUDA)
    target_sources(NDIOutput PRIVATE src/CudaGPUAcceleration.cu)
    # NDI_HAS_CUDA gates the plugin's GPU-native seam (NDIOutputPlugin.cpp);
    # without it the same source builds the CPU-only plugin.
    target_compile_definitions(NDIOutput PRIVATE NDI_HAS_CUDA)
    target_compile_options(NDIOutput PRIVATE ${NDI_CUDA_EXACT_MATH})
    # cudart must link STATICALLY: a cudart64_*.dll staged beside the plugin
    # would hit the same loader trap as z.dll (Windows never searches the
    # plugin's folder for its load-time imports) and silently empty the
    # Effects Library. test_plugin_delayload enforces this stays true.
    set_target_properties(NDIOutput PROPERTIES CUDA_RUNTIME_LIBRARY Static)
    # cudart_static.lib embeds /DEFAULTLIB:LIBCMT (it is built /MT); in this
    # /MD build that raises LNK4098, which the plugin's /WX would make fatal.
    # Dropping LIBCMT is the standard pairing for cudart_static + /MD — its
    # CRT needs resolve from the dynamic runtime.
    target_link_options(NDIOutput PRIVATE "/NODEFAULTLIB:LIBCMT")
endif()
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

# CUDA kernel-identity test (ticket #22): the fused kernels must be
# byte-identical to the shared CPU references. Compiles the module in
# directly, like test_metal_downscale does on macOS; runs on any machine with
# a CUDA device and skips cleanly without one (hosted CI compiles it, the
# Windows workstation executes it — LEARNINGS.md Tier 0).
if(NDI_ENABLE_CUDA)
    add_executable(test_cuda_downscale
        tests/test_cuda_downscale.cpp
        src/CudaGPUAcceleration.cu)
    target_include_directories(test_cuda_downscale PRIVATE src)
    target_compile_options(test_cuda_downscale PRIVATE ${NDI_CUDA_EXACT_MATH})
    set_target_properties(test_cuda_downscale PROPERTIES CUDA_RUNTIME_LIBRARY Static)
    target_link_options(test_cuda_downscale PRIVATE "/NODEFAULTLIB:LIBCMT") # see the plugin's note
    target_link_libraries(test_cuda_downscale PRIVATE ZLIB::ZLIB)
    add_test(NAME test_cuda_downscale COMMAND test_cuda_downscale)
    set_tests_properties(test_cuda_downscale PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

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

if(NDI_ENABLE_CUDA)
    message(STATUS "NDI Output ${NDI_VERSION_FILE} - Windows build with the "
                   "CUDA GPU-native pipeline (ticket #22)")
else()
    message(STATUS "NDI Output ${NDI_VERSION_FILE} - Windows CPU-only build "
                   "(NDI_ENABLE_CUDA=OFF)")
endif()

cmake_minimum_required(VERSION 3.18)
project(NDIOutput LANGUAGES CXX CUDA)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set CUDA standard
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# Version management
file(READ "VERSION" VERSION_STRING)
string(STRIP "${VERSION_STRING}" VERSION_STRING)

# Platform-specific settings
if(WIN32)
    # Windows-specific settings
    set(NDI_SDK_PATH "C:/Program Files/NDI/NDI 6 Advanced SDK" CACHE PATH "Path to NDI Advanced SDK")
    set(NDI_INCLUDE "${NDI_SDK_PATH}/Include")
    set(NDI_LIB "${NDI_SDK_PATH}/Lib/x64/Processing.NDI.Lib.x64.lib")
    
    # CUDA settings for Windows
    find_package(CUDAToolkit REQUIRED)
    
    # Compiler flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --expt-relaxed-constexpr")
    
    # Define Windows macros
    add_definitions(-D_WIN32 -DWIN32 -D_WINDOWS)
    
elseif(APPLE)
    # macOS-specific settings
    set(NDI_SDK_PATH "/Library/NDI Advanced SDK for Apple" CACHE PATH "Path to NDI Advanced SDK")
    set(NDI_INCLUDE "${NDI_SDK_PATH}/include")
    set(NDI_LIB "${NDI_SDK_PATH}/lib/macOS/libndi_advanced.dylib")
    
    # Compiler flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
    
    # Metal framework
    find_library(METAL_FRAMEWORK Metal)
    find_library(METALKIT_FRAMEWORK MetalKit)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    
else()
    # Linux-specific settings
    set(NDI_SDK_PATH "/usr/local/include/ndi" CACHE PATH "Path to NDI SDK")
    set(NDI_INCLUDE "${NDI_SDK_PATH}")
    set(NDI_LIB "ndi")
    
    # Compiler flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
endif()

# Include directories
include_directories(
    openfx/include
    ${NDI_INCLUDE}
    src
)

# Source files
set(SOURCES
    src/NDIOutputPlugin.cpp
)

# Platform-specific source files
if(WIN32)
    list(APPEND SOURCES src/CudaGPUAcceleration.cu)
elseif(APPLE)
    list(APPEND SOURCES src/MetalGPUAcceleration.mm)
endif()

# Create the plugin library
if(WIN32)
    add_library(NDIOutput SHARED ${SOURCES})
    set_target_properties(NDIOutput PROPERTIES
        SUFFIX ".ofx"
        PREFIX ""
    )
elseif(APPLE)
    add_library(NDIOutput MODULE ${SOURCES})
    set_target_properties(NDIOutput PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "ofx.bundle"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist"
    )
else()
    add_library(NDIOutput SHARED ${SOURCES})
    set_target_properties(NDIOutput PROPERTIES
        SUFFIX ".ofx"
        PREFIX ""
    )
endif()

# Link libraries
if(WIN32)
    target_link_libraries(NDIOutput
        ${NDI_LIB}
        CUDA::cudart
        CUDA::cuda_driver
    )
elseif(APPLE)
    target_link_libraries(NDIOutput
        ${NDI_LIB}
        ${METAL_FRAMEWORK}
        ${METALKIT_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
    )
else()
    target_link_libraries(NDIOutput
        ${NDI_LIB}
        GL
    )
endif()

# Compiler definitions
target_compile_definitions(NDIOutput PRIVATE
    kPluginVersionString="${VERSION_STRING}"
)

# CUDA-specific settings
if(WIN32)
    set_property(TARGET NDIOutput PROPERTY CUDA_SEPARABLE_COMPILATION ON)
    set_property(TARGET NDIOutput PROPERTY CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    
    # Set CUDA architecture (adjust based on target GPUs)
    set_property(TARGET NDIOutput PROPERTY CUDA_ARCHITECTURES "52;61;75;86")
endif()

# Installation
if(WIN32)
    # Windows installation
    install(TARGETS NDIOutput
        DESTINATION "C:/Program Files/Common Files/OFX/Plugins"
    )
elseif(APPLE)
    # macOS installation
    install(TARGETS NDIOutput
        DESTINATION "/Library/OFX/Plugins"
    )
else()
    # Linux installation
    install(TARGETS NDIOutput
        DESTINATION "/usr/OFX/Plugins"
    )
endif()

# Copy NDI library on Windows
if(WIN32)
    # Find NDI runtime DLL
    find_file(NDI_DLL
        NAMES "Processing.NDI.Lib.x64.dll"
        PATHS "${NDI_SDK_PATH}/Bin/x64"
        NO_DEFAULT_PATH
    )
    
    if(NDI_DLL)
        install(FILES ${NDI_DLL}
            DESTINATION "C:/Program Files/Common Files/OFX/Plugins"
        )
    endif()
endif()

# Print configuration summary
message(STATUS "NDI Output Plugin Configuration:")
message(STATUS "  Version: ${VERSION_STRING}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  NDI SDK Path: ${NDI_SDK_PATH}")
if(WIN32)
    message(STATUS "  CUDA Support: Enabled")
    message(STATUS "  CUDA Version: ${CUDAToolkit_VERSION}")
elseif(APPLE)
    message(STATUS "  Metal Support: Enabled")
endif() 
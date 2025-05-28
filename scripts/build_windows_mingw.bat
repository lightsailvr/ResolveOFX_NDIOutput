@echo off
REM NDI Output Plugin Windows Build Script with MinGW and CUDA Support
REM Requires: MinGW-w64, CUDA Toolkit 11.0+, NDI Advanced SDK

echo Building NDI Output Plugin for Windows with MinGW and CUDA support...

REM Set build configuration
echo Setting build variables...
set "BUILD_TYPE=Release"
echo BUILD_TYPE=%BUILD_TYPE%
set "GENERATOR=MinGW Makefiles"
echo GENERATOR=%GENERATOR%
set "PLATFORM=x64"
echo PLATFORM=%PLATFORM%

REM Add MinGW to PATH (assuming MSYS2 installation)
set "PATH=C:\msys64\mingw64\bin;%PATH%"

REM Check for required tools
echo Checking for cmake...
where cmake >nul 2>nul
if not %errorlevel%==0 goto cmake_error

echo Checking for nvcc...
where nvcc >nul 2>nul
if not %errorlevel%==0 goto nvcc_error

echo Checking for gcc...
where gcc >nul 2>nul
if not %errorlevel%==0 goto gcc_error

REM Check for NDI SDK
echo Setting NDI_SDK_PATH...
set "NDI_SDK_PATH=C:\Program Files\NDI\NDI 6 Advanced SDK"
echo NDI_SDK_PATH=%NDI_SDK_PATH%
echo Checking if NDI SDK exists at %NDI_SDK_PATH% ...
if not exist "%NDI_SDK_PATH%" goto ndi_error
echo NDI SDK found.

REM Create build directory
if not exist build mkdir build
cd build

REM Configure with CMake
echo Configuring build with CMake...
cmake .. -G "%GENERATOR%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DNDI_SDK_PATH="%NDI_SDK_PATH%" ^
    -DCMAKE_CUDA_ARCHITECTURES="52;61;75;86" ^
    -DCMAKE_C_COMPILER=gcc ^
    -DCMAKE_CXX_COMPILER=g++

if not %errorlevel%==0 goto cmake_config_error

REM Build the project
echo Building project...
cmake --build . --config %BUILD_TYPE% --parallel

if not %errorlevel%==0 goto build_error

REM Copy to DaVinci Resolve plugins directory (if it exists)
set "RESOLVE_PLUGINS_DIR=C:\Program Files\Blackmagic Design\DaVinci Resolve\OFX\Plugins"
if exist "%RESOLVE_PLUGINS_DIR%" (
    echo Installing to DaVinci Resolve plugins directory...
    copy "NDIOutput.ofx" "%RESOLVE_PLUGINS_DIR%\" >nul
    
    REM Copy NDI DLL if it exists
    if exist "%NDI_SDK_PATH%\Bin\x64\Processing.NDI.Lib.x64.dll" (
        copy "%NDI_SDK_PATH%\Bin\x64\Processing.NDI.Lib.x64.dll" "%RESOLVE_PLUGINS_DIR%\" >nul
    )
    
    echo Plugin installed to DaVinci Resolve
) else (
    echo DaVinci Resolve plugins directory not found
    echo Manual installation required to: "%RESOLVE_PLUGINS_DIR%"
)

REM Copy to common OFX plugins directory
set "OFX_PLUGINS_DIR=C:\Program Files\Common Files\OFX\Plugins"
if not exist "%OFX_PLUGINS_DIR%" mkdir "%OFX_PLUGINS_DIR%"

echo Installing to common OFX plugins directory...
copy "NDIOutput.ofx" "%OFX_PLUGINS_DIR%\" >nul

REM Copy NDI DLL
if exist "%NDI_SDK_PATH%\Bin\x64\Processing.NDI.Lib.x64.dll" (
    copy "%NDI_SDK_PATH%\Bin\x64\Processing.NDI.Lib.x64.dll" "%OFX_PLUGINS_DIR%\" >nul
)

echo.
echo Build completed successfully!
echo Plugin location: %CD%\NDIOutput.ofx
echo.
echo CUDA GPU acceleration is enabled for optimal performance.
echo Make sure you have a CUDA-compatible GPU (GTX 10 series or newer).
echo.

cd ..
pause
goto end

:cmake_error
echo ERROR: CMake not found in PATH
echo Please install CMake and add it to your PATH
exit /b 1

:nvcc_error
echo ERROR: CUDA compiler (nvcc) not found in PATH
echo Please install CUDA Toolkit and add it to your PATH
exit /b 1

:gcc_error
echo ERROR: GCC compiler not found in PATH
echo Please install MinGW-w64 (via MSYS2) and add it to your PATH
exit /b 1

:ndi_error
echo ERROR: NDI Advanced SDK not found at "%NDI_SDK_PATH%"
echo Please install NDI Advanced SDK or set NDI_SDK_PATH environment variable
exit /b 1

:cmake_config_error
echo ERROR: CMake configuration failed
exit /b 1

:build_error
echo ERROR: Build failed
exit /b 1

:end 
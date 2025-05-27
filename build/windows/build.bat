@echo off
REM Windows build script for NDI Output Plugin
REM Run this from Visual Studio Developer Command Prompt

echo Building NDI Output Plugin for Windows...

REM Set paths
set SRC_DIR=..\..\src
set OFX_INCLUDE=..\..\openfx\include
set NDI_INCLUDE=C:\Program Files\NDI\NDI 5 SDK\Include
set NDI_LIB=C:\Program Files\NDI\NDI 5 SDK\Lib\x64\Processing.NDI.Lib.x64.lib

REM Clean previous build
if exist *.obj del *.obj
if exist *.ofx del *.ofx
if exist *.exp del *.exp
if exist *.lib del *.lib

REM Compile the plugin
cl /I"%OFX_INCLUDE%" /I"%NDI_INCLUDE%" /std:c++11 /EHsc /c "%SRC_DIR%\NDIOutputPlugin.cpp"

REM Link the plugin
link NDIOutputPlugin.obj "%NDI_LIB%" /OUT:NDIOutput.ofx /DLL /SUBSYSTEM:WINDOWS

if exist NDIOutput.ofx (
    echo Build successful!
    echo To install, run: build.bat install
) else (
    echo Build failed!
    exit /b 1
)

if "%1"=="install" (
    echo Installing plugin...
    if not exist "C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\Contents\Win64" mkdir "C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\Contents\Win64"
    if not exist "C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\Contents" mkdir "C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\Contents"
    copy NDIOutput.ofx "C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\Contents\Win64\"
    copy Info.plist "C:\Program Files\Common Files\OFX\Plugins\NDIOutput.ofx.bundle\Contents\"
    echo Plugin installed successfully!
) 
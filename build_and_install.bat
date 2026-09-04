@echo off
REM SEI Stamper Plugin - Automated Build and Install Script
REM Configures, builds and stages the plugin under out\obs-studio.
REM
REM Required environment:
REM   OBS_SOURCE_DIR  - OBS Studio source tree with libobs built (e.g. C:\obs-studio)
REM Optional environment:
REM   OBS_BUILD_DIR   - OBS build output (default: %OBS_SOURCE_DIR%\build)
REM   OBS_DEPS_DIR    - OBS deps bundle (default: newest under %OBS_SOURCE_DIR%\.deps;
REM                     must be FFmpeg 8, i.e. windows-deps-2026-05-21 or newer)
REM   CMAKE_GENERATOR - CMake generator (default: Visual Studio 17 2022)

setlocal

echo ================================================
echo SEI Stamper Plugin - Build and Install
echo ================================================
echo.

if not defined OBS_SOURCE_DIR (
    echo ERROR: OBS_SOURCE_DIR is not set. Point it to the OBS Studio source tree, e.g.:
    echo   set OBS_SOURCE_DIR=C:\obs-studio
    pause
    exit /b 1
)
if not defined OBS_BUILD_DIR set "OBS_BUILD_DIR=%OBS_SOURCE_DIR%\build"
if not defined CMAKE_GENERATOR set "CMAKE_GENERATOR=Visual Studio 17 2022"

echo OBS source:  %OBS_SOURCE_DIR%
echo OBS build:   %OBS_BUILD_DIR%
echo Generator:   %CMAKE_GENERATOR%
echo.

REM Check if build directory exists
if not exist "build" (
    echo Creating build directory...
    mkdir build
) else (
    echo Build directory exists, cleaning...
    rmdir /S /Q build
    mkdir build
)

REM Navigate to build directory
cd build

echo.
echo ================================================
echo Step 1: Configuring CMake...
echo ================================================
cmake .. -G "%CMAKE_GENERATOR%" -A x64 -DOBS_SOURCE_DIR="%OBS_SOURCE_DIR%" -DOBS_BUILD_DIR="%OBS_BUILD_DIR%"
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed!
    cd ..
    pause
    exit /b 1
)

echo.
echo ================================================
echo Step 2: Building project (Release)...
echo ================================================
cmake --build . --config Release
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed!
    cd ..
    pause
    exit /b 1
)

echo.
echo ================================================
echo Step 3: Installing to output directory...
echo ================================================
cmake --install . --config Release --prefix ../out/obs-studio
if %ERRORLEVEL% neq 0 (
    echo ERROR: Install failed!
    cd ..
    pause
    exit /b 1
)

REM Copy SRT library to output directory (if exists)
echo.
echo ================================================
echo Step 4: Copying dependencies...
echo ================================================

set SRT_DLL_PATHS=^
"..\srt\_build\Release\srt.dll" ^
"%OBS_BUILD_DIR%\rundir\Release\bin\64bit\srt.dll"

for %%P in (%SRT_DLL_PATHS%) do (
    if exist "%%~P" (
        echo Found SRT library: %%~P
        if not exist "..\out\obs-studio\obs-plugins\64bit" mkdir "..\out\obs-studio\obs-plugins\64bit"
        copy /Y "%%~P" "..\out\obs-studio\obs-plugins\64bit\" >nul
        echo SRT library copied successfully
        goto :srt_done
    )
)
echo WARNING: SRT library not found, receiver may not work
:srt_done

REM Copy locale files
echo.
echo Copying locale files...
xcopy /E /I /Y ..\data\locale ..\out\obs-studio\data\obs-plugins\sei-stamper\locale >nul
if %ERRORLEVEL% neq 0 (
    echo WARNING: Failed to copy locale files
) else (
    echo Locale files copied successfully
)

REM Return to project root
cd ..

echo.
echo ================================================
echo Build Complete!
echo ================================================
echo.
echo Output directory: %CD%\out\obs-studio
echo Plugin DLL: %CD%\out\obs-studio\obs-plugins\64bit\sei-stamper.dll
echo.
echo To install to OBS Studio:
echo   1. Copy contents of out\obs-studio\ to C:\Program Files\obs-studio
echo   2. Administrator privileges required
echo   3. Restart OBS Studio
echo.
echo ================================================
pause

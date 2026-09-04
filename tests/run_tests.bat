@echo off
REM SEI-Stamper unit test runner
REM Compiles and runs test_ntp_sei.c using MSVC (no OBS dependencies)

setlocal

REM Find VS Build Tools
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS_PATH=%%i"

if not defined VS_PATH (
    echo ERROR: Visual Studio with C++ workload not found.
    exit /b 1
)

REM Set up MSVC environment
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

REM Build
cd /d "%~dp0"
echo Compiling test_ntp_sei.c...
cl.exe /nologo /W4 /Fe:test_ntp_sei.exe test_ntp_sei.c >nul 2>&1
if %errorlevel% neq 0 (
    echo Compile failed, retrying with output:
    cl.exe /W4 /Fe:test_ntp_sei.exe test_ntp_sei.c
    exit /b 1
)

REM Run
echo.
"%~dp0test_ntp_sei.exe"
set TEST_EXIT=%errorlevel%

REM Clean up build artifacts
del /q test_ntp_sei.obj 2>nul

exit /b %TEST_EXIT%

@echo off
setlocal enabledelayedexpansion

:: --- Configuration ---
set "BUILD_DIR=build"
set "TARGET_EXE=matrix_player_windows.exe"
set "ARCH_FLAG="

:: --- Argument Parsing ---
:ParseArgs
if "%~1"=="" goto :DoneParsing
if "%~1"=="-h" goto :ShowHelp
if "%~1"=="--help" goto :ShowHelp

if "%~1"=="clean" (
    echo ==========================================
    echo Cleaning previous build...
    echo ==========================================
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Clean finished.
    shift
    goto :ParseArgs
)

if "%~1"=="v3" (
    :: x86-64-v3 equivalent for MSVC (Enables AVX2, FMA3, BMI2)
    set "ARCH_FLAG=-DCMAKE_CXX_FLAGS=/arch:AVX2 -DCMAKE_C_FLAGS=/arch:AVX2"
    echo [Config] Target Microarchitecture: x86-64-v3 (AVX2)
    shift
    goto :ParseArgs
)

if "%~1"=="v4" (
    :: x86-64-v4 equivalent for MSVC (Enables AVX-512)
    set "ARCH_FLAG=-DCMAKE_CXX_FLAGS=/arch:AVX512 -DCMAKE_C_FLAGS=/arch:AVX512"
    echo [Config] Target Microarchitecture: x86-64-v4 (AVX-512)
    shift
    goto :ParseArgs
)

:: Catch unknown arguments
echo Unknown argument: %1
goto :ShowHelp

:DoneParsing

echo ==========================================
echo Locating MSVC Environment (vswhere)
echo ==========================================
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo ERROR: Visual Studio or C++ Build Tools not found via vswhere.
    exit /b 1
)

set "VCVARS_BAT=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS_BAT%" (
    echo ERROR: Found VS at "%VS_PATH%", but vcvars64.bat is missing.
    exit /b 1
)

call "%VCVARS_BAT%" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialize MSVC environment.
    exit /b 1
)

echo.
echo ==========================================
echo Updating Submodules
echo ==========================================
where git >nul 2>&1
if errorlevel 1 (
    echo WARNING: Git not found in PATH. Skipping submodule update.
) else (
    git submodule update --init --recursive
    if errorlevel 1 (
        echo ERROR: Git submodule update failed.
        exit /b 1
    )
)

echo.
echo ==========================================
echo Configuring CMake (Ninja, Release)
echo ==========================================
:: Injected %ARCH_FLAG% to pass optimization parameters to CMake
cmake -G Ninja -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release %ARCH_FLAG%
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo.
echo ==========================================
echo Building Project
echo ==========================================
cmake --build "%BUILD_DIR%" --parallel
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo ==========================================
echo Success! 
echo Output: %BUILD_DIR%\%TARGET_EXE%
echo ==========================================
endlocal
exit /b 0

:ShowHelp
echo Usage: build.bat [clean] [v3 ^| v4] [--help]
echo.
echo Options:
echo   clean    Deletes the build directory before configuring.
echo   v3       Optimizes for x86-64-v3 processors (Enables AVX2 instruction sets).
echo   v4       Optimizes for x86-64-v4 processors (Enables AVX-512 instruction sets).
echo   --help   Shows this help message.
endlocal
exit /b 0
@echo off
setlocal

echo ==========================================
echo Loading MSVC environment (vcvars64)
echo ==========================================
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed. Adjust path if VS BuildTools live elsewhere.
    exit /b 1
)

echo.
echo ==========================================
echo Updating submodules
echo ==========================================
git submodule update --init --recursive
if errorlevel 1 (
    echo ERROR: git submodule update failed.
    exit /b 1
)

echo.
echo ==========================================
echo Configuring CMake (Ninja, Release)
echo ==========================================
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

echo.
echo ==========================================
echo Building
echo ==========================================
cmake --build build
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo ==========================================
echo Done. Output: build\matrix_player_windows.exe
echo ==========================================
endlocal

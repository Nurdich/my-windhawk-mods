@echo off
setlocal

echo ============================================
echo Better File Sizes - Build Script
echo ============================================
echo.

:: Check for Visual Studio
where cl >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: Visual Studio compiler not found.
    echo Please run this from a Visual Studio Developer Command Prompt.
    echo.
    echo Or run one of these first:
    echo   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    echo   "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    exit /b 1
)

:: Check for CMake
where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: CMake not found. Please install CMake.
    exit /b 1
)

:: Create build directory
if not exist build mkdir build
cd build

:: Configure with CMake
echo Configuring with CMake...
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
if %errorlevel% neq 0 (
    echo CMake configuration failed.
    exit /b 1
)

:: Build
echo.
echo Building...
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

echo.
echo ============================================
echo Build successful!
echo ============================================
echo.
echo Output files:
echo   build\bin\BetterFileSizesHook.dll
echo   build\bin\BetterFileSizesInjector.exe
echo.
echo Usage:
echo   1. Copy both files to a folder
echo   2. Run: BetterFileSizesInjector.exe inject
echo   3. Edit BetterFileSizes.ini to configure
echo.

cd ..

@echo off
setlocal

echo ============================================
echo Better File Sizes - Symbol Server Setup
echo ============================================
echo.
echo This script copies the required symbol server DLLs from Windows SDK.
echo.

:: Find Windows SDK path
set SDK_PATH=

:: Try Windows 11 SDK first
if exist "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll" (
    set SDK_PATH=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64
)

:: Try Visual Studio path
if "%SDK_PATH%"=="" (
    for /d %%i in ("C:\Program Files\Microsoft Visual Studio\*") do (
        for /d %%j in ("%%i\*") do (
            if exist "%%j\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\dbghelp.dll" (
                set SDK_PATH=%%j\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer
            )
        )
    )
)

:: Try Windows SDK debuggers
if "%SDK_PATH%"=="" (
    if exist "%ProgramFiles(x86)%\Windows Kits\10\Debuggers\x64\dbghelp.dll" (
        set SDK_PATH=%ProgramFiles(x86)%\Windows Kits\10\Debuggers\x64
    )
)

if "%SDK_PATH%"=="" (
    echo ERROR: Cannot find Windows SDK or Debugging Tools.
    echo.
    echo Please install one of the following:
    echo   1. Windows SDK: https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/
    echo      - During installation, select "Debugging Tools for Windows"
    echo   2. Or download Debugging Tools separately from Microsoft
    echo.
    echo After installation, run this script again.
    pause
    exit /b 1
)

echo Found SDK at: %SDK_PATH%
echo.

:: Get script directory
set SCRIPT_DIR=%~dp0
set BIN_DIR=%SCRIPT_DIR%build\bin

if not exist "%BIN_DIR%" (
    echo Creating output directory: %BIN_DIR%
    mkdir "%BIN_DIR%"
)

echo Copying files...

:: Copy dbghelp.dll
if exist "%SDK_PATH%\dbghelp.dll" (
    copy /Y "%SDK_PATH%\dbghelp.dll" "%BIN_DIR%\"
    echo   Copied dbghelp.dll
) else (
    echo   WARNING: dbghelp.dll not found
)

:: Copy symsrv.dll (required for symbol server)
if exist "%SDK_PATH%\symsrv.dll" (
    copy /Y "%SDK_PATH%\symsrv.dll" "%BIN_DIR%\"
    echo   Copied symsrv.dll
) else (
    echo   WARNING: symsrv.dll not found
)

:: Copy srcsrv.dll (optional, for source server)
if exist "%SDK_PATH%\srcsrv.dll" (
    copy /Y "%SDK_PATH%\srcsrv.dll" "%BIN_DIR%\"
    echo   Copied srcsrv.dll
)

echo.
echo ============================================
echo Setup complete!
echo ============================================
echo.
echo The following files were copied to: %BIN_DIR%
echo   - dbghelp.dll (debug helper library)
echo   - symsrv.dll  (symbol server client)
echo.
echo Now rebuild and run the injector:
echo   build.bat
echo   BetterFileSizesInjector.exe inject
echo.
pause

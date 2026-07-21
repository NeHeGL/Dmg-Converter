@echo off
:: build.bat — Build DMG-Converter (Release, x64)
:: Requires: an MSVC toolchain (Visual Studio 2022 or just the Build Tools) with the
:: C++ workload, plus CMake 3.20+. If neither is found, this script offers to install
:: "Build Tools for Visual Studio 2022" (CLI-only, no IDE) via winget.

setlocal

:: Try to locate vswhere so we can find the VS install automatically
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo No MSVC toolchain found ^(vswhere.exe missing^).
    where winget >nul 2>&1
    if errorlevel 1 (
        echo ERROR: winget is not available either, so this can't be installed automatically.
        echo Install "Build Tools for Visual Studio 2022" manually from:
        echo   https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
        echo Select the "Desktop development with C++" workload, then re-run this script.
        exit /b 1
    )
    echo.
    set /p INSTALL_CONFIRM="Install 'Build Tools for Visual Studio 2022' now via winget? [~2-3GB download] (y/N): "
    if /i not "%INSTALL_CONFIRM%"=="y" (
        echo Aborting. Install it yourself, then re-run this script:
        echo   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        exit /b 1
    )
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --accept-package-agreements --accept-source-agreements --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    if errorlevel 1 ( echo winget install failed. & exit /b 1 )
    if not exist "%VSWHERE%" (
        echo ERROR: Install finished but vswhere.exe still not found. Try a new terminal window and re-run.
        exit /b 1
    )
)

:: Find the VS install path
for /f "usebackq tokens=*" %%i in (
    `"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set "VS_PATH=%%i"

if "%VS_PATH%"=="" (
    echo ERROR: No Visual Studio installation with C++ tools found.
    exit /b 1
)

:: Bootstrap the VS environment (x64 native tools)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:: CMake ships with the VS Build Tools/Visual Studio "C++ CMake tools" component but
:: isn't always on PATH outside a Developer Command Prompt - fall back to the copy
:: bundled under the VS install if the global "cmake" isn't found.
where cmake >nul 2>&1
if errorlevel 1 (
    set "CMAKE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) else (
    set "CMAKE=cmake"
)
if not exist "%CMAKE%" if not "%CMAKE%"=="cmake" (
    echo ERROR: CMake not found. Install "C++ CMake tools for Windows" via the Visual Studio Installer,
    echo or install CMake separately from https://cmake.org/download/
    exit /b 1
)

:: Configure (only needed first time or when CMakeLists.txt changes)
if not exist build\CMakeCache.txt (
    echo Configuring...
    "%CMAKE%" -B build -S . -A x64
    if errorlevel 1 ( echo CMake configure failed. & exit /b 1 )
)

:: Build
echo Building Release...
"%CMAKE%" --build build --config Release
if errorlevel 1 ( echo Build failed. & exit /b 1 )

:: Copy to project root for convenience
copy /Y build\Release\DMG-Converter.exe DMG-Converter.exe >nul
echo.
echo Done!  DMG-Converter.exe is ready.
endlocal

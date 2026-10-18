@echo off

setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ==========================================
echo   Applepie Manager Build Script
echo ==========================================
echo.

:: Set up MSVC environment
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "vcvars="

if exist "%vswhere%" (
    for /f "usebackq tokens=*" %%i in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "install_path=%%i"
    )
)

if defined install_path (
    set "vcvars=!install_path!\VC\Auxiliary\Build\vcvars64.bat"
)

if defined vcvars if exist "!vcvars!" (
    echo [INFO] Found MSVC at: "!vcvars!"
    call "!vcvars!" >nul
)

where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] MSVC not found. Install the C++ desktop workload or use an x64 Developer Command Prompt.
    exit /b 1
)

where cl >nul 2>nul
if %errorlevel% equ 0 (
    echo [SUCCESS] MSVC Environment Ready.
) else (
    echo [ERROR] Failed to set up MSVC environment.
    rem Non-interactive build: failure is reported via exit code.
    exit /b 1
)

echo [OK] MSVC compiler found
echo.

if not exist bin mkdir bin

:: Build applepie_manager.dll
echo [1/4] Compiling version resource ...
rc /nologo /fo bin\version.res src\version.rc
if errorlevel 1 exit /b 1

echo [2/4] Building applepie_manager.dll ...
cl /nologo /utf-8 /O2 /MT /LD /EHsc /std:c++17 ^
    /Ideps\minhook_lib\include ^
    src\applepie_manager.cpp ^
    src\hub_core.cpp ^
    src\hub_shared.cpp src\hub_runtime.cpp src\hub_pointers.cpp src\hub_windows.cpp ^
    src\hub_lifecycle.cpp src\hub_config.cpp ^
    src\manager_loader.cpp ^
    bin\version.res ^
    deps\minhook_lib\lib\libMinHook.x64.lib ^
    user32.lib ^
    version.lib ^
    comctl32.lib ^
    ole32.lib ^
    /Fo"bin\\" ^
    /Fe"bin\applepie_manager.dll" ^
    /link /DLL /INCREMENTAL:NO /LTCG

if %errorlevel% neq 0 (
    echo [ERROR] applepie_manager.dll build failed!
    rem Non-interactive build: failure is reported via exit code.
    exit /b 1
)
echo [OK] applepie_manager.dll built successfully
echo.

:: Build d3dcompiler_47.dll (proxy loader)
echo [3/4] Building d3dcompiler_47.dll (proxy loader) ...
cl /nologo /O2 /MT /LD /EHsc /std:c++17 ^
    src\proxy_d3dcompiler.cpp ^
    /Fo"bin\\" ^
    /Fe"bin\d3dcompiler_47.dll" ^
    /link /DLL /INCREMENTAL:NO

if %errorlevel% neq 0 (
    echo [ERROR] d3dcompiler_47.dll build failed!
    rem Non-interactive build: failure is reported via exit code.
    exit /b 1
)
echo [OK] d3dcompiler_47.dll built successfully
echo.

:: Build vulkan-1.dll (vulkan proxy loader)
echo [4/4] Building vulkan-1.dll (vulkan proxy loader) ...
cl /nologo /O2 /MT /LD /EHsc /std:c++17 ^
    src\proxy_vulkan_full.cpp ^
    /Fo"bin\\" ^
    /Fe"bin\vulkan-1.dll" ^
    /link /DLL /INCREMENTAL:NO

if %errorlevel% neq 0 (
    echo [ERROR] vulkan-1.dll build failed!
    rem Non-interactive build: failure is reported via exit code.
    exit /b 1
)
echo [OK] vulkan-1.dll built successfully
echo.

echo [OK] Manager and both proxies built.
exit /b 0

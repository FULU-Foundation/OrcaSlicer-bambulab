@REM OrcaSlicer build script for Windows with VS auto-detect
@echo off
set WP=%CD%
set _START_TIME=%TIME%

@REM Check for Ninja Multi-Config option (-x)
set USE_NINJA=0
for %%a in (%*) do (
    if "%%a"=="-x" set USE_NINJA=1
)

if "%USE_NINJA%"=="1" (
    echo Using Ninja Multi-Config generator
    set CMAKE_GENERATOR="Ninja Multi-Config"
    set VS_VERSION=Ninja
    goto :generator_ready
)

@REM Detect Visual Studio version using msbuild
echo Detecting Visual Studio version using msbuild...

@REM Try to get MSBuild version - the output format varies by VS version
set VS_MAJOR=
for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
    for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
    set MSBUILD_OUTPUT=%%i
    goto :version_found
)

@REM Alternative method for newer MSBuild versions
if "%VS_MAJOR%"=="" (
    for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
        for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
        set MSBUILD_OUTPUT=%%i
        goto :version_found
    )
)

:version_found
echo MSBuild version detected: %MSBUILD_OUTPUT%
echo Major version: %VS_MAJOR%

if "%VS_MAJOR%"=="" (
    echo Error: Could not determine Visual Studio version from msbuild
    echo Please ensure Visual Studio and MSBuild are properly installed
    exit /b 1
)

if "%VS_MAJOR%"=="16" (
    set VS_VERSION=2019
    set CMAKE_GENERATOR="Visual Studio 16 2019"
) else if "%VS_MAJOR%"=="17" (
    set VS_VERSION=2022
    set CMAKE_GENERATOR="Visual Studio 17 2022"
) else if "%VS_MAJOR%"=="18" (
    set VS_VERSION=2026
    set CMAKE_GENERATOR="Visual Studio 18 2026"
) else (
    echo Error: Unsupported Visual Studio version: %VS_MAJOR%
    echo Supported versions: VS2019 ^(16.x^), VS2022 ^(17.x^), VS2026 ^(18.x^)
    exit /b 1
)

echo Detected Visual Studio %VS_VERSION% (version %VS_MAJOR%)
echo Using CMake generator: %CMAKE_GENERATOR%

:generator_ready

@REM Pack deps
if "%1"=="pack" (
    setlocal ENABLEDELAYEDEXPANSION
    cd %WP%/deps/build
    for /f "tokens=2-4 delims=/ " %%a in ('date /t') do set build_date=%%c%%b%%a
    echo packing deps: OrcaSlicer_dep_win64_!build_date!_vs!VS_VERSION!.zip

    %WP%/tools/7z.exe a OrcaSlicer_dep_win64_!build_date!_vs!VS_VERSION!.zip OrcaSlicer_dep
    goto :done
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON
if "%debug%"=="ON" (
    set build_type=Debug
    set build_dir=build-dbg
) else (
    if "%debuginfo%"=="ON" (
        set build_type=RelWithDebInfo
        set build_dir=build-dbginfo
    ) else (
        set build_type=Release
        set build_dir=build
    )
)
echo build type set to %build_type%

setlocal DISABLEDELAYEDEXPANSION
cd deps
mkdir %build_dir%
cd %build_dir%
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

if "%1"=="slicer" (
    GOTO :slicer
)
echo "building deps.."

echo on
REM Set minimum CMake policy to avoid <3.5 errors
set CMAKE_POLICY_VERSION_MINIMUM=3.5
if "%USE_NINJA%"=="1" (
    cmake ../ -G %CMAKE_GENERATOR% -DCMAKE_BUILD_TYPE=%build_type%
    if errorlevel 1 exit /b 1
    cmake --build . --config %build_type% --target deps
    if errorlevel 1 exit /b 1
) else (
    cmake ../ -G %CMAKE_GENERATOR% -A x64 -DCMAKE_BUILD_TYPE=%build_type%
    if errorlevel 1 exit /b 1
    cmake --build . --config %build_type% --target deps -- -m
    if errorlevel 1 exit /b 1
)
@echo off

if "%1"=="deps" goto :done

:slicer
call :check_linux_bridge_runtime_inputs
if errorlevel 1 exit /b 1

echo "building Orca Slicer..."
cd %WP%
mkdir %build_dir%
cd %build_dir%

echo on
set CMAKE_POLICY_VERSION_MINIMUM=3.5
if "%USE_NINJA%"=="1" (
    cmake .. -G %CMAKE_GENERATOR% -DORCA_TOOLS=ON %SIG_FLAG% -DCMAKE_BUILD_TYPE=%build_type%
    if errorlevel 1 exit /b 1
    cmake --build . --config %build_type% --target ALL_BUILD
    if errorlevel 1 exit /b 1
) else (
    cmake .. -G %CMAKE_GENERATOR% -A x64 -DORCA_TOOLS=ON %SIG_FLAG% -DCMAKE_BUILD_TYPE=%build_type%
    if errorlevel 1 exit /b 1
    cmake --build . --config %build_type% --target ALL_BUILD -- -m
    if errorlevel 1 exit /b 1
)
@echo off
cd ..
call scripts/run_gettext.bat
if errorlevel 1 exit /b 1
cd %build_dir%
cmake --build . --target install --config %build_type%
if errorlevel 1 exit /b 1
call :copy_linux_bridge_runtime
if errorlevel 1 exit /b 1

:done
@echo off
for /f "tokens=1-3 delims=:.," %%a in ("%_START_TIME: =0%") do set /a "_start_s=(1%%a-100)*3600 + (1%%b-100)*60 + (1%%c-100)"
for /f "tokens=1-3 delims=:.," %%a in ("%TIME: =0%") do set /a "_end_s=(1%%a-100)*3600 + (1%%b-100)*60 + (1%%c-100)"
set /a "_elapsed=_end_s - _start_s"
if %_elapsed% lss 0 set /a "_elapsed+=86400"
set /a "_hours=_elapsed / 3600"
set /a "_remainder=_elapsed - _hours * 3600"
set /a "_mins=_remainder / 60"
set /a "_secs=_remainder - _mins * 60"
echo.
echo Build completed in %_hours%h %_mins%m %_secs%s
exit /b 0

:resolve_rootfs_tar
if defined PJARCZAK_ROOTFS_TAR exit /b 0

if defined PJARCZAK_WSL_ROOTFS_TAR (
    if exist "%PJARCZAK_WSL_ROOTFS_TAR%" (
        set "PJARCZAK_ROOTFS_TAR=%PJARCZAK_WSL_ROOTFS_TAR%"
        exit /b 0
    )
    echo Missing file from PJARCZAK_WSL_ROOTFS_TAR: %PJARCZAK_WSL_ROOTFS_TAR%
    exit /b 1
)

if exist "%WP%\tools\pjarczak_bambu_runtime\rootfs\windows-wsl2-rootfs.tar" (
    set "PJARCZAK_ROOTFS_TAR=%WP%\tools\pjarczak_bambu_runtime\rootfs\windows-wsl2-rootfs.tar"
    exit /b 0
)

if exist "%WP%\tools\pjarczak_bambu_runtime\windows-wsl2-rootfs.tar" (
    set "PJARCZAK_ROOTFS_TAR=%WP%\tools\pjarczak_bambu_runtime\windows-wsl2-rootfs.tar"
    exit /b 0
)

echo Missing windows-wsl2-rootfs.tar
echo Expected one of:
echo   %WP%\tools\pjarczak_bambu_runtime\rootfs\windows-wsl2-rootfs.tar
echo   %WP%\tools\pjarczak_bambu_runtime\windows-wsl2-rootfs.tar
echo Or set PJARCZAK_WSL_ROOTFS_TAR to an absolute path.
exit /b 1

:check_linux_bridge_runtime_inputs
set "HOST_RUNTIME_DIR=%WP%\tools\pjarczak_bambu_linux_host\runtime\linux-x86_64"

call :resolve_rootfs_tar
if errorlevel 1 exit /b 1

if not exist "%HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host" (
    echo Missing linux host runtime: %HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host
    echo Build it first on Linux with:
    echo   tools\pjarczak_bambu_linux_host\package_linux_host_runtime.sh
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi1" (
    echo Missing linux host ABI1 runtime: %HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi1
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi0" (
    echo Missing linux host ABI0 runtime: %HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi0
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\ca-certificates.crt" (
    echo Missing CA bundle for linux bridge runtime: %HOST_RUNTIME_DIR%\ca-certificates.crt
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\slicer_base64.cer" (
    echo Missing slicer certificate for linux bridge runtime: %HOST_RUNTIME_DIR%\slicer_base64.cer
    exit /b 1
)

echo Linux bridge preflight OK
echo   host runtime: %HOST_RUNTIME_DIR%
echo   rootfs tar:   %PJARCZAK_ROOTFS_TAR%
exit /b 0

:copy_linux_bridge_runtime
set "INSTALL_DIR=%WP%\%build_dir%\OrcaSlicer"
set "HOST_RUNTIME_DIR=%WP%\tools\pjarczak_bambu_linux_host\runtime\linux-x86_64"

if not defined PJARCZAK_ROOTFS_TAR (
    call :resolve_rootfs_tar
    if errorlevel 1 exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host" (
    echo Missing linux host runtime: %HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host
    echo Build it first on Linux with:
    echo   tools\pjarczak_bambu_linux_host\package_linux_host_runtime.sh
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi1" (
    echo Missing linux host ABI1 runtime: %HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi1
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi0" (
    echo Missing linux host ABI0 runtime: %HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host_abi0
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\ca-certificates.crt" (
    echo Missing CA bundle for linux bridge runtime: %HOST_RUNTIME_DIR%\ca-certificates.crt
    exit /b 1
)

if not exist "%HOST_RUNTIME_DIR%\slicer_base64.cer" (
    echo Missing slicer certificate for linux bridge runtime: %HOST_RUNTIME_DIR%\slicer_base64.cer
    exit /b 1
)

if not exist "%INSTALL_DIR%" (
    echo Missing install directory: %INSTALL_DIR%
    exit /b 1
)

if not exist "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" (
    if exist "%WP%\%build_dir%\pjarczak_bambu_networking_bridge.dll" (
        copy /Y "%WP%\%build_dir%\pjarczak_bambu_networking_bridge.dll" "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" >nul
    )
)

if not exist "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" (
    if exist "%WP%\%build_dir%\src\%build_type%\pjarczak_bambu_networking_bridge.dll" (
        copy /Y "%WP%\%build_dir%\src\%build_type%\pjarczak_bambu_networking_bridge.dll" "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" >nul
    )
)

if not exist "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" (
    echo Missing bridge DLL in install output: %INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll
    exit /b 1
)

xcopy "%HOST_RUNTIME_DIR%\*" "%INSTALL_DIR%\\" /I /Y >nul
if errorlevel 4 (
    echo Failed to copy flat linux host runtime files into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%PJARCZAK_ROOTFS_TAR%" "%INSTALL_DIR%\windows-wsl2-rootfs.tar" >nul
if errorlevel 1 (
    echo Failed to copy windows-wsl2-rootfs.tar into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%WP%\tools\pjarczak_bambu_runtime\wsl\pjarczak_wsl_run_host.sh" "%INSTALL_DIR%\pjarczak_wsl_run_host.sh" >nul
if errorlevel 1 (
    echo Failed to copy pjarczak_wsl_run_host.sh into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%WP%\tools\pjarczak_bambu_runtime\wsl\install_runtime.ps1" "%INSTALL_DIR%\install_runtime.ps1" >nul
if errorlevel 1 (
    echo Failed to copy install_runtime.ps1 into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%WP%\tools\pjarczak_bambu_runtime\wsl\install_runtime.cmd" "%INSTALL_DIR%\install_runtime.cmd" >nul
if errorlevel 1 (
    echo Failed to copy install_runtime.cmd into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%WP%\tools\pjarczak_bambu_runtime\wsl\verify_runtime.ps1" "%INSTALL_DIR%\verify_runtime.ps1" >nul
if errorlevel 1 (
    echo Failed to copy verify_runtime.ps1 into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%WP%\tools\pjarczak_bambu_runtime\wsl\pjarczak_wsl_distro.txt" "%INSTALL_DIR%\pjarczak_wsl_distro.txt" >nul
if errorlevel 1 (
    echo Failed to copy pjarczak_wsl_distro.txt into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%WP%\tools\pjarczak_bambu_runtime\wsl\pjarczak_plugin_cache_subdir.txt" "%INSTALL_DIR%\pjarczak_plugin_cache_subdir.txt" >nul
if errorlevel 1 (
    echo Failed to copy pjarczak_plugin_cache_subdir.txt into %INSTALL_DIR%
    exit /b 1
)

copy /Y "%WP%\tools\pjarczak_bambu_runtime\release\assemble_windows_runtime_bundle.ps1" "%INSTALL_DIR%\assemble_windows_runtime_bundle.ps1" >nul
if errorlevel 1 (
    echo Failed to copy assemble_windows_runtime_bundle.ps1 into %INSTALL_DIR%
    exit /b 1
)

exit /b 0

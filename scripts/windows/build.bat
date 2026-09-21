@echo off
setlocal enabledelayedexpansion

rem ============================================================================
rem scripts\windows\build.bat
rem
rem Incremental out-of-tree qmake build of libtorrent_peer_collector_qt.
rem
rem Usage:
rem     scripts\windows\build.bat [release or debug]
rem
rem Release builds land in build\, debug builds in build-debug\, so the two
rem configurations never share object files. Run it from anywhere or double
rem click it; it always builds the project it lives in.
rem
rem The MinGW bin folder is put on PATH for the build, so the Qt bin and Tools
rem folders do not have to be on PATH before the script is started. See
rem mingw_env.bat.
rem
rem libtorrent: the .pro file links libtorrent-rasterbar through pkg-config
rem (PKGCONFIG += libtorrent-rasterbar). On Windows that means a MinGW build
rem of libtorrent with its .pc file, for example from MSYS2
rem (pacman -S mingw-w64-x86_64-libtorrent-rasterbar) or vcpkg, and
rem pkg-config.exe on PATH. Set PKG_CONFIG_PATH to the folder holding
rem libtorrent-rasterbar.pc if pkg-config does not find it on its own.
rem
rem Environment overrides:
rem     QMAKE            qmake executable   (default: qmake6.exe, then qmake.exe in PATH)
rem     MAKE             make executable    (default: mingw32-make.exe, then make.exe)
rem     MINGW_BIN        MinGW bin folder   (default: the folder of MAKE, then g++.exe
rem                                          in PATH, then C:\Qt\Tools\mingw1310_64\bin)
rem     PKG_CONFIG       pkg-config executable (default: pkg-config.exe in PATH)
rem     PKG_CONFIG_PATH  extra folders with .pc files
rem     JOBS             parallel make jobs (default: NUMBER_OF_PROCESSORS)
rem ============================================================================

pushd "%~dp0..\.."
if errorlevel 1 (
    echo ERROR: failed to locate the project folder.
    exit /b 1
)
set "PROJECT_ROOT=%CD%"
popd

set "PROJECT_FILE=%PROJECT_ROOT%\libtorrent_peer_collector_qt.pro"
if not exist "%PROJECT_FILE%" (
    echo ERROR: project file not found:
    echo "%PROJECT_FILE%"
    exit /b 1
)

set "BUILD_CONFIG=%~1"
if "%BUILD_CONFIG%"=="" set "BUILD_CONFIG=release"

rem The configuration is written back in lower case because qmake CONFIG
rem values are case sensitive, while the comparisons below are not.
if /i "%BUILD_CONFIG%"=="release" (
    set "BUILD_CONFIG=release"
    set "BUILD_DIR=%PROJECT_ROOT%\build"
    set "OTHER_CONFIG=debug"
) else if /i "%BUILD_CONFIG%"=="debug" (
    set "BUILD_CONFIG=debug"
    set "BUILD_DIR=%PROJECT_ROOT%\build-debug"
    set "OTHER_CONFIG=release"
) else (
    echo ERROR: unknown configuration: %BUILD_CONFIG%
    echo Usage: %~nx0 [release or debug]
    exit /b 2
)

rem ---- locate qmake ----
if not defined QMAKE (
    for %%Q in (qmake6.exe qmake.exe) do (
        if not defined QMAKE (
            for /f "delims=" %%P in ('where %%Q 2^>nul') do (
                if not defined QMAKE set "QMAKE=%%P"
            )
        )
    )
)
if not defined QMAKE (
    if exist "C:\Qt\6.11.1\mingw_64\bin\qmake6.exe" set "QMAKE=C:\Qt\6.11.1\mingw_64\bin\qmake6.exe"
)
if not defined QMAKE (
    echo ERROR: qmake was not found in PATH.
    echo Add the Qt 6 bin folder to PATH, for example:
    echo     set PATH=C:\Qt\6.11.1\mingw_64\bin;%%PATH%%
    echo or set QMAKE to the full path of qmake6.exe.
    exit /b 1
)

rem ---- locate make ----
if not defined MAKE (
    for %%M in (mingw32-make.exe make.exe) do (
        if not defined MAKE (
            for /f "delims=" %%P in ('where %%M 2^>nul') do (
                if not defined MAKE set "MAKE=%%P"
            )
        )
    )
)
if not defined MAKE (
    if exist "C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe" set "MAKE=C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe"
)
if not defined MAKE (
    echo ERROR: mingw32-make was not found in PATH.
    echo Add the Qt MinGW Tools bin folder to PATH, for example:
    echo     set PATH=C:\Qt\Tools\mingw1310_64\bin;%%PATH%%
    echo or set MAKE to the full path of mingw32-make.exe.
    exit /b 1
)

rem ---- put the compiler on PATH ----
rem qmake runs g++ to probe the toolchain, so having qmake and make is not
rem enough. This only changes PATH inside this script.
call "%~dp0mingw_env.bat"
if errorlevel 1 exit /b 1

rem ---- locate pkg-config and libtorrent ----
if not defined PKG_CONFIG (
    for /f "delims=" %%P in ('where pkg-config.exe 2^>nul') do (
        if not defined PKG_CONFIG set "PKG_CONFIG=%%P"
    )
)
if not defined PKG_CONFIG (
    echo ERROR: pkg-config.exe was not found in PATH.
    echo The project links libtorrent-rasterbar through pkg-config.
    echo Install pkg-config and a MinGW build of libtorrent, for example with MSYS2:
    echo     pacman -S mingw-w64-x86_64-pkgconf mingw-w64-x86_64-libtorrent-rasterbar
    echo and put C:\msys64\mingw64\bin on PATH, or set PKG_CONFIG to pkg-config.exe.
    exit /b 1
)

"%PKG_CONFIG%" --exists libtorrent-rasterbar
if errorlevel 1 (
    echo ERROR: pkg-config cannot find libtorrent-rasterbar.
    echo Set PKG_CONFIG_PATH to the folder that holds libtorrent-rasterbar.pc,
    echo for example:
    echo     set PKG_CONFIG_PATH=C:\msys64\mingw64\lib\pkgconfig
    exit /b 1
)

set "LIBTORRENT_VERSION="
for /f "delims=" %%V in ('"%PKG_CONFIG%" --modversion libtorrent-rasterbar 2^>nul') do set "LIBTORRENT_VERSION=%%V"

if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=1"

echo.
echo Project:       %PROJECT_ROOT%
echo Configuration: %BUILD_CONFIG%
echo Build folder:  %BUILD_DIR%
echo qmake:         %QMAKE%
echo make:          %MAKE%
echo compiler:      %MINGW_BIN%
echo pkg-config:    %PKG_CONFIG%
echo libtorrent:    %LIBTORRENT_VERSION%
echo.

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: failed to create build folder: "%BUILD_DIR%"
    exit /b 1
)

pushd "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: failed to enter build folder: "%BUILD_DIR%"
    exit /b 1
)

rem qmake runs on every build so a changed .pro is picked up.
rem debug_and_release is removed so the executable lands directly in the build
rem folder instead of a release\ or debug\ subfolder.
"%QMAKE%" "%PROJECT_FILE%" "CONFIG+=%BUILD_CONFIG%" "CONFIG-=%OTHER_CONFIG%" "CONFIG-=debug_and_release" "CONFIG-=debug_and_release_target"
if errorlevel 1 (
    popd
    echo ERROR: qmake failed.
    exit /b 1
)

"%MAKE%" -j%JOBS%
if errorlevel 1 (
    popd
    echo ERROR: build failed.
    exit /b 1
)
popd

set "BUILT_EXE="
for /f "delims=" %%F in ('dir /b /s "%BUILD_DIR%\libtorrent_peer_collector_qt.exe" 2^>nul') do (
    if not defined BUILT_EXE set "BUILT_EXE=%%F"
)

echo.
echo Build completed.
if defined BUILT_EXE (
    echo Executable: !BUILT_EXE!
) else (
    echo WARNING: libtorrent_peer_collector_qt.exe was not found under "%BUILD_DIR%".
)
echo.

echo %cmdcmdline% | find /i "%~nx0" >nul
if not errorlevel 1 pause

rem The find above leaves errorlevel 1 when the script was not double
rem clicked, so success is reported explicitly for a caller such as
rem rebuild.bat.
endlocal
exit /b 0

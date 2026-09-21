@echo off
setlocal enabledelayedexpansion

rem ============================================================================
rem scripts\windows\deploy.bat
rem
rem Copies the built executable into a deployment folder, runs windeployqt on
rem it, and copies the libtorrent and OpenSSL DLLs next to it, so the folder
rem can be handed to a machine without Qt or libtorrent installed.
rem
rem Usage:
rem     scripts\windows\deploy.bat [/y]
rem
rem     /y   do not ask before emptying the deployment folder
rem
rem The deployment folder is DELETED and recreated, so the script asks for
rem confirmation first unless /y is given.
rem
rem windeployqt only knows about Qt. libtorrent-rasterbar and the OpenSSL it
rem links are found through pkg-config (the same way the build finds them)
rem and copied from that install's bin folder. Boost is header-only for
rem libtorrent 2.x, so no Boost DLL is needed.
rem
rem Environment overrides:
rem     QT_DEPLOY       full path of windeployqt.exe
rem                     (default: found in PATH, then C:\Qt\6.11.1\mingw_64\bin)
rem     SOURCE_EXE      executable to deploy
rem                     (default: the first one found under build\)
rem     DEPLOY_DIR      deployment folder
rem                     (default: deploy\ inside the project folder, which
rem                      clean.bat removes and .gitignore keeps out of git)
rem     MINGW_BIN       MinGW bin folder, put on PATH so windeployqt can find
rem                     the compiler runtime DLLs
rem                     (default: g++.exe in PATH, then
rem                      C:\Qt\Tools\mingw1310_64\bin)
rem     PKG_CONFIG      pkg-config executable (default: pkg-config.exe in PATH)
rem     LIBTORRENT_BIN  folder holding libtorrent-rasterbar*.dll, libssl*.dll
rem                     and libcrypto*.dll
rem                     (default: <pkg-config prefix of libtorrent>\bin)
rem ============================================================================

pushd "%~dp0..\.."
if errorlevel 1 (
    echo ERROR: failed to locate the project folder.
    exit /b 1
)
set "PROJECT_ROOT=%CD%"
popd

set "EXE_NAME=libtorrent_peer_collector_qt.exe"

if not defined DEPLOY_DIR set "DEPLOY_DIR=%PROJECT_ROOT%\deploy"

rem ---- refuse a deployment folder that is a drive root ----
rem Checked before the path is normalised, because normalising "C:" would turn
rem it into the current folder on that drive and hide the problem.
if "%DEPLOY_DIR:~3%"=="" (
    echo ERROR: refusing to use a drive root as the deployment folder:
    echo "%DEPLOY_DIR%"
    exit /b 1
)

rem ---- normalise to a full path with no trailing backslash ----
if "%DEPLOY_DIR:~-1%"=="\" set "DEPLOY_DIR=%DEPLOY_DIR:~0,-1%"
for %%D in ("%DEPLOY_DIR%") do set "DEPLOY_DIR=%%~fD"

rem ---- refuse the project folder itself, or anything containing it ----
rem The folder is deleted and recreated, so a DEPLOY_DIR of the project root or
rem one of its parents would take the source tree with it.
call set "DEPLOY_TAIL=%%PROJECT_ROOT:%DEPLOY_DIR%=%%"
if not "%DEPLOY_TAIL%"=="%PROJECT_ROOT%" (
    echo ERROR: refusing a deployment folder that holds the project:
    echo "%DEPLOY_DIR%"
    echo It is the project folder or one of its parents, and the deployment
    echo folder is deleted before it is written.
    exit /b 1
)
set "DEPLOY_TAIL="

rem ---- locate windeployqt ----
if not defined QT_DEPLOY (
    for /f "delims=" %%P in ('where windeployqt.exe 2^>nul') do (
        if not defined QT_DEPLOY set "QT_DEPLOY=%%P"
    )
)
if not defined QT_DEPLOY (
    if exist "C:\Qt\6.11.1\mingw_64\bin\windeployqt.exe" set "QT_DEPLOY=C:\Qt\6.11.1\mingw_64\bin\windeployqt.exe"
)
if not defined QT_DEPLOY (
    echo ERROR: windeployqt.exe was not found in PATH.
    echo Add the Qt 6 bin folder to PATH, for example:
    echo     set PATH=C:\Qt\6.11.1\mingw_64\bin;%%PATH%%
    echo or set QT_DEPLOY to the full path of windeployqt.exe.
    exit /b 1
)
if not exist "%QT_DEPLOY%" (
    echo ERROR: windeployqt not found:
    echo "%QT_DEPLOY%"
    exit /b 1
)

rem ---- put the compiler on PATH ----
rem windeployqt finds the MinGW runtime DLLs that --compiler-runtime copies by
rem searching PATH, so the deployment is incomplete without them. This is only
rem a warning: an MSVC build does not need MinGW at all.
call "%~dp0mingw_env.bat"
if errorlevel 1 (
    echo WARNING: continuing without the MinGW compiler runtime on PATH.
    echo WARNING: check that the deployment folder has the runtime DLLs.
    echo.
)

rem ---- locate the libtorrent DLL folder ----
if not defined LIBTORRENT_BIN (
    if not defined PKG_CONFIG (
        for /f "delims=" %%P in ('where pkg-config.exe 2^>nul') do (
            if not defined PKG_CONFIG set "PKG_CONFIG=%%P"
        )
    )
    if defined PKG_CONFIG (
        for /f "delims=" %%V in ('"!PKG_CONFIG!" --variable=prefix libtorrent-rasterbar 2^>nul') do (
            if not defined LIBTORRENT_BIN set "LIBTORRENT_BIN=%%V\bin"
        )
    )
)
if defined LIBTORRENT_BIN (
    rem pkg-config prints forward slashes; cmd copes, but normalise anyway.
    set "LIBTORRENT_BIN=!LIBTORRENT_BIN:/=\!"
)

rem ---- locate the executable to deploy ----
rem Candidates, in order: what build.bat produces, the release subfolder a
rem debug_and_release build would use, and anything else under build\.
if not defined SOURCE_EXE (
    for %%C in (
        "%PROJECT_ROOT%\build\%EXE_NAME%"
        "%PROJECT_ROOT%\build\release\%EXE_NAME%"
    ) do (
        if not defined SOURCE_EXE if exist %%C set "SOURCE_EXE=%%~C"
    )
)
if not defined SOURCE_EXE (
    for /f "delims=" %%F in ('dir /b /s "%PROJECT_ROOT%\build\%EXE_NAME%" 2^>nul') do (
        if not defined SOURCE_EXE set "SOURCE_EXE=%%F"
    )
)
if not defined SOURCE_EXE (
    echo ERROR: no built executable was found under:
    echo "%PROJECT_ROOT%\build"
    echo Build it first:
    echo     "%~dp0build.bat"
    echo or set SOURCE_EXE to the full path of %EXE_NAME%.
    exit /b 1
)
if not exist "%SOURCE_EXE%" (
    echo ERROR: source EXE not found:
    echo "%SOURCE_EXE%"
    exit /b 1
)

set "DEPLOY_EXE=%DEPLOY_DIR%\%EXE_NAME%"

echo.
echo Source EXE:     %SOURCE_EXE%
echo windeployqt:    %QT_DEPLOY%
if defined LIBTORRENT_BIN (
    echo libtorrent bin: %LIBTORRENT_BIN%
) else (
    echo libtorrent bin: not found ^(set LIBTORRENT_BIN^)
)
echo Deploy to:      %DEPLOY_DIR%
echo.

if exist "%DEPLOY_DIR%" (
    if /i not "%~1"=="/y" (
        echo This DELETES everything in "%DEPLOY_DIR%".
        set /p "CONFIRM=Continue? [y/N] "
        if /i not "!CONFIRM!"=="y" (
            echo Cancelled.
            exit /b 1
        )
    )
    echo Cleaning deploy directory: "%DEPLOY_DIR%"
    rmdir /s /q "%DEPLOY_DIR%"
    if errorlevel 1 (
        echo ERROR: failed to remove deploy directory.
        exit /b 1
    )
)

mkdir "%DEPLOY_DIR%"
if errorlevel 1 (
    echo ERROR: failed to create deploy directory.
    exit /b 1
)

echo Copying EXE to deploy directory...
copy /y "%SOURCE_EXE%" "%DEPLOY_EXE%"
if errorlevel 1 (
    echo ERROR: failed to copy EXE.
    exit /b 1
)

pushd "%DEPLOY_DIR%"
if errorlevel 1 (
    echo ERROR: failed to change directory to "%DEPLOY_DIR%".
    exit /b 1
)

rem --compiler-runtime copies the MinGW runtime DLLs next to the executable.
rem windeployqt takes the debug or release build from the executable itself.
echo Running windeployqt...
"%QT_DEPLOY%" --compiler-runtime "%EXE_NAME%"
if errorlevel 1 (
    popd
    echo ERROR: windeployqt failed.
    exit /b 1
)

rem ---- libtorrent and OpenSSL ----
rem windeployqt does not copy these. Missing ones only warn, because the
rem target machine may have them on PATH already.
set "MISSING_DLLS="
if defined LIBTORRENT_BIN (
    echo Copying libtorrent and OpenSSL DLLs...
    for %%N in (libtorrent-rasterbar libssl libcrypto) do (
        set "FOUND_ONE="
        for %%F in ("!LIBTORRENT_BIN!\%%N*.dll") do (
            if exist "%%~F" (
                copy /y "%%~F" . >nul
                echo     %%~nxF
                set "FOUND_ONE=1"
            )
        )
        if not defined FOUND_ONE set "MISSING_DLLS=!MISSING_DLLS! %%N"
    )
) else (
    set "MISSING_DLLS= libtorrent-rasterbar libssl libcrypto"
)
popd

echo.
echo Deploy completed.
echo Output folder: "%DEPLOY_DIR%"
if defined MISSING_DLLS (
    echo.
    echo WARNING: these DLLs were not copied:%MISSING_DLLS%
    echo WARNING: the executable needs them on the target machine. Set
    echo WARNING: LIBTORRENT_BIN to the folder that holds them and rerun.
)
echo.

echo %cmdcmdline% | find /i "%~nx0" >nul
if not errorlevel 1 pause

rem The find above leaves errorlevel 1 when the script was not double
rem clicked, so success is reported explicitly for a caller.
endlocal
exit /b 0

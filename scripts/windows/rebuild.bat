@echo off
setlocal

rem ============================================================================
rem scripts\windows\rebuild.bat
rem
rem Full rebuild: clean.bat followed by build.bat, so nothing is reused from a
rem previous build.
rem
rem Usage:
rem     scripts\windows\rebuild.bat [release or debug]
rem
rem The same environment overrides as build.bat apply: QMAKE, MAKE, MINGW_BIN,
rem PKG_CONFIG, PKG_CONFIG_PATH and JOBS.
rem ============================================================================

set "BUILD_CONFIG=%~1"
if "%BUILD_CONFIG%"=="" set "BUILD_CONFIG=release"

if /i not "%BUILD_CONFIG%"=="release" if /i not "%BUILD_CONFIG%"=="debug" (
    echo ERROR: unknown configuration: %BUILD_CONFIG%
    echo Usage: %~nx0 [release or debug]
    exit /b 2
)

echo.
echo Rebuild: cleaning first.
call "%~dp0clean.bat"
if errorlevel 1 (
    echo ERROR: clean failed.
    exit /b 1
)

echo Rebuild: building %BUILD_CONFIG%.
call "%~dp0build.bat" %BUILD_CONFIG%
if errorlevel 1 (
    echo ERROR: build failed.
    exit /b 1
)

echo %cmdcmdline% | find /i "%~nx0" >nul
if not errorlevel 1 pause

rem The find above leaves errorlevel 1 when the script was not double
rem clicked, so success is reported explicitly for a caller.
endlocal
exit /b 0

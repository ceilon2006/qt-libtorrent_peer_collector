@echo off

rem ============================================================================
rem scripts\windows\mingw_env.bat
rem
rem Puts the MinGW bin folder on PATH for the calling script.
rem
rem qmake shells out to g++ to probe the toolchain, and the generated makefile
rem calls g++ by bare name, so locating mingw32-make.exe is not enough on its
rem own. Without the compiler on PATH the build stops at
rem     Project ERROR: Cannot run compiler 'g++'.
rem
rem Call it, do not run it: it changes PATH in the caller on purpose.
rem     call "%~dp0mingw_env.bat"
rem     if errorlevel 1 exit /b 1
rem
rem The caller keeps the change only until it ends, because the calling scripts
rem run inside setlocal. The PATH of the shell they were started from is left
rem alone.
rem
rem On success MINGW_BIN holds the folder that was added.
rem
rem Environment overrides:
rem     MINGW_BIN   MinGW bin folder holding g++.exe
rem                 (default: the folder of %MAKE%, then g++.exe in PATH, then
rem                  C:\Qt\Tools\mingw1310_64\bin)
rem ============================================================================

rem ---- an explicit MINGW_BIN wins ----
if defined MINGW_BIN goto :resolved

rem ---- the compiler normally sits beside mingw32-make.exe ----
if defined MAKE (
    call :dir_of "%MAKE%"
    if defined MINGW_BIN if not exist "%MINGW_BIN%\g++.exe" set "MINGW_BIN="
)
if defined MINGW_BIN goto :resolved

rem ---- already on PATH ----
for /f "delims=" %%P in ('where g++.exe 2^>nul') do (
    if not defined MINGW_BIN call :dir_of "%%P"
)
if defined MINGW_BIN goto :resolved

rem ---- last resort: the Qt Tools MinGW that the other defaults assume ----
if exist "C:\Qt\Tools\mingw1310_64\bin\g++.exe" set "MINGW_BIN=C:\Qt\Tools\mingw1310_64\bin"

:resolved
if not defined MINGW_BIN (
    echo ERROR: the MinGW compiler ^(g++.exe^) was not found.
    echo Add the Qt MinGW Tools bin folder to PATH, for example:
    echo     set PATH=C:\Qt\Tools\mingw1310_64\bin;%%PATH%%
    echo or set MINGW_BIN to that folder.
    exit /b 1
)
if not exist "%MINGW_BIN%\g++.exe" (
    echo ERROR: g++.exe not found in:
    echo "%MINGW_BIN%"
    echo Set MINGW_BIN to the MinGW bin folder that holds g++.exe.
    exit /b 1
)

set "PATH=%MINGW_BIN%;%PATH%"
exit /b 0

rem Sets MINGW_BIN to the folder of the file passed in, without the trailing
rem backslash that %~dp adds.
:dir_of
set "MINGW_BIN=%~dp1"
if "%MINGW_BIN:~-1%"=="\" set "MINGW_BIN=%MINGW_BIN:~0,-1%"
goto :eof

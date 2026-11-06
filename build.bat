@echo off
rem ============================================================
rem  PESysInfo build script (MSVC, x64 / x86)
rem
rem  Usage:
rem    build.bat            build x64 GUI + CLI
rem    build.bat x64        build x64 GUI + CLI
rem    build.bat x86        build x86 GUI + CLI
rem    build.bat all        build x64 and x86
rem    build.bat clean      remove build/ and dist/
rem ============================================================
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
set "ARG=%~1"

if /i "%ARG%"=="clean" goto :clean
if /i "%ARG%"=="all" (
    call "%ROOT%build.bat" x64
    if errorlevel 1 exit /b 1
    call "%ROOT%build.bat" x86
    exit /b %errorlevel%
)
if /i "%ARG%"=="x86" (
    set "ARCH=x86"
    set "VSCVARS=vcvars32.bat"
    set "MACHINE=X86"
) else if /i "%ARG%"=="x64" (
    set "ARCH=x64"
    set "VSCVARS=vcvars64.bat"
    set "MACHINE=X64"
) else if "%ARG%"=="" (
    set "ARCH=x64"
    set "VSCVARS=vcvars64.bat"
    set "MACHINE=X64"
) else (
    echo [build] unknown argument "%ARG%". Use x64, x86, all or clean.
    exit /b 2
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build] vswhere.exe not found. Visual Studio with C++ tools is required.
    exit /b 2
)
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [build] Visual Studio C++ toolset not found.
    exit /b 2
)

call "%VSPATH%\VC\Auxiliary\Build\%VSCVARS%" >nul
if errorlevel 1 (
    echo [build] failed to initialize MSVC %ARCH% environment.
    exit /b 1
)

set "CORE=%ROOT%src\core"
set "GUI=%ROOT%src\gui"
set "CLI=%ROOT%src\cli"
set "OBJ=%ROOT%build\obj\%ARCH%"
set "CORELIBDIR=%ROOT%build\lib\%ARCH%"
set "OUT=%ROOT%dist\%ARCH%"
if exist "%OBJ%" rmdir /s /q "%OBJ%"
if exist "%CORELIBDIR%\core.lib" del /q "%CORELIBDIR%\core.lib"
if not exist "%OBJ%\core" mkdir "%OBJ%\core" >nul
if not exist "%OBJ%\gui" mkdir "%OBJ%\gui" >nul
if not exist "%OBJ%\cli" mkdir "%OBJ%\cli" >nul
if not exist "%CORELIBDIR%" mkdir "%CORELIBDIR%" >nul
if not exist "%OUT%" mkdir "%OUT%" >nul

set "CFLAGS=/nologo /c /W4 /utf-8 /MT /O2 /GS /DUNICODE /D_UNICODE /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 /D_CRT_SECURE_NO_WARNINGS"

echo [build] core library (%ARCH%)
set "CORE_OBJS="
for %%F in ("%CORE%\*.c") do (
    echo   [cc] %%~nxF
    cl %CFLAGS% /I"%CORE%" /Fo"%OBJ%\core\%%~nF.obj" "%%~fF"
    if errorlevel 1 exit /b 1
    set "CORE_OBJS=!CORE_OBJS! "%OBJ%\core\%%~nF.obj""
)
lib /nologo /OUT:"%CORELIBDIR%\core.lib" !CORE_OBJS!
if errorlevel 1 exit /b 1

echo [build] CLI (%ARCH%)
set "CLI_OBJS="
for %%F in ("%CLI%\*.c") do (
    echo   [cc] %%~nxF
    cl %CFLAGS% /I"%CORE%" /I"%CLI%" /Fo"%OBJ%\cli\%%~nF.obj" "%%~fF"
    if errorlevel 1 exit /b 1
    set "CLI_OBJS=!CLI_OBJS! "%OBJ%\cli\%%~nF.obj""
)
set "CLI_RES="
if exist "%CLI%\cli.rc" (
    echo   [rc] cli.rc
    rc /nologo /I"%CLI%" /fo "%OBJ%\cli\cli.res" "%CLI%\cli.rc"
    if errorlevel 1 exit /b 1
    set "CLI_RES=%OBJ%\cli\cli.res"
)
echo [link] %OUT%\PESysInfo-cli.exe
link /nologo /OUT:"%OUT%\PESysInfo-cli.exe" /SUBSYSTEM:CONSOLE /MACHINE:%MACHINE% ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO !CLI_OBJS! !CLI_RES! ^
     "%CORELIBDIR%\core.lib" kernel32.lib advapi32.lib user32.lib iphlpapi.lib
if errorlevel 1 exit /b 1

echo [build] GUI (%ARCH%)
set "GUI_OBJS="
for %%F in ("%GUI%\*.c") do (
    echo   [cc] %%~nxF
    cl %CFLAGS% /I"%CORE%" /I"%GUI%" /Fo"%OBJ%\gui\%%~nF.obj" "%%~fF"
    if errorlevel 1 exit /b 1
    set "GUI_OBJS=!GUI_OBJS! "%OBJ%\gui\%%~nF.obj""
)
set "GUI_RES="
if exist "%GUI%\gui.rc" (
    echo   [rc] gui.rc
    rc /nologo /I"%GUI%" /fo "%OBJ%\gui\gui.res" "%GUI%\gui.rc"
    if errorlevel 1 exit /b 1
    set "GUI_RES=%OBJ%\gui\gui.res"
)
echo [link] %OUT%\PESysInfo.exe
link /nologo /OUT:"%OUT%\PESysInfo.exe" /SUBSYSTEM:WINDOWS /MACHINE:%MACHINE% ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO !GUI_OBJS! !GUI_RES! ^
     "%CORELIBDIR%\core.lib" kernel32.lib advapi32.lib user32.lib gdi32.lib comctl32.lib iphlpapi.lib
if errorlevel 1 exit /b 1

echo [build] OK (%ARCH%)
echo   %OUT%\PESysInfo.exe
echo   %OUT%\PESysInfo-cli.exe
exit /b 0

:clean
if exist "%ROOT%build" rmdir /s /q "%ROOT%build"
if exist "%ROOT%dist" rmdir /s /q "%ROOT%dist"
echo [clean] removed build\ and dist\
exit /b 0

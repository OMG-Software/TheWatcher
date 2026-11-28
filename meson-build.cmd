@echo off
setlocal enableextensions

rem Build and test TheWatcher with Meson + Ninja.
rem This wrapper builds the release configuration.
rem Dependencies are built as local static Meson subprojects.
rem
rem Every command is followed by `if errorlevel 1 exit /b %ERRORLEVEL%` on its
rem OWN line, outside any parenthesised block. cmd expands %ERRORLEVEL% when it
rem parses a line (a whole block is parsed at once), so the failure check must be
rem a separate top-level line to see the real exit code. `cmd || exit /b %ERRORLEVEL%`
rem would always exit 0 (stale value), and a bare `|| exit /b` also exits 0.

set "BUILD_DIR=builddir-release"

where meson >nul 2>&1
if errorlevel 1 (
    echo [meson-build] meson not found on PATH. Install with: pip install meson ninja
    exit /b 1
)

rem Put the MSVC toolchain on PATH unless cl.exe is already available (CI sets
rem it up with ilammy/msvc-dev-cmd, and a Developer Prompt has it too).
where cl >nul 2>&1
if not errorlevel 1 goto have_compiler

rem Find Visual Studio (2022 or newer) with the C++ x64 tools via vswhere,
rem which the Visual Studio installer always places in this fixed location.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vswhere

set "VS_PATH="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -version [17.0^,^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%I"
if not defined VS_PATH goto no_vs

echo [meson-build] Using Visual Studio at %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
goto have_compiler

:no_vswhere
echo [meson-build] vswhere.exe not found. Install Visual Studio 2022 or newer Build Tools with the C++ workload:
echo [meson-build]   .\scripts\setup-build-env.cmd
exit /b 1

:no_vs
echo [meson-build] No Visual Studio 2022+ installation with the C++ x64 tools was found.
echo [meson-build] Install the "Desktop development with C++" workload:
echo [meson-build]   .\scripts\setup-build-env.cmd
exit /b 1

:have_compiler

rem --backend=ninja stops Meson choosing the VS solution generator when MSVC
rem is present; harmless on hosts where ninja is the default backend anyway.
rem Only an already-configured directory can be reconfigured: a directory left
rem half-created by an interrupted setup has no coredata and needs a plain setup.
set "RECONFIGURE="
if exist "%BUILD_DIR%\meson-private\coredata.dat" set "RECONFIGURE=--reconfigure"
echo [meson-build] Configuring %BUILD_DIR% %RECONFIGURE% ...
meson setup "%BUILD_DIR%" %RECONFIGURE% --buildtype=release --default-library=static --backend=ninja %*
if errorlevel 1 exit /b %ERRORLEVEL%

echo [meson-build] Compiling ...
meson compile -C "%BUILD_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

echo [meson-build] Testing ...
meson test -C "%BUILD_DIR%" --suite thewatcher --print-errorlogs
if errorlevel 1 exit /b %ERRORLEVEL%

echo [meson-build] Done.
exit /b 0

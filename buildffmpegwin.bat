@echo off
setlocal EnableExtensions
REM Builds optional minimal FFmpeg only; leaves ffmpeg-shared and AE untouched.
REM Requires Visual Studio 2022 C++ tools and MSYS2 with GNU Make (pacman -S make).
REM Optional env: MSYS2_ROOT, GNU_MAKE_DIR, FFMPEG_WORK_DIR, FFMPEG_PREFIX.
set "ROOT=%~dp0"
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (echo Set MSYS2_ROOT to your MSYS2 installation. & exit /b 1)
if defined VSCMD_VER goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio 2022 C++ tools not found. & exit /b 1)
set "VS_PATH_FILE=%TEMP%\AlphaFix-vs-%RANDOM%.txt"
"%VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VS_PATH_FILE%"
for /f "usebackq tokens=*" %%V in ("%VS_PATH_FILE%") do set "VSROOT=%%V"
del "%VS_PATH_FILE%"
if not defined VSROOT (echo Visual Studio 2022 C++ tools not found. & exit /b 1)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
:build
REM Keep MSVC link.exe ahead of MSYS2's unrelated link utility.
set "PATH=%PATH%;%MSYS2_ROOT%\usr\bin;%GNU_MAKE_DIR%"
"%MSYS2_ROOT%\usr\bin\bash.exe" --noprofile --norc "%ROOT%scripts\build-ffmpeg-win.sh"
exit /b %errorlevel%

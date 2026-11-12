@echo off
setlocal EnableExtensions
REM Usage: buildwin.bat [--no-install] [--no-pause]
REM Optional env: AE_SDK_PATH, FFMPEG_PATH, AE_BASE_DIR, ALPHAFIX_VERSION
set "ROOT=%~dp0"
set "DO_INSTALL=1"
set "DO_PAUSE=1"
:args
if "%~1"=="" goto setup
if /i "%~1"=="--no-install" (set "DO_INSTALL=0") else if /i "%~1"=="--no-pause" (set "DO_PAUSE=0") else (echo Unknown option: %~1 & exit /b 1)
shift
goto args
:setup
if not defined AE_SDK_PATH set "AE_SDK_PATH=%ROOT%AfterEffectsSDK"
if not defined FFMPEG_PATH set "FFMPEG_PATH=%ROOT%third_party\ffmpeg-win-minimal"
REM Resolve Python before a FOR /F child CMD can alter the user's PATH.
for %%P in (python.exe) do set "PYTHON_TOOL=%%~$PATH:P"
if not defined PYTHON_TOOL (echo Python 3 is required on PATH. & goto failed)
if not defined ALPHAFIX_VERSION for /f %%V in ('""%PYTHON_TOOL%" "%ROOT%scripts\version.py""') do set "ALPHAFIX_VERSION=%%V"
REM Use the active developer environment, or discover any VS 2022 edition.
if defined VSCMD_VER goto configure
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio 2022 C++ build tools not found. & goto failed)
set "VS_PATH_FILE=%TEMP%\AlphaFix-vs-%RANDOM%.txt"
"%VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VS_PATH_FILE%"
for /f "usebackq tokens=*" %%V in ("%VS_PATH_FILE%") do set "VSROOT=%%V"
del "%VS_PATH_FILE%"
if not defined VSROOT (echo Visual Studio 2022 C++ build tools not found. & goto failed)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto failed
:configure
cd /d "%ROOT%"
if not exist "%FFMPEG_PATH%\include\libavformat\avformat.h" (
    set "FFMPEG_PREFIX=%FFMPEG_PATH%"
    call "%ROOT%buildffmpegwin.bat"
    if errorlevel 1 goto failed
)
"%PYTHON_TOOL%" "%ROOT%scripts\verify-minimal-ffmpeg.py" --prefix "%FFMPEG_PATH%"
if errorlevel 1 goto failed
cmake -B Win -S . -G "Visual Studio 17 2022" -A x64 ^
    -DAE_SDK_PATH="%AE_SDK_PATH%" ^
    -DFFMPEG_PATH="%FFMPEG_PATH%" ^
    -DAE_BASE_DIR="%AE_BASE_DIR%" ^
    -DALPHAFIX_VERSION="%ALPHAFIX_VERSION%"
if errorlevel 1 goto failed
cmake --build Win --config Release --parallel
if errorlevel 1 goto failed
ctest --test-dir Win -C Release --output-on-failure
if errorlevel 1 goto failed
if "%DO_INSTALL%"=="0" goto done
cmake --install Win --config Release
if errorlevel 1 goto failed
:done
echo Alpha Fix build complete. Package folder: %ROOT%Release-Alpha-Fix
if "%DO_PAUSE%"=="1" pause
exit /b 0
:failed
echo Alpha Fix build or install failed. See the error above.
if "%DO_PAUSE%"=="1" pause
exit /b 1

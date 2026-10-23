@echo off
rem Sets up the Visual Studio C++ (x64) build environment for the build scripts, once.
rem Already set up (cl on the PATH, e.g. in a Developer Command Prompt or CI): nothing to do. Otherwise the VS 2022
rem Build Tools' vcvars64.bat, or whichever Visual Studio vswhere finds with the C++ tools (Community, Professional,
rem Enterprise, the GitHub runners' install).
where cl >nul 2>nul && exit /b 0
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto found
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not exist "%VCVARS%" goto missing
:found
call "%VCVARS%" >nul 2>nul
where cl >nul 2>nul && exit /b 0
:missing
echo Visual Studio's C++ build tools were not found. Install "Build Tools for Visual Studio 2022" with the
echo "Desktop development with C++" workload (see CONTRIBUTING.md).
exit /b 1

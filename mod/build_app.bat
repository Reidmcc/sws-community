@echo off
rem Build the settings app (issues #4, #5): build\sws_settings.exe, one exe on the .NET Framework that ships with
rem Windows 10 and 11, and build\sws_settings_test.exe, which prints the fingerprint the app computes for a profile.
rem Needs the C# compiler from Visual Studio's build tools (the same install build_server.bat uses).
rem The app is built to a new file first and only then swapped in, so an open copy of the app is never lost: if it's
rem running, the swap fails, the old exe stays, and you're told to close it.
setlocal
cd /d "%~dp0"
set "CSC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if exist "%CSC%" goto havecsc
rem another Visual Studio (Community, Professional, Enterprise, the GitHub runners'): ask vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto nocsc
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -find MSBuild\**\Bin\Roslyn\csc.exe`) do set "CSC=%%i"
if not exist "%CSC%" goto nocsc
:havecsc
set "SRC=app\App.cs app\Core.cs app\Words.cs app\WordsLogic.cs"
"%CSC%" -nologo -optimize+ -target:winexe -out:build\sws_settings.new.exe -r:System.Windows.Forms.dll -r:System.Drawing.dll -r:Microsoft.VisualBasic.dll %SRC% || exit /b 1
"%CSC%" -nologo -optimize+ -target:exe -out:build\sws_settings_test.exe app\SelfTest.cs app\Core.cs app\Words.cs app\WordsLogic.cs || exit /b 1
move /y build\sws_settings.new.exe build\sws_settings.exe >nul 2>nul || goto inuse
echo APP BUILD OK
exit /b 0
:inuse
echo The settings app is open, so build\sws_settings.exe was left as it is. Close the app and run this again;
echo the new build is in build\sws_settings.new.exe meanwhile.
exit /b 1
:nocsc
echo C# compiler not found: "%CSC%"
exit /b 1

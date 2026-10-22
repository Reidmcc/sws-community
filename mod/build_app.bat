@echo off
rem Build the settings app (issues #4, #5): build\sws_settings.exe, one exe on the .NET Framework that ships with
rem Windows 10 and 11, and build\sws_settings_test.exe, which prints the fingerprint the app computes for a profile.
rem Needs the C# compiler from Visual Studio's build tools (the same install build_server.bat uses).
setlocal
cd /d "%~dp0"
set "CSC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if not exist "%CSC%" goto nocsc
"%CSC%" -nologo -optimize+ -target:winexe -out:build\sws_settings.exe -r:System.Windows.Forms.dll -r:System.Drawing.dll -r:Microsoft.VisualBasic.dll app\App.cs app\Core.cs app\Words.cs app\WordsLogic.cs || exit /b 1
"%CSC%" -nologo -optimize+ -target:exe -out:build\sws_settings_test.exe app\SelfTest.cs app\Core.cs app\Words.cs app\WordsLogic.cs || exit /b 1
echo APP BUILD OK
exit /b 0
:nocsc
echo C# compiler not found: "%CSC%"
exit /b 1

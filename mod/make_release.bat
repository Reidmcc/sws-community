@echo off
rem Build a player release: dist\sws-community-v<version>\ and its zip.
rem Usage: make_release.bat <version>     (for example: make_release.bat 0.1.0)
rem Needs build\unlock_ids.txt, which is generated from the game's data by the local research tools (not in the repo),
rem and build\limits.ini, from tools\gen_limits.py (reads build\defaults.ini, written by any launch of the mod).
setlocal
if "%~1"=="" (echo usage: make_release.bat ^<version^> & exit /b 1)
set "VER=%~1"
set "HERE=%~dp0"
if not exist "%HERE%build\unlock_ids.txt" (echo build\unlock_ids.txt is missing: generate it first. & exit /b 1)
if not exist "%HERE%build\limits.ini" (echo build\limits.ini is missing: run tools\gen_limits.py first. & exit /b 1)

call "%HERE%build_server.bat" || exit /b 1
call "%HERE%build_app.bat" || exit /b 1

set "DIST=%HERE%..\dist"
set "OUT=%DIST%\sws-community-v%VER%"
if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%DIST%\sws-community-v%VER%.zip" del /q "%DIST%\sws-community-v%VER%.zip"
mkdir "%OUT%\profiles" || exit /b 1

copy /y "%HERE%build\sws_launch.exe" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%build\sws_server.dll" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%build\sws_settings.exe" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%build\sws_check.exe" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%build\unlock_ids.txt" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%build\limits.ini" "%OUT%\" >nul || exit /b 1
rem defaults.ini: every config recorded so far, so the settings app lists all values before a player's first launch
if exist "%HERE%build\defaults.ini" copy /y "%HERE%build\defaults.ini" "%OUT%\" >nul
copy /y "%HERE%release\server.ini" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%release\README.txt" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%release\profiles\*.ini" "%OUT%\profiles\" >nul || exit /b 1
copy /y "%HERE%..\LICENSE" "%OUT%\LICENSE.txt" >nul || exit /b 1
copy /y "%HERE%..\NOTICE" "%OUT%\NOTICE.txt" >nul || exit /b 1

for /f %%r in ('git -C "%HERE%.." rev-parse --short HEAD') do set "REV=%%r"
> "%OUT%\VERSION.txt" (
  echo STAR WARS: Squadrons community server mod v%VER%
  echo Built from commit %REV% of https://github.com/Reidmcc/sws-community
)

pushd "%DIST%" || exit /b 1
rem Windows' own tar (bsdtar) writes a real zip for a .zip name. Named by its full path: in a Git Bash session Git's GNU
rem tar comes first on the PATH, and it quietly writes a tar archive under the .zip name (v0.1.0 to v0.2.1 shipped that way).
"%SystemRoot%\System32\tar.exe" -a -c -f "sws-community-v%VER%.zip" "sws-community-v%VER%" || (popd & exit /b 1)
popd
powershell -NoProfile -Command "$s=[IO.File]::OpenRead('%DIST%\sws-community-v%VER%.zip'); $b=New-Object byte[] 2; [void]$s.Read($b,0,2); $s.Close(); if ($b[0] -ne 0x50 -or $b[1] -ne 0x4B) { exit 1 }" || (echo NOT A ZIP: dist\sws-community-v%VER%.zip & exit /b 1)
echo RELEASE OK: dist\sws-community-v%VER%.zip

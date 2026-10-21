@echo off
rem Build a player release: dist\sws-community-v<version>\ and its zip.
rem Usage: make_release.bat <version>     (for example: make_release.bat 0.1.0)
rem Needs build\unlock_ids.txt, which is generated from the game's data by the local research tools (not in the repo).
setlocal
if "%~1"=="" (echo usage: make_release.bat ^<version^> & exit /b 1)
set "VER=%~1"
set "HERE=%~dp0"
if not exist "%HERE%build\unlock_ids.txt" (echo build\unlock_ids.txt is missing: generate it first. & exit /b 1)

call "%HERE%build_server.bat" || exit /b 1

set "DIST=%HERE%..\dist"
set "OUT=%DIST%\sws-community-v%VER%"
if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%DIST%\sws-community-v%VER%.zip" del /q "%DIST%\sws-community-v%VER%.zip"
mkdir "%OUT%\profiles" || exit /b 1

copy /y "%HERE%build\sws_launch.exe" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%build\sws_server.dll" "%OUT%\" >nul || exit /b 1
copy /y "%HERE%build\unlock_ids.txt" "%OUT%\" >nul || exit /b 1
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
tar -a -c -f "sws-community-v%VER%.zip" "sws-community-v%VER%" || (popd & exit /b 1)
popd
echo RELEASE OK: dist\sws-community-v%VER%.zip

@echo off
call "%~dp0vsenv.bat" || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /EHa /LD sws_dump.cpp /Fo:build\ /Fe:build\sws_dump.dll kernel32.lib || exit /b 1
echo BUILD OK

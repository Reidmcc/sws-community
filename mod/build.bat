@echo off
call "%~dp0vsenv.bat" || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /EHa /LD sws_probe.cpp /Fo:build\ /Fe:build\sws_probe.dll user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MT /W3 /EHa /LD /DPROBE_TEST sws_probe.cpp /Fo:build\t_ /Fe:build\sws_probe_test.dll user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MT /W3 inject.cpp /Fo:build\ /Fe:build\inject.exe kernel32.lib advapi32.lib || exit /b 1
cl /nologo /O2 /MT /W3 test_host.cpp /Fo:build\ /Fe:build\test_host.exe kernel32.lib || exit /b 1
echo BUILD OK

@echo off
call "%~dp0vsenv.bat" || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHa /LD /DRULES /DSERVER sws_probe.cpp /Fo:build\s_ /Fe:build\sws_server.dll user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MT /W3 /EHa /LD /DRULES /DSERVER /DPROBE_TEST sws_probe.cpp /Fo:build\st_ /Fe:build\sws_server_test.dll user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MT /W3 sws_launch.cpp /Fo:build\ /Fe:build\sws_launch.exe kernel32.lib advapi32.lib || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc sws_check.cpp /Fo:build\ /Fe:build\sws_check.exe || exit /b 1
cl /nologo /O2 /MT /W3 test_server_target.cpp /Fo:build\ /Fe:build\test_server_target.exe kernel32.lib || exit /b 1
echo BUILD OK

@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d E:\sws_community\probe
cl /nologo /O2 /MT /W3 /EHa /LD /DPROBE_TEST sws_probe.cpp /Fo:build\t_ /Fe:build\sws_probe_test.dll user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MT /W3 test_host.cpp /Fo:build\ /Fe:build\test_host.exe kernel32.lib || exit /b 1
echo BUILD OK

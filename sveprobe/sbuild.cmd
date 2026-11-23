@echo off
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" arm64
cl /nologo /O1 /MT /W3 sveprobe.c /link /OUT:sveprobe.exe
cl /nologo /O1 /MT /W3 kver.c version.lib /link /OUT:kver.exe
cl /nologo /O1 /MT /W3 /D_CRT_SECURE_NO_WARNINGS setfeat.c /link /OUT:setfeat.exe
cl /nologo /O1 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS p3agent.c ws2_32.lib /link /OUT:p3agent.exe

@echo off
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /O1 /MT /W3 kprobe2.c advapi32.lib version.lib psapi.lib /link /OUT:kprobe2.exe

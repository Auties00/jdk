@echo off
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" arm64
cl /nologo /O1 /MT /W3 sveprobe.c /link /OUT:sveprobe.exe

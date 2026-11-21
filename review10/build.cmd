@echo off
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
echo VS: %VSDIR%
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" arm64_amd64 >nul || exit /b 1
ml64 /nologo /c /Foxprobe_asm.obj xprobe.asm || exit /b 1
cl /nologo /O1 /W3 xprobe.c xprobe_asm.obj /link /INCREMENTAL:NO /OUT:xprobe.exe || exit /b 1
link /nologo /DLL /NOENTRY /INCREMENTAL:NO /EXPORT:fill_below /EXPORT:amx_ldtilecfg /EXPORT:amx_tilezero0 /EXPORT:amx_tilerelease xprobe_asm.obj /OUT:r5native.dll || exit /b 1
echo built

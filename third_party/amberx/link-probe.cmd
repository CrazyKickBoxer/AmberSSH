@echo off
rem link-probe.cmd — after probe-core.cmd has left one .obj per allowlisted
rem source in probe-out\, archive them into AmberXCore.lib and try to link
rem an executable with /WHOLEARCHIVE so every object is pulled in. The link
rem is EXPECTED TO FAIL; its error list is the point. Nothing is run.
setlocal
call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set ROOT=%~dp0
set OUT=%ROOT%probe-out
cd /d "%OUT%"
del /q AmberXCore.lib link-stub.obj AmberXLinkProbe.exe 2>nul
lib /nologo /OUT:AmberXCore.lib *.obj > lib.log 2>&1
cl /nologo /c /w /Fo"link-stub.obj" "%ROOT%link-stub.c" > stub.log 2>&1
link /nologo /OUT:AmberXLinkProbe.exe /WHOLEARCHIVE:AmberXCore.lib link-stub.obj AmberXCore.lib ^
     /NODEFAULTLIB:libcmt.lib msvcrt.lib > link.log 2>&1
echo LINK_EXIT %errorlevel% >> link.log
endlocal

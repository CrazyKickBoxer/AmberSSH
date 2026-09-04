@echo off
rem probe-core.cmd — the Phase 1 build gate, as a measurement.
rem
rem Compiles every .c in the device-independent core (dix, mi, fb) of the
rem pinned xserver tree with MSVC, one at a time, against the hand-written
rem config in config-msvc\ and the pinned xorgproto/pixman headers. os\ is
rem excluded on purpose: it is the POSIX layer a Windows port replaces, and
rem including it would measure the wrong thing.
rem
rem Writes one log per file to %OUT% and prints PASS/FAIL per file. The
rem summary is produced by run-probe.sh. Nothing is linked and nothing is
rem produced: this answers "does it compile", nothing more.
setlocal enabledelayedexpansion
call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set ROOT=%~dp0
set UP=%ROOT%upstream
set OUT=%ROOT%probe-out
if not exist "%OUT%" mkdir "%OUT%"
set INC=/I"%ROOT%config-msvc" /I"%ROOT%config-msvc\compat" /I"%UP%\xserver\include" /I"%UP%\xserver\Xext" /I"%UP%\xserver\miext\damage" /I"%UP%\xserver\miext\sync" /I"%UP%\xserver\render" /I"%UP%\xserver\xkb" /I"%UP%\xserver\Xi" /I"%UP%\xserver\mi" /I"%UP%\xserver\fb" /I"%UP%\xserver\composite" /I"%UP%\xserver\randr" /I"%UP%\xserver\present" /I"%UP%\xserver\xfixes" /I"%UP%\xserver\damageext" /I"%UP%\xserver\dix" /I"%UP%\xorgproto\include" /I"%UP%\libxfont\include" /I"%UP%\libxkbfile\include" /I"%UP%\pixman\pixman" /I"%ROOT%config-msvc\pixman"
set DEFS=/DHAVE_DIX_CONFIG_H /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX
cd /d "%UP%\xserver"
rem The core, then every extension subtree on the allowlist. miext dirs are
rem nested, so the log name flattens the path separator.
for %%d in (dix mi fb render randr xfixes damageext composite present xkb Xi miext\damage miext\sync) do (
  set L=%%d
  set L=!L:\=_!
  for %%f in (%%d\*.c) do (
    rem misyncshm.c is the MIT-SHM fence path. MIT-SHM is off by policy
    rem (REJECTED-COMPONENTS.md), so it is not on the allowlist and is not
    rem measured — skipped rather than shimmed with a pretend mmap.
    if /i "%%~nf"=="misyncshm" (
      echo SKIP %%f
    ) else (
      cl /nologo /c /w /TC /std:c11 %DEFS% %INC% /Fo"%OUT%\!L!__%%~nf.obj" "%%f" > "%OUT%\!L!__%%~nf.log" 2>&1
      if !errorlevel! equ 0 (echo PASS %%f) else (echo FAIL %%f)
    )
  )
)
rem Xext: only the approved subset (LICENSE-MATRIX.md). The rest of the
rem directory is out of scope and is not measured.
set L=Xext
for %%f in (Xext\bigreq.c Xext\shape.c Xext\sync.c Xext\xcmisc.c Xext\xtest.c Xext\security.c Xext\hashtable.c Xext\geext.c Xext\xace.c) do (
  cl /nologo /c /w /TC /std:c11 %DEFS% %INC% /Fo"%OUT%\!L!__%%~nf.obj" "%%f" > "%OUT%\Xext__%%~nf.log" 2>&1
  if !errorlevel! equ 0 (echo PASS %%f) else (echo FAIL %%f)
)
endlocal

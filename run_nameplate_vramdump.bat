@echo off
rem Nameplate/HUD dot hunt (Oct 21): dumps the 4 MB GS memory 150 s after start (vram_dump.bin), to diff our shared
rem GS memory (native) against paraLLEl-GS (diag, clean). Be on the Select Stage screen before 150 s and stay there
rem until the log says [vramdump]. Argument: native | diag
if "%~1"=="" (echo usage: %~nx0 native^|diag & exit /b 1)
set DUMPDIR=F:\hctp-newbase\logs\vramdump_%~1
if not exist "%DUMPDIR%" mkdir "%DUMPDIR%"
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_VRAM_DUMP_SEC=150
set HCTP_NATIVE_P07=0
set HCTP_MOVIES=1
set VENDETTA_EE_CYCLE_PCT=100
if /i "%~1"=="native" set HCTP_RENDERER=cpu
if /i "%~1"=="native" set EXE=F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe
if /i "%~1"=="diag" set HCTP_VU1_FAST=1
if /i "%~1"=="diag" set EXE=F:\hctp-newbase\PS2Recomp\out\diag\ps2xRuntime\ps2EntryRunner.exe
"%EXE%" SLUS_207.87 2> "%DUMPDIR%\run.err"
if exist vram_dump.bin move /y vram_dump.bin "%DUMPDIR%\vram_dump.bin" >nul

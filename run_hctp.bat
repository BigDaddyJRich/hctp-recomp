@echo off
rem Starts HCTP (no pad script). Log: F:\hctp-newbase\logs\x.err
rem VENDETTA_EE_CYCLE_PCT: how much emulated time guest code is charged (100 = old flat 32 cycles per loop).
rem   Lower = more game work per frame (match frame rate up); too low = menus/animations may run fast. Try 35, 50, 100.
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_MOVIES=1
set HCTP_PCPROF=1
set VENDETTA_HOSTPROF=1
set VENDETTA_SPULOG=1
set VENDETTA_EE_CYCLE_PCT=100
set VENDETTA_PADLOG=1
set VENDETTA_PADSCRIPT=
set HCTP_VU1_FAST=1
"F:\hctp-newbase\PS2Recomp\out\fast\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\x.err"

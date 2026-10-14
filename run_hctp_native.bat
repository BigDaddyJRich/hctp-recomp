@echo off
rem Starts the NATIVE RENDERER build (branch native-renderer, out\native, HCTP_RENDERER=d3d11). Log: F:\hctp-newbase\logs\x_native.err
rem Fallback = run_hctp.bat (out\fast, tag fallback-2026-10-14). HCTP_RENDERER unset = the paraLLEl-GS renderer.
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_MOVIES=1
set HCTP_PCPROF=1
set VENDETTA_HOSTPROF=1
set VENDETTA_SPULOG=1
set VENDETTA_EE_CYCLE_PCT=100
set VENDETTA_PADLOG=1
set VENDETTA_PADSCRIPT=
set HCTP_VU1_FAST=1
set HCTP_RENDERER=d3d11
set HCTP_NATIVE_P08=1
"F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\x_native.err"

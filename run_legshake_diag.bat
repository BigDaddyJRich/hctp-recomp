@echo off
rem Leg-shake split test (Oct 21): CURRENT source built the old way (raylib + paraLLEl-GS, out\diag) - same settings as
rem run_hctp.bat. Steady here = the native host (D3D11/XAudio2/Win32 timing) causes the shake; shakes here = a code change.
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_MOVIES=1
set VENDETTA_EE_CYCLE_PCT=100
set HCTP_VU1_FAST=1
"F:\hctp-newbase\PS2Recomp\out\diag\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\legshake_diag.err"

@echo off
rem Leg-shake split test 2 (Oct 21): the NATIVE exe with every built-in native default off (stage-1 skinning p01-p04
rem incl. S1_ANYHASH, p08 clip, VU1 natives, native ADX) and the CPU GS renderer. Steady here = one of those defaults;
rem shakes here = the bare native host itself (Win32 present/vsync pacing, XInput, XAudio2).
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_NATIVE_DEFAULTS=0
set HCTP_RENDERER=cpu
set HCTP_MOVIES=1
set VENDETTA_EE_CYCLE_PCT=100
"F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\legshake_native_nodefaults.err"

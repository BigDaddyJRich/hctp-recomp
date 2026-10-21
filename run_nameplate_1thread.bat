@echo off
rem Nameplate/HUD dot test (Oct 21): GS memory dump showed row 0 of every 32-row page losing its top byte (the
rem PSMT4HL/HH font/HUD textures). Native exe, D3D11 renderer, ONE GS drawing thread. Clean = the threaded band split.
cd /d "F:\HCTP MODS\Vanilla"
set VENDETTA_GS_THREADS=1
set HCTP_NATIVE_P07=0
"F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\nameplate_1thread.err"

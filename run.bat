@echo off
rem Runs the recompiled game (the fast build). Debug switches: set VENDETTA_THREADS=1 / VENDETTA_CDLOG=1 first.
cd /d "%~dp0"
PS2Recomp\out\fast\ps2xRuntime\ps2EntryRunner.exe "%~dp0game\SLUS_206.39"

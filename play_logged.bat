@echo off
rem Starts HCTP with a separate log per run (never overwritten): F:\hctp-newbase\logs\runs\run_YYYYMMDD_HHMMSS.err
rem After a crash, search that file for "[ram]" (the crash report names the game function).
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%t
if not exist "F:\hctp-newbase\logs\runs" mkdir "F:\hctp-newbase\logs\runs"
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_MOVIES=native
"F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\runs\run_%STAMP%.err" > "F:\hctp-newbase\logs\runs\run_%STAMP%.out"

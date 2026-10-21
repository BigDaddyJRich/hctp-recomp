@echo off
rem Nameplate / 1-2-3 black-dot split test (Oct 21). Argument:
rem   scale1 = D3D11 renderer at native resolution (PS2X_GS_SCALE=1). Clean = upscaled render-to-texture readback bug.
rem   cpu    = CPU GS renderer. Clean = a D3D11-only bug; dots here too = GS emulation / texture data.
rem p07 stays off (leg-shake fix) so both runs look like the next build.
if "%~1"=="" (echo usage: %~nx0 scale1^|cpu & exit /b 1)
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_NATIVE_P07=0
rem hang profiler: a stuck load logs [hctpstack] (the entrance-start hang, HANDOFF OPEN #1)
set HCTP_PCPROF=1
if /i "%~1"=="scale1" set PS2X_GS_SCALE=1
if /i "%~1"=="cpu" set HCTP_RENDERER=cpu
"F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\nameplate_%~1.err"

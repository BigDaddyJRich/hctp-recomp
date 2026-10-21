@echo off
rem Leg-shake split test 4 (Oct 21): native exe, all native defaults ON except the one switch named as the argument,
rem e.g. run_legshake_off.bat HCTP_NATIVE_ADX  (candidates left: HCTP_NATIVE_ADX, HCTP_NATIVE_P07, HCTP_NATIVE_LINES)
if "%~1"=="" (echo usage: %~nx0 SWITCH_NAME & exit /b 1)
cd /d "F:\HCTP MODS\Vanilla"
set %~1=0
"F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\legshake_off_%~1.err"

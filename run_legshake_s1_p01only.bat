@echo off
rem Leg-shake split test 3 (Oct 21): native exe, all native defaults ON except stage-1 skinning limited to the verified
rem p01 (p02 3-bone / p03 2-bone / p04 rigid back on the VU1, no ANYHASH). Steady = p02-p04 native skinning is the shake.
cd /d "F:\HCTP MODS\Vanilla"
set HCTP_NATIVE_S1=1
set HCTP_S1_ANYHASH=0
"F:\hctp-newbase\PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe" SLUS_207.87 2> "F:\hctp-newbase\logs\legshake_s1_p01only.err"

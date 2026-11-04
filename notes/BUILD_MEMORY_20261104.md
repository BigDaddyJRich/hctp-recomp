# Player build memory (Nov 4 2026) - measured

Measured with psapi peak working set / peak commit per cl.exe (kit flags /O2 /Ob1 /arch:AVX2, vulcan runtime sources,
HCTP runner 14.8k files). Machine: 16 cores.

## Before (unity batches of 32, big files inside them)
| compile | peak commit |
|---|---|
| median unity batch (unity_274, 0.3 MB) | 0.20 GB |
| unity_433 (register_functions.cpp 40 MB + 31) | 1.76 GB |
| unity_460 (3 files 1.2-2.4 MB) | 5.22 GB |
| **unity_459 (sub_00419D90 5 MB, 4439F0 3.7 MB, 45D340 3.1 MB ...)** | **27.57 GB** <- testers' OOM |

Big files alone at /O2 /Ob1: 0.6-4 GB for 1-2.3 MB files; sub_0045D340 5.6, sub_004439F0 10.3, sub_00419D90 12.5 GB;
register_functions.cpp alone 1.7 GB (one ctor of 900k stores - NOT the problem); vu1_compiled.cpp (/Ob2) 3.5 GB.
sub_00419D90 by flag: /O1 12.5, /Ot/Oi/Oy 12.5, /Od 0.51, **/O2 /Ob0 0.53 GB** -> the cost is inlining runtime helpers
into a multi-MB function. Those functions are load-time code (~0.5% in sampler runs).

## Fix (PS2Recomp ps2xRuntime/CMakeLists.txt, in F:\hctp-newbase-vulcan, UNCOMMITTED)
Runner sources > PS2X_RUNNER_HEAVY_SOURCE_BYTES (1 MiB) leave the unity batches and (Ninja) compile in object
library ps2EntryRunnerHeavy with /Ob0 (PS2X_RUNNER_HEAVY_INLINE) in job pool ps2x_heavy = PS2X_RUNNER_HEAVY_JOBS (1).
Outer tools/setup_game.py: game build runs BELOW_NORMAL priority (compilers inherit it).

## After - full fresh build, kit flags, -j5 (what build_jobs picks on 16 GB)
F:\hctp-newbase-vulcan\out\memtest: exit 0, 1598 s. Sum of all cl/link working sets (2 s samples): peak **5.3 GB**;
largest single cl.exe 2.3 GB; link.exe 0.96 GB. -> fits 8 GB at -j2 and 16 GB at -j5 with room to spare.

## To ship
Commit the CMake change in PS2Recomp and bump the outer PS2Recomp submodule (already a release blocker), then
rebuild the kit (make_release_hctp takes PS2Recomp at its committed HEAD). Game not booted from the memtest exe
(no code change, only inline level of 13 load-time functions) - Jordan check: boot + a match.

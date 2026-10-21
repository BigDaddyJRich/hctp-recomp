# HCTP native build - Codex handoff (Wed Oct 21 2026, from Claude)

Jordan gives Codex permission to work directly on the NATIVE build in `F:\hctp-newbase` (not "native copy").
Goal: finish the last bugs, then produce the release executable + launcher.
Rules that still apply (from F:\hctp-project\CLAUDE.md + memory):
- Batch changes, then ONE build and one test. Don't build per fix.
- PCSX2 source (`F:\pcsx2-master\pcsx2-master`) is the behaviour reference.
- Never ship game data. Never push generated runner code. Ask Jordan before pushing or publishing anything.
- Don't regress: caution/memory-card screen, menu flicker, interlace/shimmy fix.
- Game files: `F:\HCTP MODS\Vanilla` (Jordan's working install; don't use other HCTP folders).
- Logs are big: grep them, never read whole.

## Build / run
1. Copy `overrides_hctp\hctp_overrides.cpp` and every `overrides_hctp\*.inl` into `PS2Recomp\ps2xRuntime\src\runner\`
   (the build compiles the runner copy).
2. `cmd /c F:\hctp-newbase\build-native.bat` -> `PS2Recomp\out\native\ps2xRuntime\ps2EntryRunner.exe`.
3. Run from the game folder: `cd /d "F:\HCTP MODS\Vanilla"` then `...\out\native\ps2xRuntime\ps2EntryRunner.exe SLUS_207.87 2> log.err`.
- `out\fast` (Oct 14) and `out\diag` (current source, raylib + paraLLEl-GS, `build-diag.bat`) are reference builds. Keep them.
- Built-in native defaults: `PS2Recomp\ps2xRuntime\src\lib\ps2_host_settings.cpp` (~line 437). The environment overrides them.

## UNBUILT changes waiting for the next build (uncommitted, in PS2Recomp)
1. LEG SHAKE fix: `ps2_host_settings.cpp` default `HCTP_NATIVE_P07` = "0" (was "1").
2. MENU/HUD FONT DOTS fix: `gs/gs_cpu_backend.cpp` `QueueBatchUnlocked` + `include/runtime/gs/gs_cpu_backend.h`
   `PageRange.layout`. Adds a write-after-write hazard. Queued draws that write the same GS pages through a
   different layout (base/width/page shape) now flush the queue first. Before this, band threads raced on the Z24
   buffer whose top byte holds the PSMT4HL/HH font + HUD textures.
   Verify: main menu, Select Stage "SmackDown! A", in-match HUD text have no black dots WITH threads on (default).
   Jordan already confirmed `VENDETTA_GS_THREADS=1` makes them clean.
Build these first, test, then commit (PS2Recomp, then bump the submodule in the outer repo).

## Remaining bugs / tasks (in order)
1. **Entrance nameplate dots** (SmackDown entrance plates, 1-2-3 graphic). NOT fixed by the font fix (still there
   with 1 GS thread). Not D3D11-only, not upscaling (still there at `PS2X_GS_SCALE=1`). PCSX2 and the paraLLEl-GS
   build are clean. Notes say the dots are baked into the plate's texture (a render-to-texture composite).
   Method that worked for the fonts: `HCTP_VRAM_DUMP_SEC=N` dumps 4 MB GS memory (`vram_dump.bin` in the game
   folder). Dump the same screen in native (`HCTP_RENDERER=cpu`) and diag (`run_nameplate_vramdump.bat native|diag`
   uses 150 s). Then diff per 8 KB page, and per word top byte vs low 24 bits. Systematic patterns point at the
   bad write (local->local copy, sprite edge rules, CLUT, PSMCT24/Z24 RMW, etc).
   Compare against PCSX2 GSState / GSLocalMemory.
2. **p07 native morph weights - make it correct, then re-enable** (frame rate). Confirmed cause of the leg/arm/finger
   shake by elimination: `HCTP_NATIVE_P07=0` = steady. The 0x2800 output verifies 100%, so the bug is a side effect.
   Likely: VU1 microprogram 0x2908 (target blend, still on the VU1) reads VF/VI registers or flags that the skipped
   0x2800 run would have left set. Fix: reproduce that end state in `runP07` (`hctp_native_vu1.cpp` ~1196), or port 0x2908 too.
3. **Native VU1 side-effect audit** (prevents more p07-style bugs). For every native program in `hctp_native_vu1.cpp`
   (p01-p04 stage 1, p08, p09, p10, p11, p14-p19, lines), check the VU1 state it leaves vs the real program:
   all VF/VI/flags/ACC + data memory, not just the output buffer. Extend `HCTP_NATIVE_VERIFY` to run the real VU1
   after the native one and compare full state, then log the mismatching program. One test match covers all of them.
   Anything that differs gets fixed or goes back to the VU1 by default.
4. **Entrance-start hang** (intermittent; game stays on the pre-entrance loading screen, e.g. Chris Jericho).
   See HANDOFF_NEXT_CHAT.md OPEN #1: CRI ADXT svm_lock blocked in ADXT_ExecRdCompChk, suspected native ADX hooks vs
   lock pairing. `HCTP_PCPROF=1` logs `[hctpstack]` on a stall.
5. **Memory card saves** (mc0 empty - saves don't persist).
6. **Hogan**: SYM models as attires (decided Oct 21: no body+gear merge), entrance lights, shirt tear.
   See F:\hctp-project\notes\HANDOFF_NEXT_CHAT.md "HOGAN" sections.
7. **Commentary boost** (quiet clips) - notes\COMMENTARY_HANDOFF.md in F:\hctp-project.
8. **Release roster default**: vanilla roster by default. Restored cut wrestlers only through a launcher option
   (and only when their files are present).
9. **Extras pack**: one optional download of all non-vanilla content (models, TitanTrons, music, portraits). No
   commentary, no other modders' models without their OK.
10. **Frame rate pass** (60 fps target): re-enable p07 once fixed (task 2), keep the threaded GS, profile with `HCTP_PCPROF`.
11. **Release executable + launcher**:
    - Release build of the native exe (`build-native.bat` config, RelWithDebInfo /O2 /arch:AVX2).
    - Launcher (`settings-app` folder exists): pick the game folder, resolution/fullscreen/vsync (`hctp_native.ini`),
      roster option (task 8), extras pack on/off, starts the exe from the game folder.
    - Package: exe + launcher + readme. NO game files, NO SLUS ELF, NO ISO data.
11b. **Commentary from the player's OWN discs (required for release; Jordan, Oct 21)**. Today `Build HCTP.bat` /
    `tools/hctp_setup.py` only takes the HCTP disc; commentary data (BGM\PBPSVR*, PBPBETA*, PBPEXTRA*, FINKEL*, PBP_*.csv)
    is built by hand on Jordan's PC and never shipped. Make it a setup step:
    - `hctp_setup.py --extra <iso|chd|folder>` (repeatable; also drag several files onto the bat): identify each disc by
      its serial / key files (SvR 2004 retail, SvR06, SvR07, SvR08, Shut Your Mouth, LoW, 2K14 PS3...) and skip unknowns
      with a clear message. Optional: a missing disc only removes that source's clips (the engine already copes).
    - Move the builders from `F:\hctp-project\tools` into this repo's `tools/commentary/` (build_pbp_extra.py,
      build_finkel_pack.py, map_moves.py, build_line_owners.py, build_banter_drop.py, build_banter_pairs.py) and make
      them take paths from the setup instead of Jordan's folders. Transcripts/CSVs that are OUR work (line tables,
      move map rules, owners, banter pairs) ship; audio and anything copied from a disc never ship.
    - Read F:\hctp-project\notes\COMMENTARY_HANDOFF.md first (data layout, partitions 13-16, rules), plus the memory
      rules: SvR retail+beta first, other games only for gaps; Finkel from SYM/LoW/2K14; name rules (Valbowski=Val Venis...).
    - The SvR public BETA clips: find out whether players can get that build; if not, ship without it (retail covers it).
    - Check: fresh folder + HCTP disc + one extra disc -> BGM\ files built, `[hctp-pbp] loaded N tables` in the log.
12. **Final QA pass**: boot, menus, CAM round trip, exhibition match with entrances, season, saves.

## Test launchers made this session (`F:\hctp-newbase`)
`run_legshake_diag.bat` (diag build), `run_legshake_native_nodefaults.bat`, `run_legshake_s1_p01only.bat`,
`run_legshake_off.bat <SWITCH>`, `run_nameplate_test.bat scale1|cpu`, `run_nameplate_vramdump.bat native|diag`,
`run_nameplate_1thread.bat`. Dumps from this session: `logs\vramdump_native`, `logs\vramdump_diag`.

## Where the history is
`F:\hctp-project\notes\HANDOFF_NEXT_CHAT.md` (PENDING list at the top + OPEN list) and `PROGRESS_NOTES.md` (newest at the bottom).

# WWE SmackDown! Here Comes the Pain — PC recompilation (work in progress)

A static recompilation of **WWE SmackDown! Here Comes the Pain** (PlayStation 2, NTSC-U, **SLUS-20787**) into a native
Windows program. The game's MIPS code is translated to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and
compiled on your PC. The main renderer is a native **Direct3D 11** GS backend with the game's VU1 programs rewritten as
native code; [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (Vulkan) and the
[ps2-native-gs](https://github.com/SirBraga/ps2-native-gs) module remain as optional/reference backends. The IOP
(sound / disc processor) runs the game's own IRX modules.

This project is built on **[Def Jam Vendetta Recompiled](https://github.com/jcsix694/def-jam-vendetta-recomp)** by
jcsix694 (its original README: [README_VENDETTA.md](README_VENDETTA.md)), adapted for HCTP.

**No game files or game code are in this repository.** You need your own copy of the game; everything generated from
it (the recompiled C++ in `PS2Recomp/ps2xRuntime/src/runner/` and `generated/`) stays on your PC and is never committed.

## Status (late October 2026)

Working (D3D11 renderer, tested by hand on an RTX 3060):
- Boot, ATTENTION / memory-card screens, Sofdec movies (intro, menus, Titantrons, full-screen entrance videos)
- All menus, character select, Create-a-Moveset, Season mode
- Entrances and matches at a steady 60 fps, including full arena entrances (lights, flares, Titantron)
- Music, crowd, sound effects (native libsdr -> the game's real sdrdrv IRX) and an in-progress commentary system
- Recent fixes: the **wrestler shake** (a runtime dispatcher bug that dropped the last bones of the skeleton copy),
  entrance slowdowns (per-read GPU depth stalls), entrance nameplate holes, menu/HUD font dots, rope-shadow depth

Known issues (being worked on):
- Rope shadows sit too high; small frame-pacing hitches in some menus
- Black screen when leaving the attract-mode demo with Start (the title movie does not restart)
- Hulk Hogan's modern entrance (restored content)
- Memory-card saving not fully tested; multitap / more than two controllers

Plan: finish correctness and release a playable build, then move more of the rendering native (draw wrestlers and
arenas directly from the game's model data instead of through the emulated GS), then new features (restored roster,
widescreen, commentary).

## Layout

| Path | What |
|---|---|
| `PS2Recomp/` | submodule: the recompiler + runtime, branch `hctp` (EE scheduler, VU1, GS front-end, IOP, SIF, libsdr ...) |
| `overrides_hctp/` | HCTP-specific runtime overrides (heap, CD search, movie hooks, native libsdr hookup, profiler) |
| `config_hctp/` | recompiler configuration for SLUS_207.87 (function names / stub lists only) |
| `tools/` | `vu1_recomp.py` (VU1 microcode -> C++), `autotest.ps1` (automated boot + attract-demo test with screenshots and audio dump), `audio_check.py` (stereo offset / click analysis) |
| `run_hctp.bat` | launcher with the usual diagnostics |

## Building

Same toolchain as Def Jam Vendetta Recompiled (Visual Studio 2022+/MSVC x64, CMake, Ninja, Python, Vulkan-capable GPU):
1. Put your own unmodified `SLUS_207.87` and the disc files in a folder.
2. Recompile it with `PS2Recomp` using `config_hctp/hctp.toml` (generates the runner C++ locally).
3. `HCTP_VU1_RANGES="0x44c6e4:0x44eb70,0x4526a4:0x452d08" python -I tools/vu1_recomp.py SLUS_207.87 generated/vu1_compiled.cpp`
4. `build-native.bat` (the D3D11 native build), or `build-fast.bat` / `build-runtime.bat` for the paraLLEl-GS builds.
5. Run from the game folder: `ps2EntryRunner.exe SLUS_207.87` (see `run_hctp.bat` for useful environment variables).

## Credits

- [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) — the recompiler and runtime this is built on
- [jcsix694/def-jam-vendetta-recomp](https://github.com/jcsix694/def-jam-vendetta-recomp) — the base project (IOP emulation, paraLLEl-GS integration, VU1 compiler, tools)
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) — GS on Vulkan
- [SirBraga/ps2-native-gs](https://github.com/SirBraga/ps2-native-gs) — optional native GS module (GPL-3.0)
- [PCSX2](https://github.com/PCSX2/pcsx2) — hardware behaviour reference
- BT3-Recomp, Ace Combat 5 static recompilation, NBA Street V3 recomp — techniques and references

## License

GPL-3.0, as the base project (see `LICENSE` and `THIRD-PARTY-NOTICES.txt`). WWE SmackDown! Here Comes the Pain is
(c) its owners; this project contains none of its files or code.

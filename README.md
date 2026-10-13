# WWE SmackDown! Here Comes the Pain — PC recompilation (work in progress)

A static recompilation of **WWE SmackDown! Here Comes the Pain** (PlayStation 2, NTSC-U, **SLUS-20787**) into a native
Windows program. The game's MIPS code is translated to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and
compiled on your PC. The PS2 graphics chip is drawn on the GPU with Vulkan through
[paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), and the IOP (sound / disc processor) runs the game's
own IRX modules.

This project is built on **[Def Jam Vendetta Recompiled](https://github.com/jcsix694/def-jam-vendetta-recomp)** by
jcsix694 (its original README: [README_VENDETTA.md](README_VENDETTA.md)), adapted for HCTP.

**No game files or game code are in this repository.** You need your own copy of the game; everything generated from
it (the recompiled C++ in `PS2Recomp/ps2xRuntime/src/runner/` and `generated/`) stays on your PC and is never committed.

## Status (October 2026)

Working:
- Boot, ATTENTION / memory-card screens, real Sofdec movies (intro, menus) with ADX audio
- All menus, character select, Create-a-Moveset, Season mode (cutscenes, locker room, backstage)
- Entrances and matches in the ring (exhibition, season, attract-mode demo)
- Music, commentary/crowd audio, **menu and in-game sound effects** (native libsdr -> the game's real sdrdrv IRX)
- Memory card: existing save data is detected and offered for loading (saving not fully tested yet)

Known issues (being worked on):
- Frame rate in entrances / matches: VU1 (vertex processing) is the bottleneck
- In-ring only: wrestler gloss/specular pass missing and 3D crowd placed wrong with the fast VU1 model
  (correct with `VENDETTA_VU1_FAST=0`, which is slow) — being bisected per VU1 program
- Some objects vanish / stretched polygons near the camera; season dialogue font garbled
- Movie (Sofdec) audio: right channel offset by 8192 samples (fix in testing)

Plan: make the game correct and fast on paraLLEl-GS, then a **native renderer** (hook the game's draw calls and
render wrestlers / arenas with Vulkan directly, following the BT3 and Ace Combat 5 recomps), then new features.

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
4. `build-fast.bat` (quick, for development) or `build-runtime.bat` (fully optimised).
5. Run from the game folder: `ps2EntryRunner.exe SLUS_207.87` (see `run_hctp.bat` for useful environment variables).

## Credits

- [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) — the recompiler and runtime this is built on
- [jcsix694/def-jam-vendetta-recomp](https://github.com/jcsix694/def-jam-vendetta-recomp) — the base project (IOP emulation, paraLLEl-GS integration, VU1 compiler, tools)
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) — GS on Vulkan
- [PCSX2](https://github.com/PCSX2/pcsx2) — hardware behaviour reference
- BT3-Recomp, Ace Combat 5 static recompilation, NBA Street V3 recomp — techniques and references

## License

GPL-3.0, as the base project (see `LICENSE` and `THIRD-PARTY-NOTICES.txt`). WWE SmackDown! Here Comes the Pain is
(c) its owners; this project contains none of its files or code.

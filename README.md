# WWE SmackDown! Here Comes the Pain — PC recompilation (work in progress)

A static recompilation of **WWE SmackDown! Here Comes the Pain** (PlayStation 2, NTSC-U, **SLUS-20787**) into a native
Windows program. The game's MIPS code is translated to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and
compiled on your PC. The PS2 graphics chip is drawn on the GPU with Vulkan through
[paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), and the IOP (sound / disc processor) runs the game's
own IRX modules.

The runtime uses upstream PS2Recomp components and the paraLLEl-GS backend. HCTP-specific code and configuration live
in the HCTP-named directories in this repository.

**No game files or game code are in this repository.** You need your own copy of the game; everything generated from
it (the recompiled C++ in `PS2Recomp/ps2xRuntime/src/runner/` and `generated/`) stays on your PC and is never committed.

## Current status (October 2026)

Working:
- Boot, ATTENTION / memory-card screens, real Sofdec movies (intro, menus) with ADX audio
- All menus, character select, Create-a-Moveset, Season mode (cutscenes, locker room, backstage)
- Entrances and matches in the ring (exhibition, season, attract-mode demo)
- Music, commentary/crowd audio, **menu and in-game sound effects** (native libsdr -> the game's real sdrdrv IRX)
- Memory-card screens and save/load paths are working in the native test path; saves are stored in the PC runtime's
  user-data area rather than requiring a PCSX2 card image.
- A Windows test package is produced separately from this source repository. It contains the launcher, native runner,
  optional media/mod content, and setup instructions; copyrighted game files and commentary source audio are never
  redistributed.

Fixed since the first release (Oct 13-14):
- Garbled in-match HUD / season dialogue text (PSMT4HH/PSMT8H upload sizes)
- Crowd piled in the ring, missing skin gloss, stray grey slabs (libvu0 stubs)
- Runaway VU1 programs / garbage polygons (JR/JALR branch fix); all 20 VU1 programs now compiled
- Exhibition match load softlock (IOP heap), whole-screen shimmy, 5-9 s in-match freezes, in-match hit sounds
- Frame rate: demo match ~10 fps -> 32-44 (VU1 throughput mode, on by default; `HCTP_VU1_FAST=0` = exact)
- Optional native music (`run_hctp_native_audio.bat`, `HCTP_NATIVE_ADX=1`): ADX streams decoded on the PC

Known limitations:
- Frame rate: not yet a steady 60 in matches
- Intro movie hangs at boot now and then (press START to skip the intro)
- ADX music can stop after several minutes on the emulated IOP path; the native music path is still being tested
- Right audio channel late / weak stereo separation; weak rumble
- Shadows under the wrestlers show lines / visible triangles

Near-term plan: stabilize the public Windows test package, improve native audio and commentary coverage, and continue
the native-renderer investigation without regressing the working paraLLEl-GS path.

## Layout

| Path | What |
|---|---|
| `PS2Recomp/` | submodule: the recompiler + runtime, branch `hctp` (EE scheduler, VU1, GS front-end, IOP, SIF, libsdr ...) |
| `overrides_hctp/` | HCTP-specific runtime overrides (heap, CD search, movie hooks, native libsdr hookup, profiler) |
| `config_hctp/` | recompiler configuration for SLUS_207.87 (function names / stub lists only) |
| `tools/` | `vu1_recomp.py` (VU1 microcode -> C++), `autotest.ps1` (automated boot + attract-demo test with screenshots and audio dump), `audio_check.py` (stereo offset / click analysis) |
| `run_hctp.bat` | normal HCTP developer runner |
| `developer/runs/` | optional cycle-exact, field-inversion, native-audio, and slow-VU1 diagnostic runners |

## Building

Toolchain: Visual Studio 2022+/MSVC x64, CMake, Ninja, Python, and a Vulkan-capable GPU:
1. Put your own unmodified `SLUS_207.87` and the disc files in a folder.
2. Recompile it with `PS2Recomp` using `config_hctp/hctp.toml` (generates the runner C++ locally).
3. `HCTP_VU1_RANGES="0x44c6e4:0x44eb70,0x4526a4:0x452d08" python -I tools/vu1_recomp.py SLUS_207.87 generated/vu1_compiled.cpp`
4. `build-fast.bat` (quick, for development) or `build-runtime.bat` (fully optimised).
5. Run from the game folder: `ps2EntryRunner.exe SLUS_207.87` (see `run_hctp.bat` for useful environment variables).

## Credits

- [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) — the recompiler and runtime this is built on
- upstream PS2Recomp components — recompiler, runtime, IOP/SIF support, and related tooling
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) — GS on Vulkan
- [PCSX2](https://github.com/PCSX2/pcsx2) — hardware behaviour reference
- BT3-Recomp, Ace Combat 5 static recompilation, NBA Street V3 recomp — techniques and references

## License

GPL-3.0, as the base project (see `LICENSE` and `THIRD-PARTY-NOTICES.txt`). WWE SmackDown! Here Comes the Pain is
(c) its owners; this project contains none of its files or code.

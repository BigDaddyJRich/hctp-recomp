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

Working in the native Windows build:

- The recompiled game runs natively on Windows. The game code is recompiled C++, the graphics path is GPU-backed
  through paraLLEl-GS, and the audio path runs through the native runtime/IOP integration rather than PCSX2.
- Native video playback works for the game's Sofdec movies, with the game's ADX audio path available alongside the
  native music option.
- Native game audio works across music, commentary, crowd audio, menu sound effects, and in-match sound effects.
- Gameplay runs at 60 FPS in matches, menus and full-arena entrances on the tested configuration (D3D11 native
  renderer, RTX 3060).
- Boot, loading, menus, character selection, Create-a-Moveset, Season mode, cutscenes, locker-room/backstage scenes,
  entrances, exhibition matches, and attract-mode demonstrations are functional.
- Commentary is playable in matches, including the HCTP commentary restoration path. The public setup plan supports
  HCTP, Shut Your Mouth, SmackDown vs. Raw, and the public beta as the core sources, with later games as optional
  supplements.
- Memory-card screens and save/load paths work in the native test path. PC builds use a user-data save location rather
  than requiring a PCSX2 memory-card image.
- A Windows test package is produced separately from this source repository. It contains the launcher, native runner,
  optional media/mod content, and setup instructions; copyrighted game files and commentary source audio are never
  redistributed.
- PS2 modders: see [`docs/COMMENTARY_PS2_PORTING.md`](docs/COMMENTARY_PS2_PORTING.md) for the documented pack format,
  hook addresses, and the remaining ELF code-cave work needed for original-hardware commentary.

Fixed since the first release (Oct 13-14):
- Garbled in-match HUD / season dialogue text (PSMT4HH/PSMT8H upload sizes)
- Crowd piled in the ring, missing skin gloss, stray grey slabs (libvu0 stubs)
- Runaway VU1 programs / garbage polygons (JR/JALR branch fix); all 20 VU1 programs now compiled
- Exhibition match load softlock (IOP heap), whole-screen shimmy, 5-9 s in-match freezes, in-match hit sounds
- Frame rate: demo match ~10 fps -> 32-44 (VU1 throughput mode, on by default; `HCTP_VU1_FAST=0` = exact)
- Optional native music (`developer/runs/run_hctp_native_audio.bat`, `HCTP_NATIVE_ADX=1`): ADX streams decoded on the PC
- Right-channel audio timing and weak stereo separation
- Wrestler shadow artifacts / visible triangles
- Missing skin gloss, stray grey slabs, and crowd geometry regressions
- Runaway VU1 programs, garbage polygons, and incomplete VU1 program coverage
- Exhibition loading softlocks, long in-match freezes, and missing in-match hit sounds

Fixed Oct 26:
- Wrestler / referee shake (runtime dispatcher dropped the last bones of the skeleton copy after a checkpoint)
- Entrance frame-rate drops (per-read GPU depth stalls in the flare visibility test)
- Holes in the entrance nameplate (depth readback overwrote the texture stored in the Z buffer's top byte)

Known limitations:
- Rope shadows sit too high
- Leaving the attract-mode demo with Start can leave a black screen (title movie does not restart)
- Hulk Hogan's modern entrance (restored content) is still being finished

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

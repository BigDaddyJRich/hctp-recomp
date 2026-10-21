![HCTP Recompiled](images/hctp_recompiled_banner.webp)

# WWE SmackDown! Here Comes the Pain — PC recompilation (work in progress)

A static recompilation of **WWE SmackDown! Here Comes the Pain** (PlayStation 2, NTSC-U, **SLUS-20787**) into a native
Windows program. The game's MIPS code is translated to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and
compiled for the PC. It now runs on its own **native Windows host**: a Direct3D 11 renderer for the PS2 graphics chip,
native C++ versions of the game's VU1 vertex programs, XInput controllers and XAudio2 sound. No emulator window or
Vulkan is needed.

Built on **[Def Jam Vendetta Recompiled](https://github.com/jcsix694/def-jam-vendetta-recomp)** by jcsix694 (original
README: [README_VENDETTA.md](README_VENDETTA.md)), adapted for HCTP.

**No game files are in this repository or in any release.** You need your own copy of the game.

## Status (October 19, 2026)

Working:
- Boot, ATTENTION / memory-card screens, Sofdec movies, all menus, character select, Create-a-Moveset, Season mode
- Entrances and full matches (exhibition, season, attract-mode demo); matches run at about 59-60 fps, with dips into
  the 40s
- Direct3D 11 renderer with the GS's blending, alpha/destination-alpha tests and depth rules (HUD, overlays and shadows
  fixed)
- Native VU1 programs, checked against the real VU1 microcode output: skinning (CV1-CV4), TriStrip with clipping,
  character strips, environment maps, the 3D crowd (100% match over 134 million qwords), particles and morph
  weights (100% over 77 million qwords)
- Native ADX audio: music, commentary, crowd, ambience and in-memory sound effects

Known bugs:
- Wrestlers' calves can twitch (seen during some holds/submissions)
- Rope shadows can fall on other objects and be seen from below the ring ropes
- Frame rate dips at boot and in some match scenes
- The memory card should start as a clean, empty card
- Movie audio: the right channel lags behind; a movie can occasionally hang

Fixed recently: the red bar on the main menu (a developer memory meter the menu draws), the "Now Loading" icon
flicker after matches.

Not native yet (still run through the VU1 / IOP emulation, so they work but are slower):
- VU1: clipped variants of some strip programs and part of the particle cases. NonClip, TriList, LgMap,
  ProjectionMap and line programs are written and on, but not yet checked in other arenas or modes; the
  shadow-volume program is written but off until a mode that uses it is tested
- Sound: Sony Standard Kit effects (HSYNTH) and movie audio

Next steps: fix the bugs above, finish the native VU1 programs, add native synth and movie playback, then remove the IOP
emulation. After that come resolution scaling, widescreen and optional content packs.

## Playing

The release is a **build kit**: it builds the game on your PC from your own disc. Neither the download nor this
repository contains game files or the game's code.

1. Download `HCTP-Recomp-Windows.zip` from Releases and unzip it to a short path (e.g. `C:\Games\HCTP Recomp`).
2. Drag your disc image (`.iso` / `.chd`, USA SLUS-20787) or your game folder (the one with `SLUS_207.87`) onto
   `Build HCTP.bat`. If Microsoft's free C++ build tools are missing, they are installed first. The first build needs
   the internet and takes roughly 30-90 minutes.
3. Start the game with `Play HCTP.bat`. Native settings are built in; `HCTP_NATIVE_DEFAULTS=0` turns them all off,
   for comparison.

Needs Windows 10/11 x64, a CPU with AVX2, a Direct3D 11 GPU, about 16 GB of RAM and about 15 GB of free space.

## Layout

| Path | What |
|---|---|
| `PS2Recomp/` | submodule: recompiler + runtime (EE, VU1, GS front-end, D3D11 renderer, native VU1 programs, IOP, audio) |
| `overrides_hctp/` | HCTP-specific runtime overrides (heap, CD search, movie hooks, native ADX, profiler) |
| `config_hctp/` | recompiler configuration for SLUS_207.87 (function names / stub lists only) |
| `tools/` | `vu1_recomp.py`, `autotest.ps1` (automated boot + match test), `audio_check.py`, Ghidra query helpers |
| `build-native.bat` | builds the native Windows host into `PS2Recomp\out\native` |

## Building

Visual Studio 2022+ (MSVC x64), CMake, Ninja, Python.
1. Put your own unmodified `SLUS_207.87` and the disc files in a folder.
2. Recompile it with `PS2Recomp` using `config_hctp/hctp.toml` (this generates the runner C++ on your PC).
3. `HCTP_VU1_RANGES="0x44c6e4:0x44eb70,0x4526a4:0x452d08" python -I tools/vu1_recomp.py SLUS_207.87 generated/vu1_compiled.cpp`
4. `build-native.bat`.

## Credits

- [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp): the recompiler and runtime
- [jcsix694/def-jam-vendetta-recomp](https://github.com/jcsix694/def-jam-vendetta-recomp): the base project
- Kaiko's Crush Hour port: native D3D11 PS2Recomp port, used as a renderer reference
- [PCSX2](https://github.com/PCSX2/pcsx2): hardware behaviour reference
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs): earlier GS backend, used as a reference
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) + ghidra-emotionengine-reloaded: game code analysis
- [FFmpeg](https://ffmpeg.org): video decoding (LGPL prebuilt DLLs, downloaded during the build)
- BT3-Recomp, Ace Combat 5 static recompilation, NBA Street V3 recomp: techniques and references

## License

GPL-3.0, as the base project (see `LICENSE` and `THIRD-PARTY-NOTICES.txt`). WWE SmackDown! Here Comes the Pain is
(c) its owners; this repository contains none of its files.

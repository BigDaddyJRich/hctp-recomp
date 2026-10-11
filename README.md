# def-jam-vendetta-recomp

**Def Jam Vendetta (PS2, USA) recompiled into a native PC program** with
[PS2Recomp](https://github.com/ran-j/PS2Recomp): the game's PS2 code is translated to C++ and built against a runtime
that stands in for the PS2's hardware. The goal is a version of the game where the PS2's limits (memory, disc size,
fixed tables) can be lifted, and, later, where [adjmk](https://github.com/jcsix694/adjmk) mods can run without them.

Made by [@jcsix on X](https://x.com/jcsix) · [jcsix694 on GitHub](https://github.com/jcsix694).

> **Status: work in progress.** The game boots, plays its movies, runs its menus and plays a full match at 60 frames/s
> with music, sound effects and two pads, drawn on the graphics card at up to 16× the PS2's detail. Not finished yet:
> story mode, saving, reverb.

**No game files or game code are in this repository.** You need your own Def Jam Vendetta disc (USA,
`SLUS-20639`), as a `.chd` or `.iso`. Everything made from it stays on your PC.

## Build it

Needs Windows, [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) (the "Desktop
development with C++" workload), Python 3 and git. To play: a graphics card with Vulkan 1.3 (any recent NVIDIA, AMD
or Intel Arc card); without one the game draws on the CPU at the PS2's resolution.

```bat
git clone https://github.com/jcsix694/def-jam-vendetta-recomp
cd def-jam-vendetta-recomp
setup.bat                                        :: the submodules, PS2Recomp + our patch, the recompiler
python tools\prepare_disc.py "D:\path\to\Def Jam Vendetta.chd"    :: or the .iso
recomp.bat                                       :: the game's code → C++ (~2 min)
build-fast.bat                                   :: build it (first time ~12 min)
run.bat                                          :: run it
build-settings.bat                               :: the settings program (needs the .NET 10 SDK)
make-release.bat                                 :: a player's zip in dist\ (nothing is published)
```

**A player's zip** (`make-release.bat`) has the settings program, a portable Python and the source (PS2Recomp with
our patch applied, so no git). Opened the first time, Vendetta Settings asks for the disc, installs Microsoft's C++
build tools if they're missing (one Windows prompt) and builds the game with a progress bar
(`tools\setup_game.py`); the game program, `game\` and the saves stay in the unzipped folder.

`prepare_disc.py` checks it's the right disc, then writes `disc\SLUS-20639.iso` (the game reads its disc by sector)
and `game\` (the disc's files). Clone without `--recurse-submodules`: `setup.bat` fetches only the parts of
paraLLEl-GS's own submodules it needs (a full recursive clone runs into Windows' path-length limit).

### Settings

The game reads `settings.toml` from its folder when it starts (written with the defaults, a comment on each line, the
first time). **Vendetta Settings** (`build-settings.bat`, then `settings-app\bin\Release\net10.0\Vendetta Settings.exe`)
changes it in a window — display (window / borderless / fullscreen, screen, resolution, refresh rate, VSync, picture
shape), graphics (with High Poly in every match) and sound — and has a PLAY button. A switch set before `run.bat` wins over the file
(e.g. `set VENDETTA_SSAA=16`):

| | |
|---|---|
| `VENDETTA_SSAA=1/2/4/8/16` | samples per pixel on the graphics card (`scale` in `settings.toml`, default 16 — four times the PS2's detail; 8 with no settings file); 4 and up also draw at twice the resolution |
| `VENDETTA_GS=cpu` | draw on the CPU instead (the PS2's own resolution) |
| `VENDETTA_FILTER=fsr/bilinear/nearest` | how the picture is scaled to the window: AMD FSR upscaling + sharpening on the graphics card (default), plain smoothing, or none (the blocky look) |
| `VENDETTA_SHARPEN=0..2` | FSR's sharpening (default 0.5; 0 = the most) |
| `VENDETTA_GPU=<number or name>` | which graphics card, e.g. `nvidia` (default: the dedicated one) |
| `VENDETTA_ASPECT=16:9` | stretch to widescreen |
| `VENDETTA_UPSCALE_SPRITES=1/0` | sharp 2-D: the HUD, text and menus super-sampled like the 3-D (`sharp_2d` in `settings.toml`, on by default) |
| `VENDETTA_HIRES4=0` | at 16× the picture is scanned out at 4× the PS2's resolution (each super-sample its own pixel); 0 = 2× as at 8× |
| `VENDETTA_KEYBOARD=1..4/0` | which player the keyboard plays as, with their controller (`keyboard` in `settings.toml`; 0 = nobody, the default). Keys: arrows / W A S D move, X ✕, Z □, C ○, V △, Q L1, E R1, Shift L2 / R2, Enter START, Tab SELECT, I J K L the right stick |
| `VENDETTA_HIGHPOLY=1/0` | High Poly wrestlers and arenas in every match (`high_poly`, on by default in `settings.toml`); 0 = as on the PS2, the lower-detail ones in 3- and 4-fighter matches |

The same settings can be changed while playing in the **Graphics** tab of the debug panel (hidden at start; F1 shows /
hides it; `VENDETTA_DEBUG_TAB=Graphics` opens it on that tab at start). The first start on a PC is slow for a minute while the graphics
card prepares its shaders.

## What's in here

| | |
|---|---|
| `config\` | the recompiler's inputs: the function map (from Ghidra), Sony library routines to replace with the runtime's own, function starts Ghidra missed |
| `overrides\` | game-specific runtime code (where the disc image is, debug dumps) |
| `patches\` | our fixes to PS2Recomp's runtime (applied by `setup.bat`) |
| `settings-app\` | Vendetta Settings: the game's settings in a window, and PLAY (Avalonia, .NET 10) |
| `third_party\parallel-gs` | [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), the PS2's graphics chip on the graphics card (submodule) |
| `tools\` | disc preparation, config building, the missing-function scan, the VU1 recompiler, debugging helpers |

## Licence

Copyright (C) 2026 jcsix694 (@jcsix on X). Free software under the **GNU General Public License version 3** (`LICENSE`): use it,
change it and share it; a changed version you share must come with its source, under the same licence.

It has to be the GPL: PS2Recomp (ran-j, GPL-3.0) is built into the game program, and our changes to it (`patches\`)
are under its licence; our changes to paraLLEl-GS (`patches\parallel-gs-local.diff`, applied by `setup.bat`) are
under its LGPL-3.0+. paraLLEl-GS (© Arntzen Software AS) is LGPL-3.0+ and uses Granite (© Hans-Kristian Arntzen,
MIT); both are included as submodules. Every other part and its licence: `THIRD-PARTY-NOTICES.txt`.

Def Jam Vendetta is © its owners. This project includes none of its files or code; the settings program's pictures
(`settings-app\Assets`) are artwork based on its logo, and the licence doesn't cover them.

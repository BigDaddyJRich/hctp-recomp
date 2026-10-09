# def-jam-vendetta-recomp

**Def Jam Vendetta (PS2, USA) recompiled into a native PC program** with
[PS2Recomp](https://github.com/ran-j/PS2Recomp): the game's PS2 code is translated to C++ and built against a runtime
that stands in for the PS2's hardware. The goal is a version of the game where the PS2's limits (memory, disc size,
fixed tables) can be lifted, and, later, where [adjmk](https://github.com/jcsix694/adjmk) mods can run without them.

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
```

`prepare_disc.py` checks it's the right disc, then writes `disc\SLUS-20639.iso` (the game reads its disc by sector)
and `game\` (the disc's files). Clone without `--recurse-submodules`: `setup.bat` fetches only the parts of
paraLLEl-GS's own submodules it needs (a full recursive clone runs into Windows' path-length limit).

### Picture settings

Set these before `run.bat` (e.g. `set VENDETTA_SSAA=16`):

| | |
|---|---|
| `VENDETTA_SSAA=1/2/4/8/16` | samples per pixel on the graphics card (default 8); 4 and up also draw at twice the resolution |
| `VENDETTA_GS=cpu` | draw on the CPU instead (the PS2's own resolution) |
| `VENDETTA_FILTER=fsr/bilinear/nearest` | how the picture is scaled to the window: AMD FSR upscaling + sharpening on the graphics card (default), plain smoothing, or none (the blocky look) |
| `VENDETTA_SHARPEN=0..2` | FSR's sharpening (default 0.5; 0 = the most) |
| `VENDETTA_GPU=<number or name>` | which graphics card, e.g. `nvidia` (default: the dedicated one) |
| `VENDETTA_ASPECT=16:9` | stretch to widescreen |

The same settings can be changed while playing in the **Graphics** tab of the debug panel (hidden at start; F1 shows /
hides it; `VENDETTA_DEBUG_TAB=Graphics` opens it on that tab at start). The first start on a PC is slow for a minute while the graphics
card prepares its shaders.

## What's in here

| | |
|---|---|
| `config\` | the recompiler's inputs: the function map (from Ghidra), Sony library routines to replace with the runtime's own, function starts Ghidra missed |
| `overrides\` | game-specific runtime code (where the disc image is, debug dumps) |
| `patches\` | our fixes to PS2Recomp's runtime (applied by `setup.bat`) |
| `third_party\parallel-gs` | [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), the PS2's graphics chip on the graphics card (submodule) |
| `tools\` | disc preparation, config building, the missing-function scan, the VU1 recompiler, debugging helpers |

## Licence

PS2Recomp is GPL-licensed and included as a submodule; our changes to it (`patches\`) are under the same licence.
paraLLEl-GS (© Arntzen Software AS) is LGPL-3.0+ and uses Granite (© Hans-Kristian Arntzen, MIT); both are included as
submodules.
Def Jam Vendetta is © its owners. This project includes none of it.

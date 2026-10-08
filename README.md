# def-jam-vendetta-recomp

**Def Jam Vendetta (PS2, USA) recompiled into a native PC program** with
[PS2Recomp](https://github.com/ran-j/PS2Recomp): the game's PS2 code is translated to C++ and built against a runtime
that stands in for the PS2's hardware. The goal is a version of the game where the PS2's limits (memory, disc size,
fixed tables) can be lifted, and, later, where [adjmk](https://github.com/jcsix694/adjmk) mods can run without them.

> **Status: work in progress.** The game boots, plays its movies, runs its menus and plays a full match at 60 frames/s
> with sound and two pads. Not finished yet: story mode, saving, some visuals and the in-game sound effects.

**No game files or game code are in this repository.** You need your own Def Jam Vendetta disc (USA,
`SLUS-20639`), as a `.chd` or `.iso`. Everything made from it stays on your PC.

## Build it

Needs Windows, [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) (the "Desktop
development with C++" workload), Python 3 and git.

```bat
git clone --recurse-submodules https://github.com/jcsix694/def-jam-vendetta-recomp
cd def-jam-vendetta-recomp
setup.bat                                        :: PS2Recomp + our patch, the recompiler
python tools\prepare_disc.py "D:\path\to\Def Jam Vendetta.chd"    :: or the .iso
recomp.bat                                       :: the game's code → C++ (~2 min)
build-fast.bat                                   :: build it (first time ~12 min)
run.bat                                          :: run it
```

`prepare_disc.py` checks it's the right disc, then writes `disc\SLUS-20639.iso` (the game reads its disc by sector)
and `game\` (the disc's files).

## What's in here

| | |
|---|---|
| `config\` | the recompiler's inputs: the function map (from Ghidra), Sony library routines to replace with the runtime's own, function starts Ghidra missed |
| `overrides\` | game-specific runtime code (where the disc image is, debug dumps) |
| `patches\` | our fixes to PS2Recomp's runtime (applied by `setup.bat`) |
| `tools\` | disc preparation, config building, the missing-function scan, the VU1 recompiler, debugging helpers |

## Licence

PS2Recomp is GPL-licensed and included as a submodule; our changes to it (`patches\`) are under the same licence.
Def Jam Vendetta is © its owners. This project includes none of it.

# WWE SmackDown! Here Comes the Pain — PC recompilation

A static recompilation of **WWE SmackDown! Here Comes the Pain** (PlayStation 2, USA, **SLUS-20787**) into a native
Windows game, built on your PC from your own disc. The game's code is translated to C++ with
[PS2Recomp](https://github.com/ran-j/PS2Recomp) and drawn by a native Direct3D 11 renderer.

**No game files or game code are in this repository or the download.** Everything made from your disc stays on your PC.

## Download and play

1. Get **HCTP-Recompiled-Windows.zip** from the [latest release](https://github.com/BigDaddyJRich/hctp-recomp/releases/latest)
   and unzip it to a short path, e.g. `C:\Games\HCTP Recompiled` (not Desktop / Downloads / OneDrive).
2. Open **HCTP Recompiled Settings.exe** (the launcher) and pick your disc image (.iso or .chd).
3. It builds the game on your PC (30-90 minutes the first time). Then press **PLAY**.

**Only the unmodified USA retail disc (SLUS-20787) works.** Modded, patched or other-region ISOs break the game
(for example a freeze on the Brock Lesnar screen).

You need Windows 10/11 64-bit, a CPU with AVX2, a DirectX 11 graphics card, 16 GB of RAM and 15 GB of free space.

- **Saves:** `Documents\HCTP`.
- **Controllers:** Xbox controllers, PlayStation 4 / 5 controllers (USB or Bluetooth, with vibration), and keyboard.
  Keys and buttons can be changed on the launcher's Controls page. Up to 4 players.
- **Slow or choppy?** Lower **Scale** in the launcher's Settings (Auto picks 6x on a 4K screen; try 3x or 2x).
- **Matrix spin + instant replay:** two features cut from the game (L1 during a finisher, L2), behind a switch in the
  launcher's Settings, off by default.

What's new in each version: [CHANGELOG.md](CHANGELOG.md).

## Bugs and help

Join the **[HCTP Discord server](https://discord.gg/z39SWwQDq)** and post in the bugs channel, or open a
[GitHub issue](https://github.com/BigDaddyJRich/hctp-recomp/issues). Please don't send bug reports by direct message.
Include what you did, your CPU / graphics card / RAM, and the logs `game.log` and `game.log.err` from
`HCTP Recompiled\files\logs` (`files\game\logs` if you start the game from Steam; `setup.log` if the build failed).

Known limits: PPV MODE and ONLINE are on the main menu but don't work yet. Crashes are possible in parts that
haven't been tested.

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

Players: use the release zip above. Developers - toolchain: Visual Studio 2022+/MSVC x64, CMake, Ninja, Python, a Direct3D 11 GPU:
1. Put your own unmodified `SLUS_207.87` and the disc files in a folder.
2. Recompile it with `PS2Recomp` using `config_hctp/hctp.toml` (generates the runner C++ locally).
3. `HCTP_VU1_RANGES="0x44c6e4:0x44eb70,0x4526a4:0x452d08" python -I tools/vu1_recomp.py SLUS_207.87 generated/vu1_compiled.cpp`
4. `build-fast.bat` (quick, for development) or `build-runtime.bat` (fully optimised).
5. Run from the game folder: `ps2EntryRunner.exe SLUS_207.87` (see `run_hctp.bat` for useful environment variables).

## Credits

- [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) — the recompiler and runtime this is built on
- upstream PS2Recomp components — recompiler, runtime, IOP/SIF support, and related tooling
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) — GS on Vulkan (the earlier renderer; reference)
- [PCSX2](https://github.com/PCSX2/pcsx2) — hardware behaviour reference
- BT3-Recomp, Ace Combat 5 static recompilation, NBA Street V3 recomp — techniques and references

## License

GPL-3.0, as the base project (see `LICENSE` and `THIRD-PARTY-NOTICES.txt`). WWE SmackDown! Here Comes the Pain is
(c) its owners; this project contains none of its files or code.

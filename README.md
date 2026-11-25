# WWE SmackDown! Here Comes the Pain — PC recompilation

> ## ⚠️ THIS IS A BETA. IT IS NOT A FINISHED GAME.
> **HCTP Recompiled is a work in progress.** Every download on the Releases page is a **test build**: expect bugs,
> crashes, missing features and things that change from one version to the next. It is not a final release and it is
> not "done". If something breaks, report it (see [Bugs and help](#bugs-and-help)) — that is what these builds are for.

> ## 🛑 USA DISC ONLY — SLUS-20787 — ORIGINAL AND UNMODIFIED
> **The ONLY disc that works is the original, unmodified WWE SmackDown! Here Comes the Pain, USA version (SLUS-20787).**
>
> **These DO NOT work** (from the next update the setup refuses them outright):
> - ❌ **Modded / patched ISOs** — HCTP 2X, 3X, Ultimate Edition, DE, Ruthless Aggression, Patch Projects, any ISO
>   someone edited (rosters, models, music, attires...)
> - ❌ **PAL / Europe** and **Japan** discs
> - ❌ Any other game (Shut Your Mouth, SvR, ...)
>
> From the next update, picking the wrong disc shows a red **WRONG DISC** screen and builds nothing. That is not a bug:
> get a clean dump of the USA disc. (Support for modded discs, starting with Nero's 3X, is planned for later — not yet.)

> ## 🖥️ THIS IS A WINDOWS PC PORT. ONLY.
> **It runs on Windows 10 / 11 PCs (desktops and laptops). That's it.**
> - 🕒 **Steam Deck / Linux:** planned, but **not coming any time soon.** No date. Don't ask for one.
> - ❌ **Never coming:** Android, iPhone / iPad, phones or tablets of any kind, Xbox, PlayStation, Switch or any other
>   console, smart TVs, Mac, Chromebooks, or anything else that isn't a PC.
> - ❌ **Not for a real PS2.** This is not a PS2 mod or a patched ISO; it does not run on PS2 hardware or in an emulator.
>
> It's open source (GPL-3.0): if you want it on something else, you're free to port it yourself.

A static recompilation of **WWE SmackDown! Here Comes the Pain** (PlayStation 2, USA, **SLUS-20787**) into a native
Windows game, built on your PC from your own disc. The game's code is translated to C++ with
[PS2Recomp](https://github.com/ran-j/PS2Recomp) and drawn by a native Direct3D 11 renderer. It is **not an emulator**.

**No game files or game code are in this repository or the download.** You need your own disc. Everything made from
your disc stays on your PC.

---

## Contents

1. [Before you download](#before-you-download)
2. [System requirements](#system-requirements)
3. [Install, step by step](#install-step-by-step)
4. [Playing](#playing)
5. [Troubleshooting](#troubleshooting)
6. [Bugs and help](#bugs-and-help)
7. [What works and what doesn't yet](#what-works-and-what-doesnt-yet)
8. [Platforms](#platforms)
9. [Developers](#developers)

---

## Before you download

Check all of these **before** you download, or the install will fail:

| | Check |
|---|---|
| ✅ | You have **your own WWE SmackDown! Here Comes the Pain USA disc (SLUS-20787)**, dumped to an `.iso` or `.chd` file, **unmodified**. |
| ✅ | **A Windows 10 or 11 PC, 64-bit.** (No Mac, phone, console or TV. Steam Deck / Linux: planned, not soon.) |
| ✅ | Your CPU has **AVX2** (Intel 4th gen "Haswell" Core i3/i5/i7 or newer, any AMD Ryzen). Older Pentium / Celeron / Core 2 / Phenom CPUs **cannot** run it. |
| ✅ | About **15 GB free** for the install and the build. |
| ✅ | You will unzip it to a **short, normal folder** like `C:\Games\HCTP Recompiled` — **not** Desktop, Downloads, OneDrive, or a folder with a very long path. |
| ✅ | Internet during the first install (it downloads Microsoft's free C++ build tools if they're missing). |
| ✅ | **Laptops: plug the charger in.** |

## System requirements

| | Minimum | Recommended |
|---|---|---|
| **OS** | Windows 10 64-bit | Windows 11 64-bit |
| **CPU** | 2 cores / 4 threads **with AVX2**: Intel Core i3-4130 / AMD Ryzen 3 1200 | 6+ cores: Intel Core i5-8400 / AMD Ryzen 5 2600 or newer |
| **Memory** | 8 GB RAM | 16 GB RAM |
| **Graphics** | DirectX 11: NVIDIA GTX 750 Ti 2 GB / AMD RX 460 / Intel UHD 770 | NVIDIA GTX 1070 / RTX 2060 / AMD RX 5600 XT or better, 6 GB |
| **Storage** | 15 GB free | 15 GB free on an SSD |
| **What you get** | 60 fps in 1-on-1 matches at 1080p with Scale on Auto or 1x–2x (entrances and six-man matches can dip) | 60 fps at 1080p–1440p at 4x, entrances and six-man included |

From the next update, the launcher's **System requirements** page shows these next to **your** PC (green = meets Recommended, yellow =
meets Minimum, red = below). On a laptop the game uses your NVIDIA / AMD graphics card, not the built-in Intel one.

## Install, step by step

1. Download **HCTP-Recompiled-Windows.zip** from the
   [latest release](https://github.com/BigDaddyJRich/hctp-recomp/releases/latest).
2. **Unzip it** (right-click → Extract All) to a short path, e.g. `C:\Games\HCTP Recompiled`.
   Don't run it from inside the zip.
3. Open **HCTP Recompiled Settings.exe** (the launcher).
4. **Choose your disc:** drop your `.iso` / `.chd` on the window or press **Browse…**.
   - Green "✓ SmackDown! Here Comes the Pain found" = good, press **Next: build the game**.
   - Red **WRONG DISC** = that disc will not work (see the box at the top of this page). Nothing gets built.
5. **Wait for the build.** It turns the game's code into a Windows program on your PC.
   - About **25–40 minutes** on a fast PC (8+ cores, 16–32 GB), **1–2 hours** on a 4-core PC or with 8 GB of memory.
   - The bar can sit still for minutes at a time. **It is still working. Don't close it.**
   - You can keep using your PC while it builds.
6. Pick your settings, then press **PLAY**. Desktop shortcuts are made for you:
   **HCTP Recompiled** (starts the game) and **HCTP Recompiled Settings** (the launcher).

**Updates:** the launcher tells you when a new version is out and updates itself (it rebuilds the game, usually
quicker than the first time). Your saves, settings and mods are kept.

## Playing

- **Saves:** `Documents\HCTP` (back this folder up).
- **Controllers:** Xbox controllers, PlayStation 4 / 5 controllers (USB or Bluetooth, with vibration), and keyboard.
  Keys and buttons can be changed on the launcher's **Controls** page. Up to 4 players.
  Using DS4Windows or DualSenseX? Turn off **Read PlayStation controllers directly** on the Controls page (Steam
  Input is detected by itself).
- **Graphics:** the launcher's **Graphics** page. **Scale** = how sharp the game is drawn (Auto picks for your screen;
  lower it if it's slow).
- **Frame rate counter:** launcher → Gameplay → **Show frame rate**. `FPS` = the window, `GAME` = the game itself.
- **Matrix spin + instant replay:** two features cut from the original game (L1 during a finisher, L2), behind a
  switch in the launcher's Settings, off by default.

What's new in each version: [CHANGELOG.md](CHANGELOG.md).

## Troubleshooting

**First stop (from the next update): the launcher's "Having trouble?" page.** It has the Discord buttons, a **SAVE LOGS** button (puts one zip
with your logs and PC info on your Desktop) and the fixes below.

| Problem | Fix |
|---|---|
| **Red "WRONG DISC" screen** | Your ISO is modded, patched, PAL, Japan, or another game. Only the clean USA disc (SLUS-20787) works. |
| **The build failed** | Check the folder is short and not in OneDrive / Desktop / Downloads, you have 15 GB free, and your antivirus isn't blocking the folder. Then post your logs (launcher → Having trouble? → SAVE LOGS). |
| **Lag / slow / choppy** | Laptop: **plug in the charger** and turn on its **Performance mode** (Lenovo Fn+Q / Vantage, ASUS Armoury Crate, MSI Center, HP OMEN Gaming Hub, Dell/Alienware Command Center). Lower **Scale** (Graphics page) one step, Anti-aliasing off. Close other heavy programs. |
| **Stuck on the "Now Loading" (Brock Lesnar) screen** — FPS still counting, GAME at 0 | Known bug we're fixing (it hits some boots, more often on 4-thread CPUs). Close the game and start it again. Send your logs so we can confirm the fix. |
| **Controller does nothing / wrong buttons** | Launcher → Controls. If you use DS4Windows / DualSenseX, turn off "Read PlayStation controllers directly". |
| **Game won't start / closes right away** | Update your graphics driver (NVIDIA app / AMD Adrenalin / Intel). Make sure the build finished. Then SAVE LOGS and post them. |
| **"Memory card corrupted" / save problems** | Your saves are in `Documents\HCTP`. Don't edit them; post your logs. |

## Bugs and help

1. Open the launcher → **Having trouble?** → press **SAVE LOGS**. A zip appears on your Desktop.
2. Join the **[HCTP Discord server](https://discord.gg/z39SWwQDq)** and post in the
   **[bugs channel](https://discord.gg/TbtXkwbHz)**:
   - what happened and what you were doing (which mode, which wrestlers, which screen),
   - **the zip from step 1** (drag it into Discord).

   Or open a [GitHub issue](https://github.com/BigDaddyJRich/hctp-recomp/issues) with the same.
3. **No logs = we can't fix it.** Please don't send bug reports by direct message.

Older version without the SAVE LOGS button? The logs are `game.log` and `game.log.err` in
`HCTP Recompiled\files\logs` (`setup.log` there if the build failed). Launcher → File → Open game folder → `files` →
`logs`.

## What works and what doesn't yet

This is a **beta**. Roughly:

- **Works:** exhibition matches (singles, tag, six-man and more), Create-a-Wrestler, Create-a-Moveset, Season,
  saves, controllers with vibration.
- **Not yet:** PPV MODE and ONLINE are on the main menu but **don't work yet**. Modded discs (Nero's 3X first) are
  planned, not done.
- **Expect:** crashes in parts nobody has tested yet, visual glitches, and the occasional freeze. Report them.

## Platforms

| Platform | Status |
|---|---|
| **Windows 10 / 11 PC (64-bit)** | ✅ Supported (beta) |
| **Steam Deck / Linux** | 🕒 Planned — **not any time soon**, no date |
| Android, iPhone / iPad, phones, tablets | ❌ Never |
| Xbox, PlayStation, Switch, any console | ❌ Never |
| Smart TVs, Mac, Chromebook | ❌ Never |
| A real PS2 / PS2 emulators | ❌ Never — this is a PC port, not a PS2 mod |

This is a **PC port**, made for PCs. It is open source (GPL-3.0): anyone who wants it somewhere else is free to port
it themselves.

---

## Developers

### Layout

| Path | What |
|---|---|
| `PS2Recomp/` | submodule: the recompiler + runtime, branch `hctp` (EE scheduler, VU1, GS front-end, IOP, SIF, libsdr ...) |
| `overrides_hctp/` | HCTP-specific runtime overrides (heap, CD search, movie hooks, native libsdr hookup, profiler) |
| `config_hctp/` | recompiler configuration for SLUS_207.87 (function names / stub lists only) |
| `settings-app/` | the launcher (Avalonia, .NET) |
| `tools/` | setup / release scripts, `vu1_recomp.py` (VU1 microcode -> C++), `prepare_disc.py` + `disc_slus20787.json` (stock-disc check: file hashes only), test helpers |
| `run_hctp.bat` | normal HCTP developer runner |
| `developer/runs/` | optional cycle-exact, field-inversion, native-audio, and slow-VU1 diagnostic runners |

### Building

Players: use the release zip above. Developers — toolchain: Visual Studio 2022+/MSVC x64, CMake, Ninja, Python, a
Direct3D 11 GPU:
1. Put your own unmodified `SLUS_207.87` and the disc files in a folder (`python tools/prepare_disc.py <disc.iso>`
   checks and extracts them).
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

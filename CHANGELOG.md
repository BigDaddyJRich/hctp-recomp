# HCTP recompilation release notes

This file records HCTP-specific changes only. Local experiments, extracted game assets, emulator dumps, and temporary
run scripts are intentionally kept out of the public repository.

## 0.3.0

First public build of HCTP Recompiled: bring your own USA disc (SLUS-20787); the game is built on your PC.

- New launcher: HCTP Recompiled Settings.exe sets up the build from your disc, then PLAY, settings and updates.
- D3D11 renderer with 1-6x sharper picture, native title movies, 60 fps matches and entrances.
- Fixed: wrestler/referee shaking, nameplate holes, rope shadows, pyro slowdowns (HBK, Goldberg, Triple H),
  CAW forehead band, THQ logo hang.
- New: PPV mode (create a card of up to 5 matches and play it back to back), restored menu options.
- Optional content and commentary packs are shared separately (Discord).

Known issues: HBK ice-cave entrance slowdown, Demo -> Start black screen, see-through hair on some models.

## Current test build

- Native Windows launcher and runner packaging prepared for community testing.
- Required commentary setup is documented separately; players supply their own legally obtained game sources.
- HCTP, Shut Your Mouth, SmackDown vs. Raw, and the public beta are the core commentary sources. Other supported
  games are optional supplements.
- No game ISO, executable, commentary source audio, or modded game files are distributed.

## Runtime findings

- Boot, menus, Create-a-Moveset, Season, entrances, and exhibition matches are operational in the native test path.
- Memory-card behavior is being presented as PC user-data storage rather than a PCSX2 memory-card image.
- ParaLLEl-GS remains the working graphics backend while native-renderer work continues separately.
- Current known issues are dots in nameplates, wrestler shaking/jitter, and frame-rate drops during entrances.

# HCTP recompilation release notes

This file records HCTP-specific changes only. Local experiments, extracted game assets, emulator dumps, and temporary
run scripts are intentionally kept out of the public repository.

## 0.3.0

First public build of HCTP Recompiled: bring your own USA disc (SLUS-20787); the game is built on your PC.

- New launcher: HCTP Recompiled Settings.exe sets up the build from your disc, then PLAY, settings and updates.
- D3D11 renderer with 1-6x sharper picture, native title movies, 60 fps matches and entrances.
- Fixed: wrestler/referee shaking, nameplate holes, rope shadows, pyro slowdowns (HBK, Goldberg, Triple H),
  CAW forehead band, THQ logo hang.
- PPV MODE and ONLINE are on the main menu as early proofs of concept only - neither works as a real mode yet.
  PPV mode is further along (a test card of matches can be made and played) but is unfinished and may crash;
  ONLINE does nothing yet.
- Optional content and commentary packs are shared separately (Discord).

Roster and content: the final roster and content will be made together with Nero's 3X mod. Roster expansion (new
wrestler slots beyond the stock roster) is a work in progress and is planned for the following weeks.

Known issues (most earlier bugs are fixed; these remain):
- HBK's entrance slows down badly in the ice-cave part of the stage; short frame dips during his in-ring pyro.
- HBK's head particles can float or draw as lines.
- Letting the attract demo play and then pressing Start gives a black screen (restart the game).
- Some hair looks see-through (HBK's matches PCSX2, so partly how the model was made).
- OPTIONS and CREDITS menu plates don't fade out when picked; they vanish on the next screen.
- Create-a-Wrestler base model screen: a teal band across both models' foreheads.
- Create-a-Moveset entrance preview: some modded entrances show without their attire (fine in matches).

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

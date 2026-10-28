# HCTP recompilation release notes

This file records HCTP-specific changes only. Local experiments, extracted game assets, emulator dumps, and temporary
run scripts are intentionally kept out of the public repository.

## 0.3.3

- Launcher Play page shows whether the optional Content Pack and Commentary Pack are installed, with a link to get them.
- Al Snow only appears on the roster when the Content Pack (his model) is installed.
- PPV MODE and ONLINE are not working yet: ONLINE is only a proof of concept for now, and PPV MODE is in active
  development and does not function correctly yet.
- Content Pack update (Discord): Jimmy Snuka Titantron, DX's own Titantron, Legend Undertaker's Death Valley intro,
  Al Snow's model.

## 0.3.2

- Launcher settings now reach the game: window / borderless / fullscreen (screen, resolution, refresh rate),
  start maximised, VSync and stretch all work with the Direct3D 11 renderer.
- Unlock everything (Gameplay page): every legend, attire, arena and movie is open; your save is not changed.
- Show frame rate switch (Gameplay page); F1 still toggles it in the game.
- Scale defaults to Auto (picked from your screen), which fixes frame-time hitches in some entrances.
- Fixed stray coloured dots along the right / bottom edge of the picture.

## 0.3.1

- Widescreen 16:9 works: set Picture shape to Widescreen in the launcher (based on ElHecht's PCSX2 widescreen patch).
  Matches show more of the arena; menus, HUD and movies are stretched to 16:9, as with the PCSX2 patch.
- Launcher Scale now sets the renderer's sharpness (1x-6x).

## 0.3.0

First public build of HCTP Recompiled: bring your own USA disc (SLUS-20787); the game is built on your PC.

- New launcher: HCTP Recompiled Settings.exe sets up the build from your disc, then PLAY, settings and updates.
- D3D11 renderer with 1-6x sharper picture, native title movies, 60 fps matches and entrances.
- Fixed: wrestler/referee shaking, nameplate holes, rope shadows, pyro slowdowns (HBK, Goldberg, Triple H),
  CAW forehead band, THQ logo hang.
- Working: Exhibition and Season. Season has NOT been tested yet with the restored cut wrestlers or NPCs - if you
  try it, tell us what happens.
- PPV MODE and ONLINE are on the main menu as early proofs of concept only - neither works as a real mode yet.
  PPV mode is further along (a test card of matches can be made and played) but is unfinished and may crash;
  ONLINE does nothing yet.
- Optional content and commentary packs are shared separately (Discord).

Roster and content: the final roster and content will be made together with Nero's 3X mod. Roster expansion (new
wrestler slots beyond the stock roster) is a work in progress and is planned for the following weeks.

Coming next: a fuller launcher with a built-in mod manager, along the lines of the SvR 2011 recomp's launcher.

Testing: please write down exactly what you did and what happened for anything you test (mode, wrestlers,
match type, arena, what you pressed, what went wrong, a screenshot if you can). Send notes to any of:
- Email: jrich102948@gmail.com
- Discord: https://discord.com/users/531701434597638165
- GitHub Issues: https://github.com/BigDaddyJRich/hctp-recomp/issues

Known issues (most earlier bugs are fixed):
- HBK's entrance slows down when he does his pyro in the ring.
- Commentary (optional pack) is mostly tested but not completely: wrong, missing or mismatched calls (names, match
  types, titles, tag/multi-man matches) - please send what you hear and in which match.
- There can be random crashes and random hangs in things we haven't tested yet. If you hit one, write down what
  you were doing and send it in (see Testing above).

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

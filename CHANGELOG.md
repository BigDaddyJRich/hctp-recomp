# HCTP recompilation release notes

This file records HCTP-specific changes only. Local experiments, extracted game assets, emulator dumps, and temporary
run scripts are intentionally kept out of the public repository.

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

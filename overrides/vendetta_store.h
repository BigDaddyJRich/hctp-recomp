// Def Jam Vendetta (USA, SLUS-20639) — the store: guest memory above the PS2's 32 MB for data from mods.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game's own code can only read guest memory, and the PS2's 32 MB is full of the game. The runtime backs guest
// RAM with PS2_RAM_BACKING_SIZE (256 MB) but commits only the first 32 MB; the store hands out pieces of the rest
// (from 0x02000000 up), committing memory as it goes, so a mod's animations, timing records - later models, arenas,
// sounds - can sit where the game's code reads them like any other RAM. Only data the game's own code reads belongs
// here; data our own C++ reads stays in ordinary PC memory.
//
// Areas: today everything is placed once, in the boot area, and stays. The layout keeps room for a per-match area:
// sealed after the boot area, emptied and refilled at each match start with only what that match uses, so the number
// of mods installed is no longer limited by guest memory. A user names the area it allocates in; adding the match area
// changes the store, not its users.
//
// Each run prints what the store holds ([store] ...), by user.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vendetta_store
{
    enum class Area
    {
        Boot, // placed once, kept for the whole run
    };

    // Called once at start (game_overrides): the runtime's memory.
    void install(PS2Runtime &runtime);

    // A piece of guest memory of `size` bytes in `area`, starting `phase` bytes past a multiple of `alignment` (a power
    // of two; the game's clips sit 8 past a 16-byte boundary: alignment 16, phase 8). `user` names what it holds, for
    // the report. Returns the guest address (>= 0x02000000), or 0 with a message when the store is full.
    uint32_t allocate(Area area, uint32_t size, const char *user, uint32_t alignment = 16, uint32_t phase = 0);

    // Host pointer for a guest address the store handed out.
    uint8_t *host(uint32_t guestAddress);

    // Prints what the store holds, by user (once the mods are in).
    void report();
}

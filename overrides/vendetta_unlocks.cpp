// Def Jam Vendetta (USA, SLUS-20639) - unlock everything (VENDETTA_UNLOCKALL=1; settings.toml gameplay.unlock_all).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A save profile (0x20C bytes) holds the unlocks at +0xFC: 60 wrestler / attire flags, 12 stage flags at +0x138, 120
// picture flags at +0x144 (5 girls x 24), the rest at +0x1BC. No profile signed in = a zeroed one. The switch changes
// what the game's readers of those flags see, never the flags: the save on the card stays the player's own, the game
// still sets a flag when it is earned (story, the gallery's points, a cheat code), and with the switch off everything
// is as the save says. The readers:
//
// - FUN_00157038(counts, profile) (profile 0 = the current one at 0x243100) turns a profile into counts[63] (per
//   character record, how many of its attires can be picked: the flags its row of the attire table 0x27D610 lists that
//   are set, up to 3, a -1 ends the row) and counts + 0xFC (the 12 stages in the order of the list 0x27D908, 1 =
//   unlocked), then the career panel's totals from +0x12C. Every select screen (wrestlers and attires for each
//   player, the stage select, the CPU's picks) works from those counts. The original runs as it is (the career totals
//   stay true), then the counts are rewritten as if every listed flag were set: what a completed save shows (the IN
//   PROGRESS Ludacris record 41 and flag 54 are listed nowhere, so they stay hidden, as on a completed save).
// - The gallery (the Extras menu FUN_0018adc8, its state at 0x2A07C0, the signed-in profile at +0x50): the page list
//   FUN_0018c940 asks FUN_0018c900(block, page) "is any of this girl's 24 pictures unlocked" (answered yes); the
//   pictures screen FUN_0018d848 reads profile + 0x144 + page * 0x18 + row * 6 + column directly, once a frame
//   (unlocked = view it, locked = buy it with the points at +0x30). For that call the 120 picture flags in guest
//   memory read 1; the real ones are put back when it returns (and before a cheat code FUN_0018f410 writes into them,
//   so a code entered there is still kept). Nothing else runs between: no save happens inside a screen's frame.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace vendetta_unlocks
{
    constexpr uint32_t kCounts = 0x157038u;         // FUN_00157038(counts, profile)
    constexpr uint32_t kAttireTable = 0x27D610u;    // 63 rows x 3 s32 unlock-flag indices, -1 = none
    constexpr uint32_t kRecords = 63u;
    constexpr uint32_t kStageCounts = 0xFCu;        // counts + 0xFC: 12 s32, 1 = the stage is unlocked
    constexpr uint32_t kStages = 12u;
    constexpr uint32_t kPageAny = 0x18C900u;        // FUN_0018c900(block, page): any of the page's pictures unlocked
    constexpr uint32_t kPictures = 0x18D848u;       // FUN_0018d848(state): the pictures screen, one frame
    constexpr uint32_t kCheatApply = 0x18F410u;     // FUN_0018f410(code, block): a cheat code's flags set
    constexpr uint32_t kExtrasProfile = 0x50u;      // state + 0x50: the signed-in profile
    constexpr uint32_t kBlock = 0xFCu;              // profile + 0xFC: the unlock block
    constexpr uint32_t kPictureFlags = 0x144u;      // profile + 0x144: 120 picture flags
    constexpr uint32_t kPictureCount = 120u;

    PS2Runtime::RecompiledFunction g_originalCounts = nullptr;
    PS2Runtime::RecompiledFunction g_originalPictures = nullptr;
    PS2Runtime::RecompiledFunction g_originalCheatApply = nullptr;

    // while the pictures screen runs: the profile whose picture flags read 1, and their real values
    uint32_t g_shownProfile = 0;
    std::array<uint8_t, kPictureCount> g_realPictures{};

    void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

    void counts(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t out = getRegU32(ctx, 4);
        g_originalCounts(rdram, ctx, runtime);
        for (uint32_t record = 0; record < kRecords; ++record)
        {
            uint32_t attires = 0;
            for (uint32_t column = 0; column < 3u; ++column)
            {
                if (static_cast<int32_t>(Ps2FastRead32(rdram, kAttireTable + record * 12u + column * 4u)) == -1)
                    break;
                ++attires;
            }
            Ps2FastWrite32(rdram, out + record * 4u, attires);
        }
        for (uint32_t stage = 0; stage < kStages; ++stage)
            Ps2FastWrite32(rdram, out + kStageCounts + stage * 4u, 1u);
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            std::cerr << "[unlocks] the select screens: every wrestler, attire and stage" << std::endl;
        }
    }

    void pageAny(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, 1u);
        returnToCaller(ctx);
    }

    void showPictures(uint8_t *rdram, uint32_t profile)
    {
        for (uint32_t i = 0; i < kPictureCount; ++i)
        {
            g_realPictures[i] = Ps2FastRead8(rdram, profile + kPictureFlags + i);
            Ps2FastWrite8(rdram, profile + kPictureFlags + i, 1u);
        }
    }

    void restorePictures(uint8_t *rdram, uint32_t profile)
    {
        for (uint32_t i = 0; i < kPictureCount; ++i)
        {
            // a flag the screen changed from the 1 it was shown is a real change (none is expected: buying needs a
            // locked picture)
            const uint8_t now = Ps2FastRead8(rdram, profile + kPictureFlags + i);
            Ps2FastWrite8(rdram, profile + kPictureFlags + i, now != 1u ? now : g_realPictures[i]);
        }
    }

    void pictures(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t profile = Ps2FastRead32(rdram, getRegU32(ctx, 4) + kExtrasProfile);
        if (profile == 0u || g_shownProfile != 0u)
        {
            g_originalPictures(rdram, ctx, runtime);
            return;
        }
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            std::cerr << "[unlocks] the gallery: every picture (profile 0x" << std::hex << profile << std::dec << ")"
                      << std::endl;
        }
        showPictures(rdram, profile);
        g_shownProfile = profile;
        g_originalPictures(rdram, ctx, runtime);
        g_shownProfile = 0u;
        restorePictures(rdram, profile);
    }

    void cheatApply(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t profile = g_shownProfile;
        if (profile == 0u || getRegU32(ctx, 5) != profile + kBlock)
        {
            g_originalCheatApply(rdram, ctx, runtime);
            return;
        }
        // the code's flags go into the real ones
        restorePictures(rdram, profile);
        g_originalCheatApply(rdram, ctx, runtime);
        showPictures(rdram, profile);
        std::cerr << "[unlocks] a cheat code in the gallery: kept in the save" << std::endl;
    }

    bool enabled()
    {
        const char *value = std::getenv("VENDETTA_UNLOCKALL");
        return value && *value && value[0] != '0';
    }

    void install(PS2Runtime &runtime)
    {
        if (!enabled())
            return;
        g_originalCounts = runtime.lookupFunction(kCounts);
        g_originalPictures = runtime.lookupFunction(kPictures);
        g_originalCheatApply = runtime.lookupFunction(kCheatApply);
        if (!g_originalCounts || !g_originalPictures || !g_originalCheatApply)
        {
            std::cerr << "[unlocks] the game's unlock routines were not found: off" << std::endl;
            return;
        }
        runtime.replaceFunction(kCounts, counts);
        runtime.replaceFunction(kPageAny, pageAny);
        runtime.replaceFunction(kPictures, pictures);
        runtime.replaceFunction(kCheatApply, cheatApply);
        std::cerr << "[unlocks] on: every wrestler, attire, stage and picture (the save is not changed)" << std::endl;
    }
}

void installVendettaUnlocks(PS2Runtime &runtime) { vendetta_unlocks::install(runtime); }

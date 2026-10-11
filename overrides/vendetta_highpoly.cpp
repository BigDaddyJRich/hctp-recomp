// Def Jam Vendetta (USA, SLUS-20639) - High Poly in every match (VENDETTA_HIGHPOLY=1; settings.toml graphics.high_poly).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game keeps two of everything for its 3- and 4-fighter matches, made to fit the PS2: each wrestler's model file
// holds a High Poly body (BODY + FACE + hands, the see-through ALPHA parts) and a Low Poly one (BODY2: no face poses,
// no ALPHA), and each arena 0-11 has a Low Poly copy at venue + 12 (no 3-D ringside crowd; Da Bridge, Club Luda and
// the Face Club lose scenery). With the switch on, every match keeps the High Poly ones:
//
// - Wrestlers: bit 0 of 0x1E13CC4 + n * 0x34C picks the body (1 = High Poly), set by FUN_001b3568(n, on). Its only
//   callers: the attach FUN_001b14e0 (1 when BODY, FACE and L HAND exist - every game model), and four that force it
//   in 3- / 4-fighter matches - the DEMO runner's end FUN_001256d0 (0), the victory DEMO start FUN_00125ac8 (1),
//   camera mode 4's enter FUN_00104ff0 (1) and exit FUN_00104438 (0). Only the attach's call is kept, so a model
//   stays on what it was attached with.
// - Battle's arena: FUN_00140298 takes venue = stage for up to 2 fighters, else FUN_001a2ac8(stage) = stage + 12
//   (its only caller); that lookup returns the stage unchanged.
// - Story's arenas: FUN_00170360 sets the next fight's venue at +0x3CC of the story struct (a0) - from table A
//   0x282880 (main arenas, and Def Jam's four banner versions 24-27, which are High Poly), table B 0x285500 (the
//   2-on-1s at Da Bridge, venue 17) and the 3-way's constant 20 (Face Club After Hours). A venue 12-23 there is
//   turned back into its main arena (17 -> 5, 20 -> 8); 24-27 stay.
//
// A venue reaches only the chunk lookup, the per-venue routine switches, venue -> stage and venue -> sound set, so
// the main arena's number is all that changes. Unset or 0: the PS2's behaviour.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>

namespace vendetta_highpoly
{
    constexpr uint32_t kSetQuality = 0x1B3568u;      // FUN_001b3568(n, on): the body bit
    constexpr uint32_t kAttachReturn = 0x1B15B8u;    // the attach FUN_001b14e0's call at 0x1B15B0
    constexpr uint32_t kLowPolyVenue = 0x1A2AC8u;    // FUN_001a2ac8(stage) -> stage + 12
    constexpr uint32_t kStoryFight = 0x170360u;      // FUN_00170360(story): the next fight
    constexpr uint32_t kStoryVenueOffset = 0x3CCu;

    PS2Runtime::RecompiledFunction g_originalStoryFight = nullptr;

    void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

    void setQuality(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        if (ra == kAttachReturn)
        {
            const uint32_t address = 0x1E13CC4u + getRegU32(ctx, 4) * 0x34Cu;
            uint32_t flags = Ps2FastRead32(rdram, address);
            flags = getRegU32(ctx, 5) != 0u ? (flags | 1u) : (flags & ~1u);
            Ps2FastWrite32(rdram, address, flags);
        }
        else
        {
            static std::mutex mutex;
            static std::set<uint32_t> seen;
            std::lock_guard<std::mutex> lock(mutex);
            if (seen.insert(ra).second)
                std::cerr << "[highpoly] fighter " << getRegU32(ctx, 4) << " kept on its body (the call from 0x"
                          << std::hex << ra - 8u << std::dec << " wanted " << (getRegU32(ctx, 5) != 0u ? "High" : "Low")
                          << " Poly)" << std::endl;
        }
        returnToCaller(ctx);
    }

    void lowPolyVenue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stage = getRegU32(ctx, 4);
        std::cerr << "[highpoly] 3-4 fighters at stage " << stage << ": the High Poly arena (not venue " << stage + 12u
                  << ")" << std::endl;
        setReturnU32(ctx, stage);
        returnToCaller(ctx);
    }

    void storyFight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t story = getRegU32(ctx, 4);
        g_originalStoryFight(rdram, ctx, runtime);
        const uint32_t venue = Ps2FastRead32(rdram, story + kStoryVenueOffset);
        if (venue >= 12u && venue <= 23u)
        {
            Ps2FastWrite32(rdram, story + kStoryVenueOffset, venue - 12u);
            std::cerr << "[highpoly] story fight: venue " << venue << " -> " << venue - 12u << " (the High Poly arena)"
                      << std::endl;
        }
    }

    bool enabled()
    {
        const char *value = std::getenv("VENDETTA_HIGHPOLY");
        return value && *value && value[0] != '0';
    }

    void install(PS2Runtime &runtime)
    {
        if (!enabled())
            return;
        runtime.replaceFunction(kSetQuality, setQuality);
        runtime.replaceFunction(kLowPolyVenue, lowPolyVenue);
        g_originalStoryFight = runtime.lookupFunction(kStoryFight);
        if (g_originalStoryFight)
            runtime.replaceFunction(kStoryFight, storyFight);
        std::cerr << "[highpoly] on: High Poly wrestlers and arenas in every match" << std::endl;
    }
}

void installVendettaHighPoly(PS2Runtime &runtime) { vendetta_highpoly::install(runtime); }

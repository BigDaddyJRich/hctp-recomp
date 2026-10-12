// Def Jam Vendetta (USA, SLUS-20639) - widescreen in matches (VENDETTA_WIDESCREEN, runtime/ps2_widescreen.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game sets its 3-D view in one place: FUN_001a1768(fov) calls FUN_001b6d08(1792, 1936, 512, 224, fov in radians,
// 0.75, out = 0x258040) - screen origin, size, field of view and the aspect 0.75 (f12-f17, a0); the matrix's [0][0] is
// cot(fov / 2) x aspect x 256. Only the match camera (FUN_001037a0, every match frame; FUN_00104278) and the boot
// (FUN_001a0780) call it. In widescreen the aspect is scaled by (4/3) / target, so the view widens to the sides (the
// fighters keep their size) and the frame is drawn squeezed into the game's 512-wide buffer; each call from the match
// camera marks the frame wide, which the window shows at the target shape (menus and movies, which never set this
// projection, stay 4:3). The boot's call isn't a match: marking it showed the first LOADING pictures stretched wide,
// then jumping back to 4:3.
//
// The HUD: drawn last in a match frame, with drawing context 1 (PRIM.CTXT = 0), no depth test (TEST.ZTE = 0), into the
// display buffer (FBP 0x0 / 0x40); the 3-D scene uses context 2 with the depth test, the shadows context 2 into FBP
// 0x160. That rule goes to the GS front end, which squeezes each HUD element back to its shape at the screen's left
// edge, right edge or centre (GS::setWidescreenOverlayRule).
//
// The 3-D crowd: its draw FUN_00197428 (one pose routine per arena) asks FUN_00197b88(p) for each person whether the
// point p (view space: x at +0, z at +8, in front when z <= 0) is on screen: the half field of view a = fov (0x258130,
// degrees) x 0.5 x two constants at gp - 0x7328 / gp - 0x7324 / 180 (radians), sin / cos by FUN_001f7dd0 / FUN_001f7ce8,
// and the point kept (v0 = 0) when x cos a - z sin a >= -10 and -x cos a - z sin a >= -10, else v0 = 1. Those are the
// 4:3 picture's edges: in widescreen the people standing in the widened sides were skipped and popped in and out as
// the camera moved. Here the test is answered with the edges as wide as the picture: tan a x the same factor the
// projection's aspect was divided by (target / (4/3)). VENDETTA_WIDECROWD=0: the game's own 4:3 test. Checked once
// against the game's own test with the factor 1: 16,384 calls, 0 different answers (the check called the original
// routine from here and then carried on, which isn't safe while the original can be resumed later: not kept).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_widescreen.h"
#include "runtime/gs/gs_frontend.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace vendetta_widescreen
{
    constexpr uint32_t kBuildProjection = 0x1B6D08u; // FUN_001b6d08: the aspect in f17
    constexpr uint32_t kSetProjection = 0x1A1768u;   // FUN_001a1768(fov): its only caller of FUN_001b6d08
    constexpr uint32_t kBootStart = 0x1A0780u;       // FUN_001a0780, the boot: 0x1A0780-0x1A13DF
    constexpr uint32_t kBootEnd = 0x1A13E0u;
    PS2Runtime::RecompiledFunction g_originalBuildProjection = nullptr;
    PS2Runtime::RecompiledFunction g_originalSetProjection = nullptr;
    bool g_fromBoot = false; // the game thread only

    void setProjection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        g_fromBoot = ra >= kBootStart && ra < kBootEnd;
        g_originalSetProjection(rdram, ctx, runtime);
        g_fromBoot = false;
    }

    void buildProjection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float target = ps2_widescreen::targetAspect();
        if (target > 0.0f)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                std::cerr << "[widescreen] the match's view: aspect " << ctx->f[17] << " -> "
                          << ctx->f[17] * (4.0f / 3.0f) / target << " (shown at " << target << ":1)" << std::endl;
            }
            ctx->f[17] = ctx->f[17] * (4.0f / 3.0f) / target;
            if (!g_fromBoot)
                ps2_widescreen::markWideFrame();
        }
        g_originalBuildProjection(rdram, ctx, runtime);
    }

    constexpr uint32_t kCrowdOnScreen = 0x197B88u; // FUN_00197b88(p): 0 = draw this crowd member, 1 = skip
    constexpr uint32_t kFov = 0x258130u;           // the match camera's field of view (degrees)
    float g_widen = 1.0f;                          // the picture's horizontal widening: target / (4/3)

    float readFloat(uint8_t *rdram, uint32_t address)
    {
        const uint32_t bits = Ps2FastRead32(rdram, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    uint32_t crowdSkip(uint8_t *rdram, uint32_t gp, uint32_t point, float widen)
    {
        const float x = readFloat(rdram, point);
        const float z = readFloat(rdram, point + 8u);
        if (!(z <= 0.0f))
            return 1u;
        float half = readFloat(rdram, kFov) * 0.5f;
        half = half * readFloat(rdram, gp - 0x7328u) * readFloat(rdram, gp - 0x7324u) / 180.0f;
        if (widen != 1.0f)
            half = std::atan(std::tan(half) * widen);
        const float s = std::sin(half);
        const float c = std::cos(half);
        return x * c - z * s >= -10.0f && -x * c - z * s >= -10.0f ? 0u : 1u;
    }

    void crowdOnScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t point = getRegU32(ctx, 4);
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t ra = getRegU32(ctx, 31);
        setReturnU32(ctx, crowdSkip(rdram, gp, point, g_widen));
        ctx->pc = ra;
    }

    bool isHud(const GSDrawState &state)
    {
        return state.prim.ctxt == 0u && (state.context.test & 0x10000u) == 0u && state.context.frame.fbp < 0x80u;
    }

    void install(PS2Runtime &runtime)
    {
        if (ps2_widescreen::targetAspect() <= 0.0f)
            return;
        g_originalBuildProjection = runtime.lookupFunction(kBuildProjection);
        if (g_originalBuildProjection)
            runtime.replaceFunction(kBuildProjection, buildProjection);
        g_originalSetProjection = runtime.lookupFunction(kSetProjection);
        if (g_originalSetProjection)
            runtime.replaceFunction(kSetProjection, setProjection);
        if (!std::getenv("VENDETTA_WIDEHUD") || std::getenv("VENDETTA_WIDEHUD")[0] != '0')
            runtime.gs().setWidescreenOverlayRule(isHud); // VENDETTA_WIDEHUD=0: the HUD stretched
        const char *crowd = std::getenv("VENDETTA_WIDECROWD");
        if (!crowd || crowd[0] != '0')
        {
            g_widen = ps2_widescreen::targetAspect() / (4.0f / 3.0f);
            runtime.replaceFunction(kCrowdOnScreen, crowdOnScreen);
            std::cerr << "[widescreen] the crowd drawn across the whole wide picture (its on-screen test widened x"
                      << g_widen << ")" << std::endl;
        }
    }
}

void installVendettaWidescreen(PS2Runtime &runtime) { vendetta_widescreen::install(runtime); }

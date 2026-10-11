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
#include "ps2_runtime.h"
#include "runtime/ps2_widescreen.h"
#include "runtime/gs/gs_frontend.h"

#include <cstdlib>
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
    }
}

void installVendettaWidescreen(PS2Runtime &runtime) { vendetta_widescreen::install(runtime); }

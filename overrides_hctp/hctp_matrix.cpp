// HCTP_MATRIX=1 (Nov 11 2026): native port of Shut Your Mouth's "matrix" effect (L1 during a finisher) + the L2 replay
// button, both cut from HCTP. Notes: F:\hctp-project\notes\MATRIX_SPIN_RESTORE_20261107.md section 9 (address map,
// constants, risks). Source = SYM slus_204.83 (gp 0x3d5870), decoded instruction by instruction:
//   trigger  SYM COperate::TransactionCase tail 0x228aa8..0x228bf4  -> mxCheckButtons (after HCTP TransactionCase 0x1e70c0)
//   effect   SYM CGame::Action matrix block 0x133268..0x133754      -> mxTopDraw (after GameMainSequence::TopDraw 0x2e0e70)
// SYM froze the match with an inner loop (120 x {orbit world_view, pad Read, CObjControl Draw/DrawSecond/DrawShadow/
// DrawFin}) and then drew frame F once more with the saved camera. HCTP has no such loop; the same frames come from the
// stock frame-skip flag 0x52A328: while it is 1 at ActLevelInit 0x2e0ec0 every action level is skipped (TopAction and the
// draw lists still run). Per frame: actions (ActLevelInit) -> TopDraw (-1 -> 1, else decrement) -> draw lists
// (CGameDrawFast::Draw copies world_view 0x527C60 -> camera +0xE0 -> CRender::SetCamera). Writing 1 after TopDraw freezes
// the next frame's actions; writing world_view after TopDraw reaches every draw of this frame (stage, ropes, shadows,
// CGameDrawFast) because only camera ACTIONS write it.
// Frame sequence (trigger in the actions of frame F):
//   F .. F+119  : draw orbit step i = 0..119 (FFAlpha 0x60 = AfterImage haze), actions of F+1 .. F+120 skipped
//   F+120       : draw with the saved world_view + saved FFAlpha, matrix flag cleared (= SYM's normal draw of frame F
//                 after its loop), actions run again from F+121.
// Hook rules (CLAUDE.md): act only when the hooked original really returned (ctx->pc == ra); guest calls run with
// t_eeNoCheckpoint set and never leak t_eeCheckpointHit.
#include "ps2_runtime.h"
#include <cmath>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>

extern thread_local bool t_eeCheckpointHit; // ps2_runtime.cpp
extern thread_local bool t_eeNoCheckpoint;  // ps2_runtime.cpp

namespace
{
    // ---- HCTP addresses (SYM equivalent in brackets) ----
    constexpr uint32_t kMxMask = PS2_RAM_COMMITTED_MASK;
    constexpr uint32_t kMxGChar = 0x526EA0u;      // g_pChar[8]                        [SYM 0x405b70]
    constexpr uint32_t kMxFFAlpha = 0x526E78u;    // AfterImage alpha (Get/SetFFAlpha 0x202190/0x2021a0) [SYM via 0x14dcc0/0x14dcd0]
    constexpr uint32_t kMxPause = 0x527B08u;      // pause flag (CPauseCtrlGame2D::Action) [SYM: modal ChkPauseMenu]
    constexpr uint32_t kMxNChar = 0x527B38u;      // GameVariables::nChar              [SYM 0x3cde64]
    constexpr uint32_t kMxMatrix = 0x527BE0u;     // GameVariables::matrix (byte)      [SYM 0x3cde8c]
    constexpr uint32_t kMxMatrixChar = 0x527BE8u; // GameVariables::matrix_char        [SYM 0x3cde90]
    constexpr uint32_t kMxWorldView = 0x527C60u;  // world_view (64 B)                 [SYM 0x4eb500]
    constexpr uint32_t kMxSkip = 0x52A328u;       // frame skip flag (1 = skip all action levels)
    constexpr uint32_t kMxRule = 0x4920C0u;       // GetRule() 0x1f53f0               [SYM GetRule 0x1350d0]
    // ---- guest functions ----
    constexpr uint32_t kMxGetComActive = 0x1b9b80u; // CChar::GetComActive           [SYM 0x1513e0]
    constexpr uint32_t kMxMakeGameReplay = 0x1f7350u; // MakeGameReplay(int)        [SYM: replay=1, CGame::Action clone block]
    // ---- CChar fields (SYM offset in brackets; verified on identical code: RumbleDeadChk 0x1f81c0 vs SYM 0x139b30,
    //      Chk_SmackDown, GetPlace 0x1e0dc0 vs 0x21f7f0, CMot::Fight 0x1d0d70 vs 0x16a170, TransactionCase tails) ----
    constexpr uint32_t kMxChRefId = 0x30;     // wrestler/model id; referee = IsHebner 0x1b9a10 (0x13EC/0x13F6) [SYM == 0x1FE]
    constexpr uint32_t kMxChTarget = 0x3C;    // target CChar*                     [0x3C]
    constexpr uint32_t kMxChMotion = 0xA4;    // current motion id                 [0xA4]
    constexpr uint32_t kMxChPosPtr = 0xF0;    // -> position vec (x +0, z +8)      [SYM inline floats +0xF0 / +0xF8]
    constexpr uint32_t kMxChFlag166 = 0x172;  // byte, must be 0                   [0x166]
    constexpr uint32_t kMxChFlag167 = 0x173;  // target byte, must be 0            [0x167]
    constexpr uint32_t kMxChFightBits = 0x18A;// CMot::Fight bit 3                 [0x17A]
    constexpr uint32_t kMxChPadOn = 0x1DA;    // u16 newly pressed buttons         [0x1CA]
    constexpr uint32_t kMxChFinisher = 0x240; // finisher id (Chk_SmackDown 0x1e2c80) [0x230]
    constexpr uint32_t kMxChUsed = 0x244;     // once-per-finisher counter         [0x234]
    constexpr uint32_t kMxChSlot = 0x390;     // match slot = g_pChar index        [0x370]
    constexpr uint16_t kMxPadL2 = 0x0001, kMxPadL1 = 0x0004;
    // ---- SYM constants (float bit patterns from the SYM code) ----
    constexpr float kMxPi = 3.14159274101257324f;     // 0x40490fdb
    constexpr float kMxHalfPi = 1.57079637050628662f; // 0x3fc90fdb (sceVu0RotMatrixY 0x103288)
    constexpr float kMxTwoPi = 6.28318548202514648f;  // 0x40c90fdb
    constexpr float kMxSteps = 120.0f;                // 0x42f00000
    constexpr int kMxLoop = 0x78;                     // slti 0x78 @0x133710
    // SYM SetFFAlpha(0x60) @0x133424. The FFAlpha SCALE differs: SYM ff::DrawFin 0x14ddd4 uses ALPHA FIX = FFAlpha,
    // HCTP AfterImage::Draw 0x2022ac uses FIX = FFAlpha + 0x20 (HCTP's own max is Slow ctor 0x1d68f4 SetFFAlpha(0x40)).
    // Copying 0x60 gave FIX 0x80 = 100% previous frame: the orbit never reached the screen, only a smeared freeze
    // (Jordan Nov 12: "no spin, picture warps"). Same blend as SYM = 0x60 - 0x20.
    constexpr int32_t kMxFFHaze = 0x60 - 0x20;
    constexpr int32_t kMxReplayLen = 28;              // L2: stock camera op 3A/01 0x0E * 2 (MakeCamEff 0x208890)

    PS2Runtime::RecompiledFunction g_mxOrigTc = nullptr;
    PS2Runtime::RecompiledFunction g_mxOrigTopDraw = nullptr;
    bool g_mxReplayButton = true;

    struct MxState
    {
        bool running = false;
        uint32_t seq = 0;   // GameMainSequence that started it
        int step = 0;       // SYM s1 (0..119), 120 = the closing normal frame
        float wv0[4][4]{};  // SYM sp+0x150 (saved world_view)
        int32_t savedFF = 0;// SYM s0 (GetFFAlpha)
    } g_mxState;

    uint32_t mxRd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & kMxMask), 4); return v; }
    uint16_t mxRd16(const uint8_t *rdram, uint32_t a) { uint16_t v; std::memcpy(&v, rdram + (a & kMxMask), 2); return v; }
    float mxRdF(const uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + (a & kMxMask), 4); return v; }
    void mxWr32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & kMxMask), &v, 4); }
    bool mxPtr(uint32_t p, uint32_t size) { p &= kMxMask; return p >= 0x100000u && p + size <= PS2_RAM_COMMITTED_TOP; }
    uint32_t mxReg(const R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); }

    // Guest call from a host hook (after the hooked original returned): no checkpoint may stop the callee, the caller's
    // checkpoint flag and registers are restored.
    uint32_t mxGuestCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, std::initializer_list<uint32_t> args)
    {
        PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(addr);
        if (!fn)
            return 0u;
        const bool savedHit = t_eeCheckpointHit, savedNo = t_eeNoCheckpoint;
        const R5900Context saved = *ctx;
        t_eeNoCheckpoint = true;
        int r = 4;
        for (uint32_t a : args)
            ctx->r[r++] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a)));
        ctx->pc = addr;
        fn(rdram, ctx, runtime);
        const uint32_t ret = mxReg(ctx, 2);
        *ctx = saved;
        t_eeNoCheckpoint = savedNo;
        t_eeCheckpointHit = savedHit;
        return ret;
    }

    // CMot::Fight(const CChar*) HCTP 0x1d0d70 (pure, read-only): motion in [0x6A4, 0x708) or == 0x2EF7 -> 1, else bit 3
    // of byte +0x18A. SYM 0x16a170 is the same without the 0x2EF7 case.
    bool mxFight(const uint8_t *rdram, uint32_t ch)
    {
        const int32_t m = static_cast<int32_t>(mxRd32(rdram, ch + kMxChMotion));
        if ((m >= 0x6A4 && m < 0x708) || m == 0x2EF7)
            return true;
        return ((rdram[(ch + kMxChFightBits) & kMxMask] >> 3) & 1u) != 0;
    }

    // SYM COperate::TransactionCase 0x228aa8..0x228bf4 (s0 = char = this+0x1C).
    void mxCheckButtons(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ch)
    {
        if (mxRd32(rdram, ch + kMxChFinisher) == 0u) return;              // 0x228aa8 lw 0x230; beqz
        if (rdram[(ch + kMxChFlag166) & kMxMask] != 0u) return;          // 0x228ab4 lbu 0x166; bnez
        if (mxRd32(rdram, ch + kMxChUsed) != 0u) return;                 // 0x228ac0 lw 0x234; bnez
        const int32_t nChar = static_cast<int32_t>(mxRd32(rdram, kMxNChar));
        if (!(nChar < 4)) return;                                       // 0x228acc slti 4
        bool ref = false;                                               // 0x228ae0..0x228b28: any g_pChar[0..7] is the referee
        for (uint32_t k = 0; k < 8u; ++k)
        {
            const uint32_t p = mxRd32(rdram, kMxGChar + 4u * k) & kMxMask;
            if (p == 0u || !mxPtr(p, 0x40u)) continue;
            const uint32_t id = mxRd32(rdram, p + kMxChRefId);
            if (id == 0x13ECu || id == 0x13F6u) ref = true;
        }
        if (!ref && nChar != 2) return;                                 // 0x228b2c..0x228b3c
        // Read the pad first (GetComActive is a pure lookup, so the order does not change the result) to keep the guest
        // call off the per-frame path.
        const uint16_t pad = mxRd16(rdram, ch + kMxChPadOn);              // 0x228b58 / 0x228b84 lhu 0x1ca
        if ((pad & (kMxPadL1 | kMxPadL2)) == 0u) return;
        if (g_mxState.running || rdram[kMxMatrix] != 0u || mxRd32(rdram, kMxPause) != 0u) return; // host guard (SYM: loop modal)
        if (mxGuestCall(rdram, ctx, runtime, kMxGetComActive, {ch}) != 0u) return; // 0x228b48 GetComActive; bnez
        if (pad & kMxPadL1)                                               // 0x228b5c andi 4
        {
            rdram[kMxMatrix] = 1u;                                      // 0x228b6c sb matrix
            mxWr32(rdram, kMxMatrixChar, mxRd32(rdram, ch + kMxChSlot));  // 0x228b70 lw 0x370 -> matrix_char
            mxWr32(rdram, ch + kMxChUsed, mxRd32(rdram, ch + kMxChUsed) + 1u); // 0x228b78 +0x234++
            std::fprintf(stderr, "[matrix] L1: char 0x%X slot %u finisher 0x%X nChar %d%c", ch, mxRd32(rdram, ch + kMxChSlot),
                         mxRd32(rdram, ch + kMxChFinisher), nChar, 10);
        }
        if ((pad & kMxPadL2) == 0u || !g_mxReplayButton) return;          // 0x228b88 andi 1
        const uint32_t tgt = mxRd32(rdram, ch + kMxChTarget) & kMxMask;   // 0x228b94 lw 0x3c; beqz
        if (tgt == 0u || !mxPtr(tgt, 0x400u)) return;
        if (mxRd32(rdram, tgt + kMxChMotion) != mxRd32(rdram, ch + kMxChMotion)) return; // 0x228ba0 same move
        if (rdram[(tgt + kMxChFlag167) & kMxMask] != 0u) return;         // 0x228bb0 lbu 0x167
        if (mxFight(rdram, ch)) return;                                 // 0x228bbc CMot::Fight
        if (rdram[kMxRule + 0x12u] != 0u) return;                       // 0x228bcc GetRule()+0x12
        // 0x228be4 replay_cnt = 0, replay = 1 -> HCTP's replay entry is MakeGameReplay(int) (GameReplay sets replay).
        mxGuestCall(rdram, ctx, runtime, kMxMakeGameReplay, {static_cast<uint32_t>(kMxReplayLen)});
        mxWr32(rdram, ch + kMxChUsed, mxRd32(rdram, ch + kMxChUsed) + 1u); // 0x228bec +0x234++
        std::fprintf(stderr, "[matrix] L2: replay char 0x%X motion 0x%X%c", ch, mxRd32(rdram, ch + kMxChMotion), 10);
    }

    void mxTransactionCase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = mxReg(ctx, 4) & kMxMask;
        const uint32_t ra = mxReg(ctx, 31);
        g_mxOrigTc(rdram, ctx, runtime);
        if (ctx->pc != ra || self == 0u || !mxPtr(self, 0x20u))
            return;
        const uint32_t ch = mxRd32(rdram, self + 0x1Cu) & kMxMask;
        if (ch == 0u || !mxPtr(ch, 0x400u))
            return;
        mxCheckButtons(rdram, ctx, runtime, ch);
    }

    // ---- matrix math, 1:1 with the SYM code (rows = 16-byte memory rows, "apply" = row * matrix like sceVu0) ----
    using MxMat = float[4][4];
    // out[i] = a[i].x*b[0] + a[i].y*b[1] + a[i].z*b[2] + a[i].w*b[3]  (vmulax/vmadday/vmaddaz/vmaddw, rows of a through b)
    void mxMul(MxMat out, MxMat a, MxMat b)
    {
        MxMat t;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        std::memcpy(out, t, sizeof(MxMat));
    }
    // sceVu0InversMatrix (SYM 0x102eb8, HCTP 0x1039b8): rigid inverse. Rows 0-2 = transpose of the 3x3 with w = 0,
    // row 3 xyz = -(t.x*row0 + t.y*row1 + t.z*row2) of the result, row 3 w kept.
    void mxInverse(MxMat out, MxMat m)
    {
        MxMat t;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                t[i][j] = m[j][i];
            t[i][3] = 0.0f;
        }
        for (int j = 0; j < 3; ++j)
            t[3][j] = -(m[3][0] * t[0][j] + m[3][1] * t[1][j] + m[3][2] * t[2][j]);
        t[3][3] = m[3][3];
        std::memcpy(out, t, sizeof(MxMat));
    }
    // objMatrix::RotY(float) (SYM 0x12d7a0, HCTP 0x144640): unit matrix, angle wrapped once into [-pi, pi], then
    // sceVu0RotMatrixY(m, m, a) 0x103288: cos = sin(pi/2 -/+ a) (VU polynomial), sin = +/-sqrt(1 - cos^2) with the sign of a;
    // rows (c,0,-s,0) (0,1,0,0) (s,0,c,0) (0,0,0,1).
    void mxRotY(MxMat out, float a)
    {
        if (!(a <= kMxPi)) { a = a - kMxTwoPi; if (a < -kMxPi) a = -kMxPi; }
        else if (a < -kMxPi) { a = kMxTwoPi + a; if (!(a <= kMxPi)) a = kMxPi; }
        const bool neg = a < 0.0f;
        const float tt = neg ? kMxHalfPi + a : kMxHalfPi - a;
        const float c = std::sin(tt);
        float s = std::sqrt(std::fmax(0.0f, 1.0f - c * c));
        if (neg) s = -s;
        MxMat r = {{c, 0.0f, -s, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {s, 0.0f, c, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
        std::memcpy(out, r, sizeof(MxMat));
    }
    // SYM 0x133434..0x133664 for step i: world_view = Inv( Inv(wv0) * T(-c) * RotY(2*pi*i/120) * T(c) ), c = (x, 0, z)
    // of matrix_char.
    void mxOrbit(MxMat out, MxMat wv0, float cx, float cz, int i)
    {
        MxMat T = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {cx, 0, cz, 1}}; // 0x133438 unit (vmr32 of vf0) + 0x13348c/0x133498
        MxMat C;
        mxInverse(C, wv0);                                                 // 0x133468..0x1334a8 sp+0x90 = Inv(saved)
        const float ang = (kMxTwoPi * static_cast<float>(i)) / kMxSteps;   // 0x1334b0..0x1334d8 mul.s then div.s
        MxMat R;
        mxRotY(R, ang);                                                    // 0x1334e4 RotY(sp+0x110)
        mxMul(R, R, T);                                                    // 0x1334f4..0x133560 R = R * T(+c)
        T[3][0] = -cx;                                                     // 0x133564..0x133578 neg.s x, z
        T[3][2] = -cz;
        mxMul(R, T, R);                                                    // 0x13357c..0x1335e8 R = T(-c) * R
        mxMul(C, C, R);                                                    // 0x1335ec..0x13365c C = C * R
        mxInverse(out, C);                                                 // 0x133660 InversMatrix
    }

    void mxEnd(uint8_t *rdram, bool restoreView, const char *why)
    {
        if (restoreView)
            std::memcpy(rdram + kMxWorldView, g_mxState.wv0, 64);              // 0x133724..0x13374c world_view = saved
        mxWr32(rdram, kMxFFAlpha, static_cast<uint32_t>(g_mxState.savedFF));    // 0x13371c SetFFAlpha(saved)
        rdram[kMxMatrix] = 0u;                                              // 0x133750 matrix = 0
        g_mxState.running = false;
        std::fprintf(stderr, "[matrix] end (%s) after %d steps%c", why, g_mxState.step, 10);
    }

    // After GameMainSequence::TopDraw (once per frame, after all actions, before the draw lists).
    void mxTopDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = mxReg(ctx, 4) & kMxMask;
        const uint32_t ra = mxReg(ctx, 31);
        g_mxOrigTopDraw(rdram, ctx, runtime);
        if (ctx->pc != ra)
            return;
        if (!g_mxState.running)
        {
            if (static_cast<int8_t>(rdram[kMxMatrix]) != 1)                 // 0x133268 lb matrix; bne 1
                return;
            if (mxRd32(rdram, kMxPause) != 0u)
                return;
            g_mxState.running = true;
            g_mxState.seq = self;
            g_mxState.step = 0;                                                  // 0x133430 s1 = 0
            std::memcpy(g_mxState.wv0, rdram + kMxWorldView, 64);                // 0x1332a8..0x1332d8 save world_view
            g_mxState.savedFF = static_cast<int32_t>(mxRd32(rdram, kMxFFAlpha)); // 0x133418 GetFFAlpha
            std::fprintf(stderr, "[matrix] start: char slot %u FFAlpha %d -> 0x40 (SYM 0x60)%c", mxRd32(rdram, kMxMatrixChar),
                         g_mxState.savedFF, 10);
        }
        else if (self != g_mxState.seq || static_cast<int8_t>(rdram[kMxMatrix]) != 1)
        {
            mxEnd(rdram, false, "match changed"); // GameVariables::Init cleared the flag or another sequence: no view restore
            return;
        }
        if (g_mxState.step >= kMxLoop)                                           // 0x133710 slti 0x78 -> loop done
        {
            mxEnd(rdram, true, "done");
            return;
        }
        const uint32_t slot = mxRd32(rdram, kMxMatrixChar);
        const uint32_t ch = slot < 8u ? (mxRd32(rdram, kMxGChar + 4u * slot) & kMxMask) : 0u; // 0x133458..0x133484
        const uint32_t pos = ch != 0u && mxPtr(ch, 0x400u) ? (mxRd32(rdram, ch + kMxChPosPtr) & kMxMask) : 0u;
        if (pos == 0u || !mxPtr(pos, 0x10u))
        {
            mxEnd(rdram, true, "no matrix_char");
            return;
        }
        MxMat wv;
        mxOrbit(wv, g_mxState.wv0, mxRdF(rdram, pos + 0u), mxRdF(rdram, pos + 8u), g_mxState.step);
        std::memcpy(rdram + kMxWorldView, wv, 64);                          // 0x133668..0x133698 world_view = orbit
        mxWr32(rdram, kMxFFAlpha, static_cast<uint32_t>(kMxFFHaze));       // 0x133424 SetFFAlpha(SYM 0x60 = HCTP 0x40) (kept every step)
        mxWr32(rdram, kMxSkip, 1u);                                         // next frame: no actions (SYM: draw-only loop)
        if (mxRd32(rdram, kMxPause) == 0u)
            ++g_mxState.step;                                                    // 0x133708 s1++
    }
}

void hctpMatrixInstall(PS2Runtime &runtime)
{
    const char *e = std::getenv("HCTP_MATRIX");
    if (!e || e[0] == '0')
        return;
    if (const char *r = std::getenv("HCTP_MATRIX_L2"); r && r[0] == '0')
        g_mxReplayButton = false;
    if ((g_mxOrigTc = runtime.lookupFunction(0x1e70c0u)) != nullptr && runtime.replaceFunction(0x1e70c0u, mxTransactionCase) &&
        (g_mxOrigTopDraw = runtime.lookupFunction(0x2e0e70u)) != nullptr && runtime.replaceFunction(0x2e0e70u, mxTopDraw))
        std::cerr << "[matrix] SYM matrix (L1) + replay (L2) during finishers on (HCTP_MATRIX=1"
                  << (g_mxReplayButton ? "" : ", L2 off") << ")" << std::endl;
}

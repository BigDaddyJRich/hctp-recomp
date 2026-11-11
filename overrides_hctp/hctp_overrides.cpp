#include <utility>
#include "hctp_restored_finishers.h"
#include <atomic>
#include <array>
// WWE SmackDown! Here Comes the Pain (USA, SLUS-20787) - runtime overrides for this game build.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides_hctp/ folder.
//
// HCTP_PCPROF=1: every 5 s, where the game thread / GS thread spend their time (host functions = recompiled game
// functions by name), via ps2_host_profile::samplePc / pcReport (Kernel/HostProfiler.cpp, needs the PDB: build-fast).
#include "ps2_runtime.h"
void hctpPyroArm(PS2Runtime *runtime, const char *id, uint32_t common); // hctp_svr06_pyro.cpp
bool hctpPyroEventTick(uint32_t scene, uint32_t frame, uint32_t frameMax); // hctp_svr06_pyro.cpp
void hctpPyroDisarm();
bool hctpPyroHas(const char *id); // hctp_svr06_pyro.cpp: PCEN + GMEV pack exists for this SvR06 event id
static int g_svr06CamEvent = 0; // Nov 9: HCTP event no of the SvR06 4044 reader event (CAM row 3944), 0 = none
#include "runtime/ee_scheduler.h"
#include "game_overrides.h"
#include "runtime/ps2_host_profile.h"
#include "ps2_host_backend.h"
// native movie player (PS2Recomp/ps2xRuntime/src/lib/hctp_native_movie.cpp)
extern "C" bool hctpMovieOpen(int ch, const char *path, int fid, bool loop);
extern "C" void hctpMovieStop(int ch);
extern "C" void hctpMoviePause(int ch, bool pause);
extern "C" int hctpMovieStatus(int ch);
extern thread_local bool t_eeCheckpointHit; // ps2_runtime.cpp (dispatchGuestBranch stub-return guard)
extern thread_local bool t_eeNoCheckpoint;  // ps2_runtime.cpp (no checkpoint stops while set)
extern "C" bool hctpMovieFrame(int ch, uint8_t *rgba, int w, int h);
#include <initializer_list>
#include <cmath>
#include <mutex>

#include <algorithm>
#include <chrono>
#include <map>
#include <deque>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <iostream>
#include <thread>

extern "C" void hctpCommentaryEvent(uint32_t event); // ps2xRuntime/src/lib/hctp_commentary.cpp
extern "C" void hctpCommentaryEntrance(uint32_t rosterId);
extern "C" void hctpCommentaryEntranceGroup(const uint32_t *rosterIds, uint32_t count);
// Nov 6 (Jordan): wrestler cast slots from the rule table (role 0/1); valets/managers (Haas with Benjamin,
// Sable with Vince) also sit in the cast table and made singles "tag" + got banter lines. 0xFF = rule unknown.
static uint32_t hctpRuleWrestlerSlotMask(const uint8_t *rdram, uint8_t ruleType)
{
    if (ruleType >= 0x56u)
        return 0xFFu;
    const uint8_t *const rt = rdram + 0x45AF60u + uint32_t(ruleType) * 0x20u;
    uint32_t m = 0;
    for (uint32_t k = 0; k < rt[1] && k < 6u; ++k)
        if (rt[0xC + 3 * k + 2] < 2u && rt[0xC + 3 * k] < 8u)
            m |= 1u << rt[0xC + 3 * k];
    return m ? m : 0xFFu;
}

static uint32_t g_entranceSlotMask = 0xFFu; // cast slots that are real wrestlers in this match (rule table 0x45AF60)
extern "C" void hctpCommentarySetTitleMatch(uint32_t belt);
extern "C" void hctpCommentaryResetEntrances();
extern "C" void hctpCommentarySetTagMatch(uint32_t tag);
extern "C" void hctpCommentarySetTeamSize(uint32_t size);
extern "C" void hctpCommentarySetMatchRule(uint32_t rule);
extern "C" void hctpCommentaryBellRule(uint32_t rule, uint32_t braPanties);
extern "C" void hctpCommentaryMove(uint32_t chara, uint32_t moveId);
extern "C" void hctpCommentaryParticipants(const uint32_t *rosterIds, uint32_t count);
extern "C" void hctpCommentaryArena(int stage);
extern "C" void hctpCommentaryBrand(int brand);
extern "C" void hctpCommentaryCharaRoster(uint32_t chara, uint32_t rosterId);
extern "C" void hctpCommentaryRumbleEntrant(uint32_t rosterId);
extern "C" void hctpCommentaryPreviewEntrance(uint32_t rosterId); // [COMMFIX Nov 6] CAM entrance preview

// ps2_runtime.cpp: pin CSR.FIELD at one guest read site (see applyHctp, flicker-free field pin).
void ps2PinCsrField(uint32_t pc, bool pin, bool field) noexcept;

// Roster expansion: cast code -> roster id for host code (commentary, PPV). Slot model nos (8600..9999) give the slot's
// TEMPLATE id (its Finkel/commentary rows), else code/100 / the CAW id. Defined in the ROSTER EXPANSION section.
uint32_t hctpRosterCodeId(const uint8_t *rdram, uint32_t code);

// Royal Rumble entrant (called from the hand-edited Draw__9CGameSyncFv_0x1f5c50.cpp right after CGameSync::Draw stores the
// slot popped from the entry queue 0x527A90 into 0x527AB8, store at 0x1f65c0). Host code only, no guest calls.
// The slot's CChar = *(0x526EA0 + slot*4) (Draw reads it the same way at 0x1f6410); CChar+0x30 = cast code.
void hctpRumbleSpawn(uint8_t *rdram, uint32_t slot)
{
    uint32_t chara = 0;
    uint16_t cast = 0xFFFFu;
    if (slot < 8u)
        std::memcpy(&chara, rdram + 0x526EA0u + slot * 4u, 4);
    if (chara >= 0x100000u && chara < 0x2000000u)
        std::memcpy(&cast, rdram + chara + 0x30u, 2);
    std::fprintf(stderr, "[hctp-pbp] rumble spawn: slot %u chara 0x%X cast %u%c", slot, chara, cast, 10);
    if (cast < 10000u)
        hctpCommentaryRumbleEntrant(hctpRosterCodeId(rdram, cast));
}

int hctpIkSvrTwistBone(const uint8_t *rdram, uint32_t model, int which); // Nov 7, defined at the end

namespace ps2_stubs
{
    void setLibSdrNative(uint32_t clientData, uint32_t commandBuffer); // Kernel/Stubs/Audio.cpp
}

namespace
{
    int ppvCommentaryBrand(); // PPV play: card brand 0 RAW / 1 SMACKDOWN, -1 = none (defined with the PPV code)
    // Cut-wrestler restore (HCTP_NO_CUT_ROSTER=1 disables): the ID sheet's "No Model" slots keep name/moveset/entrance
    // data in CHAR\ALL_.pac but have type byte +0x163 = 6 (hidden) and zeroed stats (+0..+4, value/5; order str,
    // sub/tech, speed, tech/sub, end). Models are inserted into CH/CH2.PAC by tools/build_cut_roster_pacs.py.
    // Records: 0x4C0F54 + id*0x170 (GetCharaGameProp 0x12cec0, ids < 0x56).
    struct CutWrestler
    {
        uint8_t id, type, stats[5];
        uint8_t tmpl;    // stock CHSL record cloned for the select list (finisher text, flags, costume ids)
        uint8_t attires; // attire slots with models (CH/CH2 keys XX00, XX10, XX20 ...)
        const char *name;
        int16_t entrance; // entrance event (record +0xEC) when the stock one is an empty 512-byte stub; 0 = keep
    };
    // Stopgap entrances (Jordan, Oct 20): Sean O'Haire 3071 for the men, Jazz 3084 for Molly. Planned ports: Jeff's own
    // (Jordan), Spike/Kidman/Gunn from Shut Your Mouth, Molly/Bradshaw/Jeff from SmackDown vs RAW.
    constexpr CutWrestler kCutRoster[] = {
        {0x0D, 0, {13, 12, 19, 15, 15}, 0x0C, 1, "Jeff Hardy", 0},   // keeps his own (empty) event: Jordan ports his entrance,
        {0x11, 0, {15, 19, 12, 18, 16}, 0x12, 1, "William Regal", 3071},
        {0x18, 0, {18, 13, 12, 14, 17}, 0x1A, 1, "Bradshaw", 3071},
        {0x20, 0, {0, 0, 0, 0, 0}, 0x2D, 2, "Hulk Hogan", 0},       // keeps its stats; 2 attires (Jordan Nov 5: Hollywood + Mr. America = own slots)
        {0x21, 0, {20, 13, 10, 13, 16}, 0x1D, 1, "Rosey", 3071},       // Jordan's recommended stats
        {0x23, 0, {18, 13, 14, 13, 15}, 0x1D, 1, "Jamal", 3071},       // Jordan's recommended stats
        {0x24, 0, {16, 13, 14, 15, 16}, 0x19, 1, "Billy Gunn", 3071},
        {0x29, 4, {8, 12, 14, 12, 8}, 0x0E, 1, "Molly Holly", 3084},
        {0x2B, 0, {10, 12, 17, 14, 13}, 0x17, 1, "Spike Dudley", 3071},
        {0x39, 0, {12, 13, 18, 15, 14}, 0x17, 1, "Billy Kidman", 3071},
        {0x4D, 0, {0, 0, 0, 0, 0}, 0x4C, 1, "Ultimate Warrior", 0}, // keeps its stats
        {0x4E, 0, {18, 12, 10, 12, 19}, 0x2D, 1, "Hulk Hogan (Legend)", 0},
        // Original NPC slots already have model keys in CH2. Keep their own moveset data.
        {50, 0, {10, 9, 12, 9, 8}, 22, 1, "Michael Cole", 3022},
        {52, 0, {10, 9, 11, 9, 8}, 22, 1, "Jim Ross", 3022},
        {53, 0, {17, 16, 14, 17, 17}, 21, 1, "Jerry Lawler", 3021},
        {54, 0, {9, 8, 14, 10, 8}, 22, 1, "Jonathan Coachman", 3022},
        {55, 4, {6, 7, 14, 10, 7}, 20, 1, "Linda McMahon", 3020},
        {65, 0, {17, 20, 13, 16, 19}, 18, 1, "Tazz", 3071},
        // Al Snow: stock 6300 is a pads-off Rock stand-in; his SYM model is the Content Pack's MODS\PAC\EMD2\6300,
        // so his slot stays hidden without it (scanCutRosterModels).
        {0x3F, 0, {14, 13, 14, 13, 15}, 0x17, 1, "Al Snow", 3071},
    };
    constexpr size_t kCutCount = sizeof(kCutRoster) / sizeof(kCutRoster[0]);
    bool g_cutRosterHasModel[kCutCount] = {};
    std::set<uint16_t> g_cutEntranceCodes; // decimal cast codes of restored entrance attires (e.g. 3204, 3214)
    // A slot is only un-hidden when its attire-1 model (key BCD(id) << 8) is in pac/CH.PAC (ids < 30) or pac/CH2.PAC
    // (DPAC directory at 0x800: tag, u16 count | 0x8000, then u16 key + u16 sectors per entry), or is a loose member
    // MODS/PAC/EMD/<key> / MODS/PAC/EMD2/<key> (the game asks "/EMD/%04d" / "/EMD2/%04d"; testers have stock PACs and
    // the Content Pack's loose models), so stock archives keep the vanilla roster. Returns the number of slots enabled.
    int scanCutRosterModels()
    {
        int enabled = 0;
        for (size_t i = 0; i < kCutCount; ++i)
        {
            const uint8_t id = kCutRoster[i].id;
            std::error_code ec;
            if (id == 0x3F && !std::filesystem::exists("MODS/PAC/EMD2/6300", ec))
                continue; // no Content Pack: the stock key is a Rock stand-in
            std::vector<uint16_t> keys;
            std::ifstream f(id < 30 ? "pac/ch.pac" : "pac/ch2.pac", std::ios::binary);
            uint8_t dir[0x4000] = {};
            if (f.read(reinterpret_cast<char *>(dir), sizeof(dir)))
            {
                uint16_t n;
                std::memcpy(&n, dir + 0x804, 2);
                n &= 0x7FFFu;
                for (uint32_t e = 0; e < n && 0x808u + e * 4u + 2u <= sizeof(dir); ++e)
                {
                    uint16_t k;
                    std::memcpy(&k, dir + 0x808 + e * 4, 2);
                    keys.push_back(k);
                }
            }
            for (const auto &e : std::filesystem::directory_iterator(id < 30 ? "MODS/PAC/EMD" : "MODS/PAC/EMD2", ec))
            {
                const std::string name = e.path().filename().string();
                if (name.size() == 4 && e.is_regular_file(ec) && e.file_size(ec) != 0 &&
                    name.find_first_not_of("0123456789") == std::string::npos)
                    keys.push_back(static_cast<uint16_t>(std::stoul(name, nullptr, 16))); // decimal digits = BCD key
            }
            const uint16_t key = static_cast<uint16_t>(((id / 10) << 12) | ((id % 10) << 8));
            for (const uint16_t k : keys)
            {
                if (k == key && !g_cutRosterHasModel[i])
                {
                    g_cutRosterHasModel[i] = true;
                    ++enabled;
                }
                // HOGAN GEAR FIX (Nov 6): GetEntranceCastNo swaps the WHOLE body for the XXA4 cast, so only full
                // models may be routed. Hogan's 3204/3214/3224 (SYM gear: FusaW2/Glass/H1_*) are gear-only (no
                // PAC type-8 entry) -> body vanished = floating glasses/boa/shirt. Gear belongs in the XXA2
                // dress, which the game draws over the ring body (like stock HBK 4502).
                auto fullModelPack = [&](uint16_t kk) {
                    char p[32];
                    std::snprintf(p, sizeof(p), "%s/%04X", id < 30 ? "MODS/PAC/EMD" : "MODS/PAC/EMD2", kk);
                    std::ifstream pf(p, std::ios::binary);
                    if (!pf)
                        return true; // stock archive member: stock XXX4 casts are full models
                    uint8_t h[8 + 16 * 8] = {};
                    pf.read(reinterpret_cast<char *>(h), sizeof(h));
                    uint32_t n = 0;
                    std::memcpy(&n, h + 4, 4);
                    for (uint32_t e = 0; e < n && e < 16; ++e)
                        if ((h[8 + e * 8] == 8 || h[8 + e * 8] == 5) && h[9 + e * 8] == 0)
                            return true; // Nov 7: SvR06 bodies (Legend 7804 = retail 7201) have type 5, no type 8
                    return false;
                };
                if ((k & 0xFF00u) == key && (k & 0xFu) == 4u && fullModelPack(k))
                    g_cutEntranceCodes.insert(static_cast<uint16_t>(id * 100u + ((k >> 4) & 0xFu) * 10u + 4u));
            }
        }
        return enabled;
    }
    // Roster-wide gloss reduction: CSkinModel::DrawChar 0x155dc0 multiplies every material colour constant by the
    // vector at 0x40F330. Its .w (0x40F33C, normally 128.0) scales the excessive reflection/gloss strength. Use 32.0
    // around every wrestler draw, then restore the game constant. HCTP_FULL_GLOSS=1 keeps the original appearance.
    bool g_entranceSnapFresh = false; // [COMMFIX Nov 6] moved up: used before its old declaration
    PS2Runtime::RecompiledFunction g_origCharDraw = nullptr;
    int g_drawCharBleed = 0; // Nov 7 SvR blood layer: CChar+0x390 blood level of the CChar being drawn (0 = none / menu)
    void charDrawReducedGloss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float saved, dull = 32.0f;
        std::memcpy(&saved, rdram + 0x40F33Cu, 4);
        std::memcpy(rdram + 0x40F33Cu, &dull, 4);
        const uint32_t ch = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        int lvl = 0; // Nov 7: bleeding = byte CChar+0x383 (UpdateBloodTex 0x1dc5e4 sets it when blood is applied);
                     // +0x390 level is already non-zero on the matchup screen (Jordan's shot: Bret blood shown)
        if (ch != 0u && ch <= PS2_RAM_SIZE - 0x394u)
            lvl = rdram[ch + 0x383u];
        const int prevBleed = g_drawCharBleed;
        g_drawCharBleed = lvl > 0 ? lvl : 0;
        g_origCharDraw(rdram, ctx, runtime);
        g_drawCharBleed = prevBleed;
        std::memcpy(rdram + 0x40F33Cu, &saved, 4);
    }
    // Nov 7 SvR BLOOD LAYER (notes\ANY_BONE_ANIMATION_RESEARCH_20261106.md s16/s18): SvR04 models carry an overlay
    // texture named "blood"; SvR04 CChar ctor 0x1acda0 stores its index (CChar+0x544) and draw 0x1ae520 skips meshes
    // with it until bleeding (+0x393). HCTP bleeds by face swap (UpdateBloodTex 0x1dc580, level CChar+0x390) and never
    // skips -> red patch. Same skip here: CSkinModel::DrawChar 0x155dc0 (a0 = model; +0xC8 = YOBJ+8: +0x10 n_mesh,
    // +0x1C meshes 64 B (+4 n_mat, +0xC mats 0xD0 B: +0x28 tex index, +0xC2 qwc of the REF tag, +0xCC addr),
    // +0x18 n_tex, +0x24 tex names 16 B). Blood materials get qwc 0 for the call (empty REF = nothing drawn), restored
    // after. Shown while the drawing CChar (CChar::Draw 0x1bb200 hook) has blood level > 0. HCTP_SVR_BLOOD=0 = off.
    PS2Runtime::RecompiledFunction g_origDrawCharBlood = nullptr;
    // Nov 7 BLOOD FLAG RESET: CChar+0x383 (blood face applied) is written ONLY by UpdateBloodTex 0x1dc5e4 and
    // UpdateBloodEdit 0x1dc46c (bleed via SetAttr 0x1b38e8 / match load / season nvBlood) and never cleared; the CChar
    // ctor 0x1b9c40 does not set it, so menu chars (MatchWrestler::MakeChar 0x345700 = matchup screen) can carry
    // leftover memory there -> the SvR blood layer showed on the matchup screen. (+0x390 is the match slot 0-5,
    // SetCsnBlood 0x251e30, not a blood level.) Zero it once the ctor really returned.
    PS2Runtime::RecompiledFunction g_origCharCtor = nullptr;
    void charCtorClearBlood(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        g_origCharCtor(rdram, ctx, runtime);
        if (ctx->pc == ra && self != 0u && self <= PS2_RAM_SIZE - 0x384u)
            rdram[self + 0x383u] = 0u;
    }
    // TEMP diag HCTP_REPLAY_PROBE=1 (Nov 7, replay restore, notes\REPLAY_RESTORE_PLAN_20261107.md): log-only.
    // MakeCamEff(type,len) 0x208890 (type 1 = replay), MakeGameReplay(int) 0x1f7350, GameReplay::Action 0x1f6d50
    // state (+0xF64A8) changes + frame counter +0x24, and game2D(*0x529300) REPLAY graphic flags +0x4B7C/+0x4B80.
    // TEMP diag HCTP_FLARE_PROBE=1 (Nov 7, flares through wrestlers): CGmLensFlare::CalcLuminusFromZbuffer 0x228bf0
    // compares 16 Z24 samples (+0xC0, 3 B each, from ReadZbuffer 0x228d40) with the flare depth (+0x58 float -> u32);
    // sample < flare = nothing in front -> brighter. Log-only, every 30th call.
    PS2Runtime::RecompiledFunction g_origFlareCalc = nullptr;
    void flareCalcProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        if (self != 0u && self <= PS2_RAM_SIZE - 0xF0u)
        {
            static uint32_t s_n = 0;
            if ((s_n++ % 30u) == 0u && s_n < 30000u)
            {
                float fz = 0.0f, sx = 0.0f, sy = 0.0f;
                std::memcpy(&fz, rdram + self + 0x58u, 4);
                std::memcpy(&sx, rdram + self + 0x50u, 4);
                std::memcpy(&sy, rdram + self + 0x54u, 4);
                char buf[400]; int o = 0;
                for (int i = 0; i < 16; ++i)
                {
                    const uint8_t *p = rdram + self + 0xC0u + i * 3u;
                    o += std::snprintf(buf + o, sizeof(buf) - o, " %06X", p[0] | (p[1] << 8) | (p[2] << 16));
                }
                std::fprintf(stderr, "[flare] obj 0x%X scr %.1f,%.1f z %.0f samples%s%c", self, sx, sy, fz, buf, 10);
            }
        }
        g_origFlareCalc(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction g_origMakeCamEff = nullptr, g_origMakeReplayI = nullptr, g_origReplayAction = nullptr;
    void replayProbeFlags(const uint8_t *rdram, uint32_t &f7c, uint32_t &f80)
    {
        uint32_t g2d = 0u; f7c = 0xFFFFFFFFu; f80 = 0xFFFFFFFFu;
        std::memcpy(&g2d, rdram + 0x529300u, 4); g2d &= PS2_RAM_MASK;
        if (g2d != 0u && g2d <= PS2_RAM_SIZE - 0x4B84u) { std::memcpy(&f7c, rdram + g2d + 0x4B7Cu, 4); f80 = rdram[g2d + 0x4B80u]; }
    }
    void makeCamEffProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t type = _mm_cvtsi128_si32(ctx->r[4]), len = _mm_cvtsi128_si32(ctx->r[5]);
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        std::fprintf(stderr, "[replay] MakeCamEff type %d len %d ra 0x%X%c", type, len, ra, 10);
        g_origMakeCamEff(rdram, ctx, runtime);
    }
    // v4: move camera CCamMot via its vtable thunk 0x2088f0 (a0 -= 0x10 -> Action 0x203630). Script args +0x128/+0x12C/
    // +0x130 (passed to 0x168aa0 with frame = +0x134 >> 1). Logs each camera's start and the last frame it reached, so a
    // run where the replay command did not fire shows whether the camera was cut short (0x526E80 == 0 / script end).
    PS2Runtime::RecompiledFunction g_origCamMotThunk = nullptr;
    void camMotThunkProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = (static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) - 0x10u) & PS2_RAM_MASK;
        g_origCamMotThunk(rdram, ctx, runtime);
        if (self == 0u || self > PS2_RAM_SIZE - 0x138u)
            return;
        static uint32_t s_obj = 0u, s_k1 = 0u, s_k2 = 0u, s_k3 = 0u, s_cnt = 0u;
        uint32_t k1, k2, k3, cnt, live = 0u;
        std::memcpy(&k1, rdram + self + 0x128u, 4); std::memcpy(&k2, rdram + self + 0x12Cu, 4);
        std::memcpy(&k3, rdram + self + 0x130u, 4); std::memcpy(&cnt, rdram + self + 0x134u, 4);
        std::memcpy(&live, rdram + 0x526E80u, 4);
        if (self != s_obj || k1 != s_k1 || k2 != s_k2 || k3 != s_k3 || cnt < s_cnt)
        {
            if (s_obj != 0u)
                std::fprintf(stderr, "[replay] cam end obj 0x%X script %X/%X/%X reached frame %u%c", s_obj, s_k1, s_k2, s_k3, s_cnt >> 1, 10);
            std::fprintf(stderr, "[replay] cam start obj 0x%X script %X/%X/%X 526E80 0x%X%c", self, k1, k2, k3, live, 10);
            s_obj = self; s_k1 = k1; s_k2 = k2; s_k3 = k3;
        }
        s_cnt = cnt;
    }
    void makeGameReplayIProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t frames = _mm_cvtsi128_si32(ctx->r[4]);
        uint32_t lnk = 0u; std::memcpy(&lnk, rdram + 0x527970u, 4);
        std::fprintf(stderr, "[replay] MakeGameReplay(%d) list 0x%X%c", frames, lnk, 10);
        g_origMakeReplayI(rdram, ctx, runtime);
    }
    void gameReplayActionProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        const bool ok = self != 0u && self <= PS2_RAM_SIZE - 0xF64B0u;
        uint32_t s0 = 0u, s1 = 0u, n = 0u, len = 0u;
        if (ok) std::memcpy(&s0, rdram + self + 0xF64A8u, 4);
        g_origReplayAction(rdram, ctx, runtime);
        if (!ok) return;
        std::memcpy(&s1, rdram + self + 0xF64A8u, 4);
        std::memcpy(&n, rdram + self + 0x24u, 4);
        std::memcpy(&len, rdram + self + 0x28u, 4);
        static uint32_t s_last7c = 0xFFFFFFFEu;
        uint32_t f7c, f80; replayProbeFlags(rdram, f7c, f80);
        if (s0 != s1 || f7c != s_last7c)
            std::fprintf(stderr, "[replay] Action obj 0x%X state %u -> %u cnt %u len %u game2D 4B7C %u 4B80 %u%c",
                         self, s0, s1, n, len, f7c, f80, 10);
        s_last7c = f7c;
    }
    // The vtable entry is the @28@ thunk 0x1f76e0 (a0 -= 0x1C, then a direct C++ call into 0x1f6d50 that skips the
    // table hook above), so hook the thunk too. MakeGameReplay() 0x1f72f0 is gated in MakeGameMain by rule+0x11/+0x12
    // == 0 and IsArena() (GetNowStage < 0x1E or 0x27/0x2A/0x30/0x31/0x34/0x35): log creation.
    // HCTP_REPLAY=1 (Nov 7): restore the cut REPLAY HUD. Nothing in HCTP sets game2D(*0x529300)+0x4B7C (HUD mode:
    // 0 normal, 1 = DrawReplay, Draw 0x23c6c0) / +0x4B80 (CtrlReplayAnim 0x2379c0 builds anim 0x2710 when ==1 and
    // +0x4B7C != 0, deletes it when ==1 and +0x4B7C == 0). Set both when GameReplay enters playback (state 1 -> 2),
    // drop +0x4B7C when it returns to 0 (+0x4B80 stays 1 so CtrlReplayAnim frees the anim; InitAnimCtrlFlag resets).
    // Host writes only, after the guest Action really returned.
    // REPLAY texture set: CtrlReplayAnim asks GetTexInfosKey 0x1a8060 for set 0 (call at 0x237a7c, ra 0x237a84), but
    // GAME2D.PAC /GA2D/DALL only has sets 1..0xF; every other match anim uses one of them (Button 1, RopeBreak 2,
    // GiveUp 3, DQ 4, Draw 5, KO 6, Pin 7/8, Count 9, RingOut A, TimeUp B, Winner D, Demo F) -> 0xC is the unused one.
    // HCTP_REPLAY_TEX=<hex> overrides (default C).
    PS2Runtime::RecompiledFunction g_origGetTexInfosKey = nullptr;
    void getTexInfosKeyReplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        if (ra == 0x237a84u && (_mm_cvtsi128_si32(ctx->r[5]) & 0xFFFF) == 0)
        {
            static const uint32_t s_set = std::getenv("HCTP_REPLAY_TEX") ? std::strtoul(std::getenv("HCTP_REPLAY_TEX"), nullptr, 16) : 0xCu;
            ctx->r[5] = _mm_cvtsi32_si128(static_cast<int>(s_set));
            g_origGetTexInfosKey(rdram, ctx, runtime);
            std::fprintf(stderr, "[replay] REPLAY tex set 0 -> 0x%X, TexInfos 0x%X%c", s_set,
                         static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2])), 10);
            return;
        }
        g_origGetTexInfosKey(rdram, ctx, runtime);
    }
    // GameMainSequence::ActLevelInit 0x2e0ec0 (vtable, a1 = action level, a2 = default) returns 0 for level 4 while
    // GameVariables::replay 0x527BF0 == 1 -> the 2D HUD (CSpriteCtrlGame2D CAction 4) and MenuAnimObject(4,7) anims
    // freeze during replay, so the REPLAY anim never leaves its first frame. With HCTP_REPLAY=1 and the REPLAY HUD on
    // (game2D+0x4B7C == 1), let level 4 run (same result as the non-replay path: a2 unless 0x52A328 == 1).
    PS2Runtime::RecompiledFunction g_origActLevelInit = nullptr;
    void actLevelInitReplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t level = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const int32_t dflt = _mm_cvtsi128_si32(ctx->r[6]);
        g_origActLevelInit(rdram, ctx, runtime);
        if (level != 4u || rdram[0x527BF0u] != 1u)
            return;
        uint32_t paused = 0u, other = 0u, g2d = 0u, hud = 0u;
        std::memcpy(&paused, rdram + 0x527B08u, 4);
        std::memcpy(&other, rdram + 0x52A328u, 4);
        std::memcpy(&g2d, rdram + 0x529300u, 4); g2d &= PS2_RAM_MASK;
        if (g2d != 0u && g2d <= PS2_RAM_SIZE - 0x4B84u) std::memcpy(&hud, rdram + g2d + 0x4B7Cu, 4);
        if (paused != 1u && other != 1u && hud == 1u)
            ctx->r[2] = _mm_cvtsi32_si128(dflt);
    }
    PS2Runtime::RecompiledFunction g_origReplayThunkOn = nullptr;
    uint32_t ppvCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, std::initializer_list<uint32_t> args);
    uint32_t g_replaySprCtrl = 0u; // CSpriteCtrlGame2D (a0 of CtrlReplayAnim 0x2379c0), captured each frame
    PS2Runtime::RecompiledFunction g_origCtrlReplayAnimOn = nullptr;
    void ctrlReplayAnimCapture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_replaySprCtrl = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        g_origCtrlReplayAnimOn(rdram, ctx, runtime);
    }
    void gameReplayThunkRestore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = (static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) - 0x1Cu) & PS2_RAM_MASK;
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        const bool ok = self != 0u && self <= PS2_RAM_SIZE - 0xF64B0u;
        uint32_t s0 = 0u, s1 = 0u;
        if (ok) std::memcpy(&s0, rdram + self + 0xF64A8u, 4);
        g_origReplayThunkOn(rdram, ctx, runtime);
        if (!ok || ctx->pc != ra)
            return;
        std::memcpy(&s1, rdram + self + 0xF64A8u, 4);
        uint32_t g2d = 0u;
        std::memcpy(&g2d, rdram + 0x529300u, 4); g2d &= PS2_RAM_MASK;
        if (g2d == 0u || g2d > PS2_RAM_SIZE - 0x4B84u)
            return;
        // v2: set at record start (0 -> 1). Probe v5: CSpriteCtrlGame2D::Action (-> CtrlReplayAnim, which builds the
        // anim) does not run during playback states 2-4, so flags set at 1 -> 2 were never seen.
        // v3: state 1 is set by MakeGameReplay(int) from the camera, not by Action, so key on "recording/playing and
        // HUD flag still 0" instead of a 0 -> 1 edge.
        uint32_t cur7c = 0u;
        std::memcpy(&cur7c, rdram + g2d + 0x4B7Cu, 4);
        if (s1 >= 1u && cur7c == 0u)
        {
            const uint32_t one = 1u;
            std::memcpy(rdram + g2d + 0x4B7Cu, &one, 4);
            rdram[g2d + 0x4B80u] = 1u;
            std::fprintf(stderr, "[replay] REPLAY HUD on%c", 10);
        }
        else if (s0 != 0u && s1 == 0u)
        {
            const uint32_t zero = 0u;
            std::memcpy(rdram + g2d + 0x4B7Cu, &zero, 4);
            std::fprintf(stderr, "[replay] REPLAY HUD off%c", 10);
        }
        // v4: playback (states 2-4) freezes action level 4 (ActLevelInit), which also holds the REPLAY MenuAnimObject
        // (CSpriteCtrlGame2D+0x4618, made by CtrlReplayAnim). Step only that object: MenuAnimObject::Action 0x196df0
        // = CSprAnim::FrameActive + Updata on +0x40. Wrestlers stay frozen.
        if (s1 >= 2u && g_replaySprCtrl != 0u && g_replaySprCtrl <= PS2_RAM_SIZE - 0x461Cu)
        {
            uint32_t obj = 0u;
            std::memcpy(&obj, rdram + g_replaySprCtrl + 0x4618u, 4);
            obj &= PS2_RAM_MASK;
            if (obj != 0u && obj <= PS2_RAM_SIZE - 0xA0u)
                ppvCall(rdram, ctx, runtime, 0x196df0u, {obj});
        }
    }
    PS2Runtime::RecompiledFunction g_origReplayThunk = nullptr, g_origMakeReplayV = nullptr;
    PS2Runtime::RecompiledFunction g_origBlackLine = nullptr, g_origFixCamReplay = nullptr;
    uint32_t g_replayMats = 0u; uint32_t *g_replayOverw = nullptr; const uint8_t *g_replayLeft = nullptr;
    void gameReplayThunkProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = (static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) - 0x1Cu) & PS2_RAM_MASK;
        const bool ok = self != 0u && self <= PS2_RAM_SIZE - 0xF64B0u;
        uint32_t s0 = 0u, s1 = 0u, n = 0u, len = 0u;
        if (ok) std::memcpy(&s0, rdram + self + 0xF64A8u, 4);
        if (g_replayMats != 0u && g_replayOverw && g_replayLeft && std::memcmp(rdram + g_replayMats, g_replayLeft, 64) != 0)
            ++*g_replayOverw;
        g_origReplayThunk(rdram, ctx, runtime);
        if (!ok) return;
        std::memcpy(&s1, rdram + self + 0xF64A8u, 4);
        std::memcpy(&n, rdram + self + 0x24u, 4);
        std::memcpy(&len, rdram + self + 0x28u, 4);
        static uint32_t s_last7c = 0xFFFFFFFEu;
        static bool s_first = true;
        uint32_t f7c, f80; replayProbeFlags(rdram, f7c, f80);
        // v3: ms timestamp + Action calls spent in the old state; playback check = char0 bone0 matrix (+0x34 -> +0xD8)
        // at entry vs what Action left there last call (differs = live animation overwrote the replayed pose).
        static uint32_t s_calls = 0, s_overw = 0, s_play = 0;
        static uint8_t s_left[64];
        static const auto s_t0 = std::chrono::steady_clock::now();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_t0).count();
        ++s_calls;
        if (s0 != s1 || f7c != s_last7c || s_first)
        {
            std::fprintf(stderr, "[replay] Action(vt) t %lld ms obj 0x%X state %u -> %u after %u calls cnt %u len %u game2D 4B7C %u 4B80 %u"
                         " | playback calls %u, pose overwritten %u%c", ms, self, s0, s1, s_calls, n, len, f7c, f80, s_play, s_overw, 10);
            s_calls = 0; s_play = 0; s_overw = 0;
        }
        uint32_t ch = 0u, cm = 0u, mats = 0u;
        std::memcpy(&ch, rdram + 0x526EA0u, 4); ch &= PS2_RAM_MASK;
        if (ch != 0u && ch <= PS2_RAM_SIZE - 0x40u) { std::memcpy(&cm, rdram + ch + 0x34u, 4); cm &= PS2_RAM_MASK; }
        if (cm != 0u && cm <= PS2_RAM_SIZE - 0xE0u) { std::memcpy(&mats, rdram + cm + 0xD8u, 4); mats &= PS2_RAM_MASK; }
        if (s1 >= 2u && mats != 0u && mats <= PS2_RAM_SIZE - 64u)
        {
            ++s_play;
            std::memcpy(s_left, rdram + mats, 64); // compared at the next call's entry (below, via g_replayLeftValid)
        }
        s_first = false;
        s_last7c = f7c;
        g_replayMats = (s1 >= 2u) ? mats : 0u;
        g_replayOverw = &s_overw;
        g_replayLeft = s_left;
    }
    // v5: CtrlReplayAnim 0x2379c0 (a0 = CSpriteCtrlGame2D). Anim object lives at +0x4618 (created from MenuAnimData
    // +0x45C8, anim key 0x2710, only if CanUse). Log the flags + object before/after whenever anything changes.
    PS2Runtime::RecompiledFunction g_origCtrlReplayAnim = nullptr;
    void ctrlReplayAnimProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        const bool ok = self != 0u && self <= PS2_RAM_SIZE - 0x4700u;
        uint32_t o0 = 0u, o1 = 0u, lib0 = 0u, lib1 = 0u;
        if (ok) std::memcpy(&o0, rdram + self + 0x4618u, 4);
        g_origCtrlReplayAnim(rdram, ctx, runtime);
        if (!ok) return;
        std::memcpy(&o1, rdram + self + 0x4618u, 4);
        std::memcpy(&lib0, rdram + self + 0x45C8u, 4); std::memcpy(&lib1, rdram + self + 0x45CCu, 4);
        uint32_t f7c, f80; replayProbeFlags(rdram, f7c, f80);
        static uint32_t s_k = 0xFFFFFFFFu;
        const uint32_t k = o1 ^ (f7c << 1) ^ (f80 << 2);
        if (k != s_k || o0 != o1)
            std::fprintf(stderr, "[replay] CtrlReplayAnim this 0x%X 4B7C %u 4B80 %u anim obj 0x%X -> 0x%X lib +45C8 %08X %08X%c",
                         self, f7c, f80, o0, o1, lib0, lib1, 10);
        s_k = k;
    }
    void makeBlackLineProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[replay] MakeBlackLine(%d)%c", _mm_cvtsi128_si32(ctx->r[4]), 10);
        g_origBlackLine(rdram, ctx, runtime);
    }
    void makeFixCamReplayProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[replay] MakeFixCameraReplay chars 0x%X 0x%X frames %d%c", _mm_cvtsi128_si32(ctx->r[4]),
                     _mm_cvtsi128_si32(ctx->r[5]), _mm_cvtsi128_si32(ctx->r[8]), 10);
        g_origFixCamReplay(rdram, ctx, runtime);
    }
    void makeGameReplayVProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[replay] MakeGameReplay() task created%c", 10);
        g_origMakeReplayV(rdram, ctx, runtime);
    }
    std::map<uint32_t, std::vector<uint32_t>> g_bloodMats;
    std::map<uint32_t, uint16_t> g_bloodQwc; // material addr -> its real REF qwc (read when the model is first seen) // key = skinData ^ (n_mesh << 24); value = material addrs
    std::map<uint32_t, uint32_t> g_mprobeFast; // TEMP diag mprobe2: FastUpdate calls per model
    void recWatchCheck(const uint8_t *rdram, const char *tag, uint32_t ra); // TEMP diag recwatch (below)
    void drawCharSvrBlood(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        uint32_t sd = 0u;
        if (model != 0u && model <= PS2_RAM_SIZE - 0xCCu)
            std::memcpy(&sd, rdram + model + 0xC8u, 4);
        sd &= PS2_RAM_MASK;
        // TEMP diag HCTP_MATCHUP_PROBE=1 (Hogan 80s invisible on matchup): every ~1 s per model: DrawChar args, bones,
        // world matrices (+0xD0, 64 B a bone) that are NaN/huge/degenerate, root/min/max translation, axis length of bone 0.
        static const bool s_mprobe = std::getenv("HCTP_MATCHUP_PROBE") != nullptr;
        recWatchCheck(rdram, "DrawChar", static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])));
        if (s_mprobe && sd != 0u && sd <= PS2_RAM_SIZE - 0x28u)
        {
            static std::map<uint32_t, uint32_t> s_cnt;
            if ((s_cnt[model]++ % 60u) == 0u)
            {
                uint32_t bones = 0, world = 0, nMesh = 0;
                std::memcpy(&bones, rdram + sd + 0x14u, 4);
                std::memcpy(&nMesh, rdram + sd + 0x10u, 4);
                std::memcpy(&world, rdram + model + 0xD0u, 4);
                world &= PS2_RAM_MASK;
                int bad = 0, firstBad = -1, zeroAx = 0;
                float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}, ax0 = 0.f;
                if (bones <= 256u && world != 0u && world + bones * 64u <= PS2_RAM_SIZE)
                    for (uint32_t b = 0; b < bones; ++b)
                    {
                        float m[16];
                        std::memcpy(m, rdram + world + b * 64u, 64);
                        bool nb = false;
                        for (float v : m) if (!std::isfinite(v) || std::fabs(v) > 1e6f) nb = true;
                        const float ax = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                        if (b == 0u) ax0 = ax;
                        if (ax < 0.01f) ++zeroAx;
                        if (nb) { ++bad; if (firstBad < 0) firstBad = static_cast<int>(b); continue; }
                        for (int i = 0; i < 3; ++i) { mn[i] = std::min(mn[i], m[12 + i]); mx[i] = std::max(mx[i], m[12 + i]); }
                    }
                std::fprintf(stderr, "[mprobe] model 0x%X ymp 0x%X a1 %d a2 0x%X a3 %d mesh %u bones %u world 0x%X bad %d (first %d) zeroAxis %d ax0 %.3f min %.1f %.1f %.1f max %.1f %.1f %.1f%c",
                             model, sd, static_cast<int>(_mm_cvtsi128_si32(ctx->r[5])), static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6])),
                             static_cast<int>(_mm_cvtsi128_si32(ctx->r[7])), nMesh, bones, world, bad, firstBad, zeroAx, ax0,
                             mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], 10);
                {   // mprobe 2: locals (+0xCC) translation of bones 1,2,10; bone record 1 (+0xF0, 80 B) floats +0x10..+0x3C;
                    // count (+0xEC); FastUpdate calls since last line
                    uint32_t loc = 0, rec = 0, cnt = 0;
                    std::memcpy(&loc, rdram + model + 0xCCu, 4); std::memcpy(&rec, rdram + model + 0xF0u, 4);
                    std::memcpy(&cnt, rdram + model + 0xECu, 4);
                    loc &= PS2_RAM_MASK; rec &= PS2_RAM_MASK;
                    float lt[9] = {}, rf[12] = {};
                    if (loc && loc + 11u * 64u <= PS2_RAM_SIZE)
                    {
                        std::memcpy(lt, rdram + loc + 64u + 48u, 12); std::memcpy(lt + 3, rdram + loc + 128u + 48u, 12);
                        std::memcpy(lt + 6, rdram + loc + 640u + 48u, 12);
                    }
                    if (rec && rec + 160u <= PS2_RAM_SIZE)
                        std::memcpy(rf, rdram + rec + 80u + 0x10u, 48);
                    const uint32_t fu = g_mprobeFast[model]; g_mprobeFast[model] = 0u;
                    std::fprintf(stderr, "[mprobe2] model 0x%X count %u fastUpd %u loc1 %.2f %.2f %.2f loc2 %.2f %.2f %.2f loc10 %.2f %.2f %.2f rec1 %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f%c",
                                 model, cnt, fu, lt[0], lt[1], lt[2], lt[3], lt[4], lt[5], lt[6], lt[7], lt[8],
                                 rf[0], rf[1], rf[2], rf[3], rf[4], rf[5], rf[6], rf[7], rf[8], rf[9], rf[10], rf[11], 10);
                }
            }
        }
        if (sd == 0u || sd > PS2_RAM_SIZE - 0x28u)
        {
            g_origDrawCharBlood(rdram, ctx, runtime);
            return;
        }
        auto rd = [&](uint32_t a) { uint32_t v = 0u; if (a <= PS2_RAM_SIZE - 4u) std::memcpy(&v, rdram + a, 4); return v; };
        const uint32_t nMesh = rd(sd + 0x10u), meshes = rd(sd + 0x1Cu) & PS2_RAM_MASK;
        const uint32_t key = sd ^ (nMesh << 24);
        auto it = g_bloodMats.find(key);
        if (it == g_bloodMats.end())
        {
            std::vector<uint32_t> mats;
            const uint32_t nTex = rd(sd + 0x18u), names = rd(sd + 0x24u) & PS2_RAM_MASK;
            std::vector<uint32_t> bloodTex, bandTex, bandMats;
            if (nTex < 256u && names != 0u && names <= PS2_RAM_SIZE - nTex * 16u)
                for (uint32_t t = 0; t < nTex; ++t)
                {
                    char nm[17] = {};
                    std::memcpy(nm, rdram + names + t * 16u, 16);
                    if (std::strcmp(nm, "blood") == 0 || std::strcmp(nm, "blood_b") == 0)
                        bloodTex.push_back(t);
                    else if (std::strcmp(nm, "bandage") == 0) // Nov 7: SvR06 injury overlay (Hogan 8700), HCTP has no
                        bandTex.push_back(t);                 // bandage state -> never drawn

                }
            if ((!bloodTex.empty() || !bandTex.empty()) && nMesh < 512u && meshes != 0u)
                for (uint32_t m = 0; m < nMesh; ++m)
                {
                    const uint32_t mesh = meshes + m * 64u, nMat = rd(mesh + 4u), mats0 = rd(mesh + 0xCu) & PS2_RAM_MASK;
                    for (uint32_t k = 0; k < nMat && k < 256u && mats0 != 0u; ++k)
                    {
                        const uint32_t mat = mats0 + k * 0xD0u;
                        if (std::find(bloodTex.begin(), bloodTex.end(), rd(mat + 0x28u)) != bloodTex.end())
                            mats.push_back(mat);
                        else if (std::find(bandTex.begin(), bandTex.end(), rd(mat + 0x28u)) != bandTex.end())
                            mats.push_back(mat), bandMats.push_back(mat);
                    }
                }
            if (!bloodTex.empty())
            std::fprintf(stderr, "[svrblood] ymp 0x%X: %zu blood tex, %zu material(s) hidden until bleeding%c", sd,
                         bloodTex.size(), mats.size(), 10);
            if (g_bloodMats.size() > 4096u)
                g_bloodMats.clear();
            for (const uint32_t mt : mats) // fresh model data: remember the real REF qwc (Nov 7 flicker fix)
            {
                uint16_t q = 0u;
                std::memcpy(&q, rdram + mt + 0xC2u, 2);
                g_bloodQwc[mt] = q;
            }
            for (const uint32_t mt : bandMats)
                g_bloodQwc[mt] = 0u; // bandage: hidden even while bleeding
            if (!bandMats.empty())
                std::fprintf(stderr, "[svrblood] ymp 0x%X: %zu bandage material(s) hidden%c", sd, bandMats.size(), 10);
            it = g_bloodMats.emplace(key, std::move(mats)).first;
        }
        if (it->second.empty())
        {
            g_origDrawCharBlood(rdram, ctx, runtime);
            return;
        }
        // Nov 7 FLICKER FIX: the GS thread reads the DMA chain after this call returns, so a qwc restored right after
        // the call showed the blood on some frames. The qwc now STAYS 0 while not bleeding and the real value is
        // written back only for a bleeding char's draw (persistent state, no restore after the call).
        for (const uint32_t mt : it->second)
        {
            const auto q = g_bloodQwc.find(mt);
            const uint16_t v = (g_drawCharBleed > 0 && q != g_bloodQwc.end()) ? q->second : uint16_t(0u);
            std::memcpy(rdram + mt + 0xC2u, &v, 2);
        }
        g_origDrawCharBlood(rdram, ctx, runtime);
    }
    // Native-model bridge probe: capture only a bounded identity record for a live
    // CSkinModel.  This stays observational; the original VIF/VU/GS draw remains
    // authoritative until a fingerprinted model has a verified native replacement.
    PS2Runtime::RecompiledFunction g_origSkinModelDraw = nullptr;
    uint32_t nativeModelFingerprint(const uint8_t *rdram, uint32_t address, uint32_t bytes)
    {
        if (!rdram || address > PS2_RAM_SIZE || bytes > PS2_RAM_SIZE - address)
            return 0u;
        uint32_t hash = 2166136261u; // FNV-1a: identity only, never a content validator.
        for (uint32_t i = 0; i < bytes; ++i)
            hash = (hash ^ rdram[address + i]) * 16777619u;
        return hash;
    }
    void skinModelDrawDiagnostics(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & PS2_RAM_MASK;
        if (model <= PS2_RAM_SIZE - 0xccu)
        {
            uint32_t skinData = 0u;
            std::memcpy(&skinData, rdram + model + 0xc8u, sizeof(skinData));
            skinData &= PS2_RAM_MASK;
            if (skinData != 0u && skinData <= PS2_RAM_SIZE - 0x40u)
            {
                static std::array<uint32_t, 32> seen = {};
                static size_t seenCount = 0u;
                const auto end = seen.begin() + seenCount;
                if (std::find(seen.begin(), end, skinData) == end && seenCount < seen.size())
                {
                    seen[seenCount++] = skinData;
                    uint32_t boneCount = 0u, meta18 = 0u, meta1c = 0u, boneNames = 0u;
                    std::memcpy(&boneCount, rdram + skinData + 0x14u, sizeof(boneCount));
                    std::memcpy(&meta18, rdram + skinData + 0x18u, sizeof(meta18));
                    std::memcpy(&meta1c, rdram + skinData + 0x1cu, sizeof(meta1c));
                    std::memcpy(&boneNames, rdram + skinData + 0x20u, sizeof(boneNames));
                    std::fprintf(stderr,
                                 "[native-skin] model=0x%08x ymp=0x%08x bones=%u meta18=0x%08x "
                                 "meta1c=0x%08x names=0x%08x fp40=0x%08x%c",
                                 model, skinData, boneCount, meta18, meta1c, boneNames,
                                 nativeModelFingerprint(rdram, skinData, 0x40u), 10);
                }
            }
        }
        g_origSkinModelDraw(rdram, ctx, runtime);
    }
    // Select screen: MenuWrestler::Draw 0x243540 (this+0x68 = its CChar) / thunk 0x244620 (this-8: +0x60) call DrawChar
    // themselves while the model fades in/out (fully shown it goes through CChar::Draw above).
    PS2Runtime::RecompiledFunction g_origMenuWrDraw[2] = {nullptr, nullptr};
    template <int I>
    void menuWrestlerDrawReducedGloss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float saved, dull = 32.0f;
        std::memcpy(&saved, rdram + 0x40F33Cu, 4);
        std::memcpy(rdram + 0x40F33Cu, &dull, 4);
        g_origMenuWrDraw[I](rdram, ctx, runtime);
        std::memcpy(rdram + 0x40F33Cu, &saved, 4);
    }
    // Rope shadows: CRope::Draw 0x2185b0 ends with DrawRopeShadow 0x217c50 (subtract blend, no Z test, drawn through
    // the crowd program p17). Here they come out as long white streaks across the screen (Jordan, Oct 20) - skipped
    // debug switch only: HCTP_NO_ROPE_SHADOW=1 skips them (to tell them apart from other streaks).
    void skipRopeShadow(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }
    // TEMP diag (rope-shadow width): DrawRopeShadow 0x217c50 (a0 = CRope*) reads per rope (3) per side (4) the
    // segment offsets at rope + 0x5a4 / 0x5a8 + seg*0x40 + rope*0x340 + side*0x9c0 for [rope+0x40]+1 segments.
    // HCTP_ROPELOG=1: print them every 300 calls.
    PS2Runtime::RecompiledFunction g_origDrawRopeShadow = nullptr;
    void drawRopeShadowLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_n = 0;
        if (s_n++ % 300u == 0u)
        {
            const uint32_t rope = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
            int32_t segs;
            std::memcpy(&segs, rdram + rope + 0x40u, 4);
            std::fprintf(stderr, "[ropelog] rope=0x%x segs=%d%c", rope, segs, 10);
            for (int side = 0; side < 4; ++side)
                for (int r = 0; r < 3; ++r)
                {
                    std::fprintf(stderr, "[ropelog] side %d rope %d:", side, r);
                    for (int k = 0; k <= segs && k < 16; ++k)
                    {
                        float a, b;
                        const uint32_t at = rope + static_cast<uint32_t>(k * 0x40 + r * 0x340 + side * 0x9c0);
                        std::memcpy(&a, rdram + ((at + 0x5a4u) & 0x1FFFFFFu), 4);
                        std::memcpy(&b, rdram + ((at + 0x5a8u) & 0x1FFFFFFu), 4);
                        std::fprintf(stderr, " (%.2f,%.2f)", a, b);
                    }
                    std::fprintf(stderr, "%c", 10);
                }
        }
        g_origDrawRopeShadow(rdram, ctx, runtime);
    }
    // No interlacing (PCSX2's HCTP "No-Interlacing" patch nops the odd-field branch in both): sceGsSetHalfOffset
    // 0x102660 / sceGsSetHalfOffset2 0x1028c8 (a0 = draw env, a1 = x, a2 = y, a3 = odd field) write XYOFFSET at
    // env+0x20 with +8 (half a line) on odd fields - a bounce when every field is shown as a frame. Always the even
    // field here. HCTP_INTERLACE_OFFSET=1 keeps the game's offset.
    void gsSetHalfOffsetEven(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t env = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        const int64_t x = static_cast<int16_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const int64_t y = static_cast<int16_t>(_mm_cvtsi128_si32(ctx->r[6]));
        uint64_t display;
        std::memcpy(&display, rdram + env + 0x30u, 8);
        const int64_t ox = x - static_cast<int64_t>((((display >> 16) & 0x7FFu) + 1u) >> 1);
        const int64_t oy = y - static_cast<int64_t>((((display >> 48) & 0x7FFu) + 1u) >> 1);
        const uint64_t xyoffset = static_cast<uint64_t>(ox * 16) | (static_cast<uint64_t>(oy) << 36);
        std::memcpy(rdram + env + 0x20u, &xyoffset, 8);
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }
    // CSd5EventData::GetEntranceCastNo 0x2af480 (a1 = cast code id*100 + attire*10) hard-codes the wrestlers who wear an
    // entrance attire (part 4): 0x3A->5804, 0x45->6904, 0x48->7204, 0x4B->7504, 0x4C->7604, 0x50->8004, 0x27->3904;
    // everyone else gets the ring (or high-poly) model. Restored wrestlers with an XXA4 pack get their entrance attire.
    uint16_t g_hoganTearBody = 0; // Hogan after-tear body override (see initEventSystemShirtTear)
    PS2Runtime::RecompiledFunction g_origGetEntranceCastNo = nullptr;
    void getEntranceCastNoCutRoster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t code = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & 0xFFFFu;
        g_origGetEntranceCastNo(rdram, ctx, runtime);
        const uint32_t entrance = code - code % 10u + 4u;
        if (code < 10000u && g_cutEntranceCodes.count(static_cast<uint16_t>(entrance)))
            SET_GPR_U32(ctx, 2, entrance);
    }
    // Molly Holly SvR port (Nov 5): GameMem::Setup 0x1feed0 sizes the 7 character pages (KB, table 0x419d68[slot],
    // cast code at 0x4920C0 + slot*32 + 0x10A): part 0 (ring) = 0x1E0, other parts 0x290, B&P mode part 3 = 0x350, then
    // ymemReallocatePage(2, 0x419d60). SvR Molly (3754 verts) needs ~535 KB without textures -> give her ring attires
    // (41x0) the part-1/2 size 0x290 when the loose SvR model (> 512 KB) is installed in MODS/PAC/EMD2.
    PS2Runtime::RecompiledFunction g_origReallocatePage = nullptr;
    void reallocatePageBigModels(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Nov 5 (Hogan session): generalised from SvR Molly (41x0) to every loose ring model > 512 KB (Hogan WM19 graft
        // 3210 = 524800 B loaded nothing -> invisible body, only the separate gear pack drew). Sizes cached per code.
        static std::map<uint16_t, uint64_t> s_looseSize;
        g_hoganTearBody = 0; // match memory setup: entrance over, ring models load by their own codes again
        auto looseBig = [](uint16_t code) {
            auto it = s_looseSize.find(code);
            if (it == s_looseSize.end())
            {
                std::error_code ec;
                const std::string key = std::to_string(code);
                uint64_t sz = std::filesystem::file_size((code < 3000u ? "MODS/PAC/EMD/" : "MODS/PAC/EMD2/") + key, ec);
                it = s_looseSize.emplace(code, ec ? 0u : sz).first;
            }
            return it->second;
        };
        auto looseSize = looseBig;
        auto looseBigFn = [&looseSize](uint16_t code) { return looseSize(code) > 0x80000u; };
        const uint32_t first = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        const uint32_t table = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        if (first == 2u && table == 0x419d60u)
        {
            for (uint32_t slot = 0; slot < 7u; ++slot)
            {
                uint16_t code = 0;
                std::memcpy(&code, rdram + 0x4920C0u + slot * 32u + 0x10Au, 2);
                uint32_t size = 0;
                std::memcpy(&size, rdram + table + 8u + slot * 4u, 4);
                if (code < 10000u && code % 10u == 0u && size == 0x1E0u && looseBigFn(code))
                {
                    // Nov 7 Legend arms/turn lock: SvR06 beta 7800 (642 KB, 109 bones) in a 0x290 page = stuck
                    // pose + no turning; in A4 the same file got 0x350 via its 794 KB entrance attire and was fine.
                    size = looseSize(code) > 0x258u * 1024u ? 0x350u : 0x290u;
                    std::memcpy(rdram + table + 8u + slot * 4u, &size, 4);
                    std::fprintf(stderr, "[mem] slot %u cast %u big loose model 0x1E0 -> 0x%X%c", slot, unsigned(code), size, 10);
                }
                // Nov 7: loose entrance attires (XXa4, e.g. SvR06 Hogan 80s 7204 = 776 KB decoded) larger than the
                // 0x290 page get the B&P part-3 size 0x350 (848 KB), whether the page is keyed by the entrance code
                // or by the ring code whose entrance attire is armed.
                if (code < 10000u && (code % 10u == 4u || code % 10u == 0u) && size < 0x350u)
                {
                    const uint16_t ent = static_cast<uint16_t>(code - code % 10u + 4u);
                    // Nov 7 Legend: SvR06 7804 (651 KB) fits 0x290 but not a 0x1E0 page -> pick the smallest that fits.
                    const uint32_t need = !g_cutEntranceCodes.count(ent) ? 0u
                                          : looseSize(ent) > 0x290u * 1024u ? 0x350u
                                          : looseSize(ent) > 0x1E0u * 1024u ? 0x290u : 0u;
                    if (need > size)
                    {
                        std::fprintf(stderr, "[mem] slot %u cast %u entrance attire %u 0x%X -> 0x%X%c", slot,
                                     unsigned(code), unsigned(ent), size, need, 10);
                        size = need;
                        std::memcpy(rdram + table + 8u + slot * 4u, &size, 4);
                    }
                }
            }
        }
        g_origReallocatePage(rdram, ctx, runtime);
    }
    // Hair chains (Nov 6): CKami (hair physics, 0x1F0 B, ctor 0x1c5980) keeps its CKami1 chain pointers at +0x1C,
    // null-terminated, and +0x58 is its "reinit" flag -> room for 14 chains. The ctor loop calls CKami1::Make
    // 0x1c5f50(char, idx, kami) until it returns 0, with no bound: models with more hair chains (Nero 3X Warrior,
    // 534 KB) overwrite +0x58.. +0x80 (char pointer) -> CKami::ActionLink ran CKami1 on garbage (crash in
    // MatchWrestler::MakeChar -> CKami1::Init writing 0x0F800000). Nero 3X grows the object to 0x270 and moves the
    // array to +0x1F0 (ELF patch). Ours: chains 14+ live in a host list per CKami; CKami::ActionLink 0x1c5d70 runs
    // them after the stock 14 (Init 0x1c6160 when the stock loop did, then CKami1::ActionLink 0x1c6170); dtor
    // 0x1c5cc0 deletes them. HCTP_KAMI_EXTRA=0 = stock (crashes on such models).
    constexpr uint32_t kKamiStockChains = 14u;
    PS2Runtime::RecompiledFunction g_origKamiMake = nullptr, g_origKamiAction = nullptr, g_origKamiDtor = nullptr;
    std::map<uint32_t, std::vector<uint32_t>> g_kamiExtra;
    bool g_kamiInitSeen = false;
    uint32_t kamiCallFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction fn,
                        uint32_t addr, std::initializer_list<uint32_t> args)
    {
        const bool savedHit = t_eeCheckpointHit, savedNo = t_eeNoCheckpoint;
        t_eeNoCheckpoint = true; // run to the end: host code cannot resume a paused callee
        const R5900Context saved = *ctx;
        int r = 4;
        for (uint32_t a : args)
            ctx->r[r++] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a)));
        ctx->pc = addr;
        fn(rdram, ctx, runtime);
        const uint32_t ret = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2]));
        *ctx = saved;
        t_eeCheckpointHit = savedHit;
        t_eeNoCheckpoint = savedNo;
        return ret;
    }
    // TEMP diag HCTP_MATCHUP_PROBE (recwatch, Nov 7 Hogan 80s matchup): bone record 1 (+0x10..+0x2C) of foreign
    // models; logs the first hook point that sees it all zero after it was valid.
    std::map<uint32_t, std::pair<uint32_t, bool>> g_recWatch; // rec1 addr -> (model, valid)
    void recWatchCheck(const uint8_t *rdram, const char *tag, uint32_t ra)
    {
        static const bool on = std::getenv("HCTP_MATCHUP_PROBE") != nullptr;
        if (!on) return;
        for (auto &w : g_recWatch)
        {
            if (w.first > PS2_RAM_SIZE - 0x30u) continue;
            uint32_t v[7]; std::memcpy(v, rdram + w.first + 0x10u, 28);
            const bool ok = (v[0] | v[1] | v[2] | v[4] | v[5] | v[6]) & 0x7FFFFFFFu;
            if (w.second.second && !ok)
                std::fprintf(stderr, "[recwatch] model 0x%X rec1 0x%X ZEROED, first seen at %s ra 0x%X%c", w.second.first, w.first, tag, ra, 10);
            else if (!w.second.second && ok)
                std::fprintf(stderr, "[recwatch] model 0x%X rec1 0x%X valid again at %s ra 0x%X%c", w.second.first, w.first, tag, ra, 10);
            w.second.second = ok;
        }
    }
    // TEMP diag HCTP_MATCHUP_PROBE: EditYMP::TransformAll 0x2d49a0 (this, CSkinModel*, morph) and ScaleBoneLength
    // 0x2d5e60 (this, CSkinModel*, scales, boneNoTbl): model, bone rec1 pos before/after, first scale vectors.
    PS2Runtime::RecompiledFunction g_origEditYmpFn[2] = {nullptr, nullptr};
    template <int I>
    void editYmpProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const char *kName[2] = {"TransformAll", "ScaleBoneLength"};
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & PS2_RAM_MASK;
        const uint32_t a2 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6])) & PS2_RAM_MASK;
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        uint32_t recs = 0u, cnt = 0u;
        if (model && model <= PS2_RAM_SIZE - 0xF4u) { std::memcpy(&recs, rdram + model + 0xF0u, 4); std::memcpy(&cnt, rdram + model + 0xECu, 4); }
        recs &= PS2_RAM_MASK;
        float b[3] = {}, af[3] = {}, sc[8] = {};
        const bool okR = recs && recs <= PS2_RAM_SIZE - 0x100u;
        if (okR) std::memcpy(b, rdram + recs + 80u + 0x10u, 12);
        if (I == 1 && a2 && a2 <= PS2_RAM_SIZE - 32u) std::memcpy(sc, rdram + a2, 32);
        g_origEditYmpFn[I](rdram, ctx, runtime);
        if (okR) std::memcpy(af, rdram + recs + 80u + 0x10u, 12);
        std::fprintf(stderr, "[editymp] %s model 0x%X bones %u ra 0x%X done %d rec1 %g %g %g -> %g %g %g scales0-1 %g %g %g %g | %g %g %g %g%c",
                     kName[I], model, cnt, ra, ctx->pc == ra ? 1 : 0, b[0], b[1], b[2], af[0], af[1], af[2],
                     sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], sc[6], sc[7], 10);
    }
    void kamiMakeExtra(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        recWatchCheck(rdram, "CKami1::Make", static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])));
        const uint32_t chr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        const uint32_t idx = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const uint32_t kami = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
        if (idx == 0u)
            g_kamiExtra.erase(kami); // fresh CKami at a recycled address
        if (idx != kKamiStockChains)
        {
            g_origKamiMake(rdram, ctx, runtime);
            return;
        }
        // Stock slot 14 = the terminator: build every further chain into the host list; the stock array ends here.
        std::vector<uint32_t> extra;
        for (uint32_t i = kKamiStockChains; i < 100u; ++i)
        {
            const uint32_t c = kamiCallFn(rdram, ctx, runtime, g_origKamiMake, 0x1c5f50u, {chr, i, kami});
            if (!c)
                break;
            extra.push_back(c);
        }
        if (!extra.empty())
        {
            std::fprintf(stderr, "[kami] CKami 0x%x: %u hair chains (stock room 14), %u kept on host%c", kami,
                         unsigned(kKamiStockChains + extra.size()), unsigned(extra.size()), 10);
            g_kamiExtra[kami] = std::move(extra);
        }
        ctx->r[2] = _mm_setzero_si128();
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }
    void kamiInitSeen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        // CKami1::Init 0x1c6160, stock body: this+4 = 1
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        const uint32_t one = 1u;
        std::memcpy(rdram + ((self + 4u) & 0x1FFFFFFu), &one, 4);
        g_kamiInitSeen = true;
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }
    void kamiActionExtra(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        recWatchCheck(rdram, "CKami::ActionLink", static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])));
        const uint32_t kami = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        auto it = g_kamiExtra.find(kami);
        if (it == g_kamiExtra.end())
        {
            g_origKamiAction(rdram, ctx, runtime);
            return;
        }
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        g_kamiInitSeen = false;
        kamiCallFn(rdram, ctx, runtime, g_origKamiAction, 0x1c5d70u, {kami});
        for (uint32_t c : it->second)
        {
            if (g_kamiInitSeen)
            {
                const uint32_t one = 1u;
                std::memcpy(rdram + ((c + 4u) & 0x1FFFFFFu), &one, 4);
            }
            kamiCallFn(rdram, ctx, runtime, runtime->lookupFunction(0x1c6170u), 0x1c6170u, {c});
        }
        ctx->pc = ra;
    }
    void kamiDtorExtra(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t kami = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        auto it = g_kamiExtra.find(kami);
        if (it != g_kamiExtra.end())
        {
            const std::vector<uint32_t> extra = std::move(it->second);
            g_kamiExtra.erase(it);
            for (uint32_t c : extra) // as the stock loop: operator delete
                kamiCallFn(rdram, ctx, runtime, runtime->lookupFunction(0x100200u), 0x100200u, {c});
        }
        g_origKamiDtor(rdram, ctx, runtime);
    }
    // Hogan's entrance shirt tear: InitEventSystem normally switches to the XXX2
    // after-tear dress only when the event member flag at +0x155 is set. Modern
    // Hogan's 32xx cast entries do not set it, so the entrance gear remains on.
    // Enable the switch only when the matching loose after-tear pack exists.
    // Nov 6 (Jordan): Hogan's entrance (event 3032, Legend 3078 bank) swaps in a SEVP after-tear body object with the
    // literal model 3200 (attire 1). For other Hogan attires that showed the attire-1 body at the tear. While such an
    // entrance runs, /EMD2/3200 is served as that attire's ring body (32A0); cleared when GameMem sets up the match.
    PS2Runtime::RecompiledFunction g_origInitEventSystem = nullptr;
    void initEventSystemShirtTear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        const uint32_t event = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        // Ghidra: InitEventSystem selects a member through this+0x728[event], not member[event].
        // The old first-eight scan missed Hogan's selected member and never armed SetDressCast.
        if (self && event < 16u && self + 0x728u + event * 4u < 0x2000000u)
        {
            uint32_t selected = 0;
            std::memcpy(&selected, rdram + self + 0x728u + event * 4u, 4);
            const uint32_t member = self + 0x88u + selected * 0xD4u;
            if (selected < 32u && member + 0xCEu < 0x2000000u)
            {
                uint16_t code = 0;
                std::memcpy(&code, rdram + member, 2);
                std::fprintf(stderr, "[entrance] InitEventSystem event %u member %u cast %u dress flag %u%c", event,
                             selected, unsigned(code), unsigned(rdram[member + (0x155u - 0x88u)]), 10);
                if (code < 10000u && code / 100u == 32u && (code % 100u) / 10u != 0u)
                {
                    g_hoganTearBody = static_cast<uint16_t>(code - code % 10u);
                    std::fprintf(stderr, "[entrance] Hogan tear body 3200 -> %u%c", unsigned(g_hoganTearBody), 10);
                }
                // Nov 7 (Jordan): a Hogan with its own after-tear model (XX?5, e.g. Hogan 80s 8705 = SvR06 retail 7201,
                // bandana kept) gets it at the tear; the match then loads its ring model (beta 8700) as usual.
                else if (code < 10000u)
                {
                    uint16_t tear = static_cast<uint16_t>(code - code % 10u + 5u);
                    // Nov 7 (Jordan): Legend = 2 models only; the tear reuses the XX?4 entrance body (retail 7201)
                    // when that is a registered full entrance model, so no separate XX?5 copy is needed.
                    {
                        char dress[32];
                        std::snprintf(dress, sizeof(dress), "MODS/PAC/EMD2/%04u", unsigned(tear - 3u));
                        std::error_code ecd;
                        if (g_cutEntranceCodes.count(static_cast<uint16_t>(tear - 1u)) &&
                            std::filesystem::exists(dress, ecd)) // shirt-tear wrestlers only (XX?2 gear present)
                            tear = static_cast<uint16_t>(tear - 1u);
                    }
                    char key[8];
                    std::snprintf(key, sizeof(key), "%04u", unsigned(tear));
                    std::error_code ec;
                    if (std::filesystem::exists(std::string("MODS/PAC/EMD2/") + key, ec))
                    {
                        g_hoganTearBody = tear;
                        std::fprintf(stderr, "[entrance] after-tear model 3200 -> %u%c", unsigned(tear), 10);
                    }
                }
                if (false && code / 100u == 0x20u && code < 10000u) // superseded by enableHoganDress (IsDress table)
                {
                    char key[8];
                    std::snprintf(key, sizeof(key), "%04u", code - code % 10u + 2u);
                    std::error_code ec;
                    if (std::filesystem::exists(std::string("MODS/PAC/EMD2/") + key, ec))
                    {
                        rdram[member + (0x155u - 0x88u)] = 1;
                        std::fprintf(stderr, "[entrance] shirt tear armed: event %u member %u cast %u dress %s%c",
                                     event, selected, unsigned(code), key, 10);
                    }
                }
            }
        }
        g_origInitEventSystem(rdram, ctx, runtime);
    }
    void restoreCutRoster(uint8_t *rdram)
    {
        // Nov 6 Rosey (0x21): the roster record says "Rosie" at +0x116 (name: select lists, CAM, results, title holders,
        // stables - GetWrestlerName 0x12cde0) and +0x144 (second copy, ring name). Table 0x4C0F54 comes from the
        // memory-card save (keeps the old spelling), so the loose MODS\PAC\CHAR\ALL fix alone never showed. Patch the
        // live record; the next save writes "Rosey" back. HCTP_NO_ROSEY_FIX=1 = off.
        static const bool roseyFix = std::getenv("HCTP_NO_ROSEY_FIX") == nullptr;
        if (roseyFix)
            for (const uint32_t off : {0x116u, 0x144u})
            {
                uint8_t *const nm = rdram + 0x4C0F54u + 0x21u * 0x170u + off;
                if (std::memcmp(nm, "Rosie", 6) == 0)
                    std::memcpy(nm, "Rosey", 6);
            }
        // Nov 10 (Jordan): stock-slot stat overrides from MODS\ROSTER\STOCK_STATS.txt, lines "<hex id> str sub spd tec end"
        // (record +0..+4, 0-20 = 0-10 on screen; '#' = comment). Records come from the memory-card save, so the values
        // are re-applied on every load (in-game stat edits of these ids do not stick). Used for SvR legend stats.
        static const std::vector<std::pair<uint32_t, std::array<uint8_t, 5>>> stockStats = [] {
            std::vector<std::pair<uint32_t, std::array<uint8_t, 5>>> v;
            std::ifstream f("MODS/ROSTER/STOCK_STATS.txt");
            for (std::string line; std::getline(f, line);)
            {
                unsigned id, a[5];
                if (!line.empty() && line[0] != '#' &&
                    std::sscanf(line.c_str(), "%x %u %u %u %u %u", &id, &a[0], &a[1], &a[2], &a[3], &a[4]) == 6 && id < 0x56u)
                {
                    std::array<uint8_t, 5> b{};
                    for (int i = 0; i < 5; ++i)
                        b[i] = uint8_t(a[i] > 20u ? 20u : a[i]);
                    v.emplace_back(id, b);
                }
            }
            std::fprintf(stderr, "[rosterx] STOCK_STATS.txt: %zu stock stat rows%c", v.size(), 10);
            return v;
        }();
        for (const auto &[id, st] : stockStats)
            std::memcpy(rdram + 0x4C0F54u + id * 0x170u, st.data(), 5);
        for (size_t i = 0; i < kCutCount; ++i)
        {
            const CutWrestler &w = kCutRoster[i];
            uint8_t *const rec = rdram + 0x4C0F54u + w.id * 0x170u;
            if (!g_cutRosterHasModel[i] || rec[0x163] != 6)
                continue;
            rec[0x163] = w.type;
            if (w.entrance != 0)
                std::memcpy(rec + 0xEC, &w.entrance, 2);
            static constexpr uint8_t kZero[5] = {};
            if (std::memcmp(rec, kZero, 5) == 0)
                std::memcpy(rec, w.stats, 5);
        }
    }
    PS2Runtime::RecompiledFunction g_origGetCharaGameProp = nullptr;
    // CSd5EventData::IsDress 0x2af300 returns byte 0x41B5C0[id * 6 + attire]: 1 = the attire has an entrance dress
    // (XXX2 pack) that SetSuperStarData arms (+0x155) and the shirt tear removes. Stock sets it for Legend Hogan
    // (78) only; give Modern Hogan's attire 0 the same flag when its dress pack (EMD2\3202) is installed.
    void enableHoganDress(uint8_t *rdram)
    {
        // Nov 5: every Modern Hogan attire A whose dress pack EMD2A2 is installed gets the flag (was attire 0 only),
        // so each attire walks out in its own gear and tears it (WM19 = attire 1 -> 3212).
        static const uint8_t dressMask = [] {
            uint8_t m = 0;
            for (unsigned a = 0; a < 6; ++a)
            {
                char key[32];
                std::snprintf(key, sizeof(key), "MODS/PAC/EMD2/32%u2", a);
                if (std::filesystem::exists(key))
                    m |= uint8_t(1u << a);
            }
            return m;
        }();
        for (unsigned a = 0; a < 6; ++a)
            if (dressMask & (1u << a))
                rdram[0x41B5C0u + 0x20u * 6u + a] = 1;
        // Nov 5: Jerry Lawler (53): ring attire 0 = EMD2\5300 (5330 body + retail head, no crown), entrance dress =
        // EMD2\5302 (stock 5300 jacket + crown as a gear model, like HBK 4502). Flag only when the gear pack exists.
        static const bool lawlerDress = std::filesystem::exists("MODS/PAC/EMD2/5302");
        if (lawlerDress)
            rdram[0x41B5C0u + 53u * 6u + 0u] = 1;
        // Legend Hogan (0x4E) is a restored cut slot: ch2 has his body 7800 but no dress 7802, so his stock flag made
        // the entrance ask for a missing pack -> CFile::MemAllocCashPage spun forever in CachRemoveHead (hang).
        static const bool legendDress = std::filesystem::exists("MODS/PAC/EMD2/7802");
        if (!legendDress)
        {
            rdram[0x41B5C0u + 0x4Eu * 6u + 0u] = 0;
            // nvCast's backstage attire table 0x41A8D0 (s8 per id < 0x56) has 100 for Legend Hogan: >99 sets the cast's
            // dress flag (+0xC9) -> same missing 7802 pack in Season cutscenes. Use plain attire 0 instead.
            rdram[0x41A8D0u + 0x4Eu] = 0;
        }
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            uint16_t modern = 0, legend = 0;
            std::memcpy(&modern, rdram + 0x4C0F54u + 0x20u * 0x170u + 0xECu, 2);
            std::memcpy(&legend, rdram + 0x4C0F54u + 0x4Eu * 0x170u + 0xECu, 2);
            std::fprintf(stderr, "[entrance] event no: modern Hogan %u, legend Hogan %u%c", modern, legend, 10);
        }
    }
    void getCharaGamePropCutRoster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        enableHoganDress(rdram);
        restoreCutRoster(rdram);
        g_origGetCharaGameProp(rdram, ctx, runtime);
    }
    // Season's IsSeasonEntry reads the character type directly from the global
    // property table, so it can run before GetCharaGameProp has restored the NPCs.
    PS2Runtime::RecompiledFunction g_origIsSeasonEntry = nullptr;
    void isSeasonEntryCutRoster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t id = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0xFFu;
        restoreCutRoster(rdram);
        g_origIsSeasonEntry(rdram, ctx, runtime);
        // Nov 5: restored wrestlers are Season-playable again (Jordan). Season cutscene casts ask GetMatchWrestlerNo for the
        // backstage part XX01 and fall back to the ring model; restored slots now get their entrance attire there
        // (getMatchWrestlerNoCutRoster). The announcer/NPC slots stay out of Season until Jordan decides (Lawler reached
        // the match loader and hung on a black screen, Oct 22).
        if (id >= 50 && id <= 55)
            SET_GPR_U32(ctx, 2, 0u);
    }
    // GetMatchWrestlerNo 0x12cce0 (a0 id, a1 attire, a2 backstage wanted, a3 entrance, t0 special, t1 CAW): returns model
    // rec+0x114 * 100 + attire*10, +1 when a2 is set and that backstage pack exists (MakeCharName + CFile::ExistenceCheck).
    // Season casts (nvCast 0x258a30) set a2. Restored wrestlers have no XX01 backstage pack: use their entrance attire
    // (XXn4, Jordan Nov 5), or keep the ring attire when they have none (Jeff).
    PS2Runtime::RecompiledFunction g_origGetMatchWrestlerNo = nullptr;
    void getMatchWrestlerNoCutRoster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t id = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        const bool backstage = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6])) != 0u &&
                               static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[7])) == 0u &&
                               static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[8])) == 0u;
        g_origGetMatchWrestlerNo(rdram, ctx, runtime);
        const uint32_t code = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2])) & 0xFFFFu;
        if (!backstage || id >= 0x56u || code >= 10000u || code % 10u != 0u)
            return;
        bool restored = false;
        for (size_t i = 0; i < kCutCount; ++i)
            restored = restored || (g_cutRosterHasModel[i] && kCutRoster[i].id == id);
        // Nov 7 (Jordan): never for wrestlers with an entrance shirt pack XX?2 (Legend Hogan 7802): this check also
        // fired in exhibition and made the match load the entrance body 7804 instead of ring 7800.
        char dressKey[32];
        std::snprintf(dressKey, sizeof(dressKey), "MODS/PAC/EMD2/%04u", unsigned(code + 2u));
        std::error_code dressEc;
        if (restored && g_cutEntranceCodes.count(static_cast<uint16_t>(code + 4u)) &&
            !std::filesystem::exists(dressKey, dressEc))
        {
            SET_GPR_U32(ctx, 2, code + 4u);
            static int logged = 0;
            if (logged++ < 16)
                std::fprintf(stderr, "[season] backstage cast %u -> entrance attire %u%c", code, code + 4u, 10);
        }
    }
    // CSeasonCharaFrame ctor 0x316ba0 (a0 this): builds its pickable roster (u16 list at +0x88, count +0x1EC) from
    // GetCharaGameProp types, which restoreCutRoster made valid for the restored slots. Season has no story data for
    // them -> drop them from the list after the stock ctor returns.
    PS2Runtime::RecompiledFunction g_origSeasonCharaFrameCtor = nullptr;
    void seasonCharaFrameCtorCutRoster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])), ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        g_origSeasonCharaFrameCtor(rdram, ctx, runtime);
        if (ctx->pc != ra)
        {
            std::fprintf(stderr, "[season] chara frame ctor paused, list not filtered%c", 10);
            return;
        }
        uint32_t count = 0;
        std::memcpy(&count, rdram + self + 0x1ECu, 4);
        if (count > (0x1ECu - 0x88u) / 2u)
            return;
        uint32_t out = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint16_t id = 0;
            std::memcpy(&id, rdram + self + 0x88u + i * 2u, 2);
            const bool cut = id >= 50 && id <= 55; // Nov 5: only the announcer/NPC slots stay out of Season
            if (!cut)
                std::memcpy(rdram + self + 0x88u + (out++) * 2u, &id, 2);
        }
        for (uint32_t i = out; i < count; ++i)
        {
            const uint16_t none = 0x76;
            std::memcpy(rdram + self + 0x88u + i * 2u, &none, 2);
        }
        std::memcpy(rdram + self + 0x1ECu, &out, 4);
        std::fprintf(stderr, "[season] chara list %u -> %u (NPC slots removed)%c", count, out, 10);
    }

    // Season roster entry: "only one of these wrestlers" rule (Jordan Nov 5). Stock CRosterFrame hard-codes one pair:
    // CheckRosterSpecialCondition 0x34ffe0 (leaf; this+0x458 = cursor index, +0x362 u16[0x76] roster ids, +0x2EC s8[0x76]
    // entry flags: <0 unavailable, 0 out, 1 in, 2 locked) returns 2 when Undertaker 0x03 / Legend Undertaker 0x48 is picked
    // while the other is in; CheckEntryOther 0x350100 then undoes the toggle and InputAction shows SetWarning(6) =
    // WarningStart(kind 5, msg 29) = GetWarningMsg 0x168e40 table 0x410580[5] entry 29 -> {char *lines[5], count 5}:
    // "@C", "You can't enter Undertaker and", "The Undertaker at the same time.", "Please select one.", "".
    // SetRosterSpecialCondition 0x34fd10 (ctor) drops one Taker when a saved roster has both.
    // Here: data-driven groups (first = stock Taker, same text), message lines swapped per group.
    constexpr uint16_t kNoId = 0xFFFF;
    struct OneOfGroup
    {
        uint16_t ids[4]; // kNoId = unused (roster id 0 is The Rock, so 0 is a real id)
        const char *line1, *line2;
    };
    // A group with one id does nothing until its second slot is added (roster expansion): just add the new id.
    constexpr OneOfGroup kOneOfGroups[] = {
        {{0x03, 0x48, kNoId, kNoId}, "You can't enter Undertaker and", "The Undertaker at the same time."}, // stock
        {{0x20, 0x4E, kNoId, kNoId}, "You can't enter Hulk Hogan and", "Legend Hulk Hogan at the same time."},
        {{0x07, kNoId, kNoId, kNoId}, "You can't enter Kane and", "Legend Kane at the same time."},           // + 90s Kane
        {{0x00, kNoId, kNoId, kNoId}, "You can't enter The Rock and", "Legend The Rock at the same time."},   // + SvR 90s Rock
        {{0x44, 0xA2, kNoId, kNoId}, "You can't enter Roddy Piper and", "Legend Roddy Piper at the same time."}, // Piper = 0x44 (0x45 = DiBiase); 0xA2 = new Modern Piper slot
        // Mr. America: add 0x20's group if he should count as a Hogan.
    };
    int g_oneOfHit = -1;          // group of the last refusal (GetWarningMsg reads it)
    uint32_t g_oneOfMsg[std::size(kOneOfGroups)] = {}; // guest {char *lines[5], count} per group
    int oneOfGroupOf(uint16_t id)
    {
        for (size_t g = 0; g < std::size(kOneOfGroups); ++g)
            for (uint16_t m : kOneOfGroups[g].ids)
                if (m != kNoId && m == id)
                    return static_cast<int>(g);
        return -1;
    }
    // LEGEND banner. IsLegend 0x12d200 (leaf, a0 ROSTER_ID) is a hard-coded id list; it drives the select-screen info
    // plate (SelectWrestlerDrawer::DrawInfoPlate 0x2883c0: plate shifted 0x37 + legend banner), the Season superstar
    // list (CSeasonCharaFrame::DrawSuperStarList 0x319ee0) and keeps legends out of Royal Rumble random entrants
    // (CRoyalRumbleFrame::ElectRumbleStar 0x300690). Stock list below; add new legend slot ids to kExtraLegends
    // (90s Rock, 90s Kane, Andre...). Modern Piper / Modern Hogan 0x20 stay out.
    constexpr uint16_t kStockLegends[] = {0x3A, 0x44, 0x45, 0x46, 0x48, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52};
    constexpr uint16_t kExtraLegends[] = {kNoId};
    void isLegendExtended(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t id = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0xFFFFu;
        uint32_t legend = 0;
        for (uint16_t l : kStockLegends)
            legend |= l == id;
        for (uint16_t l : kExtraLegends)
            legend |= l != kNoId && l == id;
        SET_GPR_U32(ctx, 2, legend);
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }
    uint16_t rosterIdAt(const uint8_t *rdram, uint32_t self, uint32_t i)
    {
        uint16_t id;
        std::memcpy(&id, rdram + self + 0x362u + i * 2u, 2);
        return id;
    }
    int8_t rosterFlagAt(const uint8_t *rdram, uint32_t self, uint32_t i) { return static_cast<int8_t>(rdram[self + 0x2ECu + i]); }
    void checkRosterSpecialConditionGroups(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        uint32_t idx;
        std::memcpy(&idx, rdram + self + 0x458u, 4);
        uint32_t result = 0;
        if (idx < 0x76u)
        {
            const uint16_t id = rosterIdAt(rdram, self, idx);
            const int g = oneOfGroupOf(id);
            // only refuse an ADD (CheckEntryOther toggled the flag before calling us), so a roster that somehow holds two
            // can still drop one
            if (g >= 0 && rosterFlagAt(rdram, self, idx) >= 1)
                for (uint32_t i = 0; i < 0x76u && !result; ++i)
                    if (i != idx && rosterIdAt(rdram, self, i) != id && oneOfGroupOf(rosterIdAt(rdram, self, i)) == g &&
                        rosterFlagAt(rdram, self, i) >= 1)
                    {
                        result = 2;
                        g_oneOfHit = g;
                    }
        }
        SET_GPR_U32(ctx, 2, result);
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }
    PS2Runtime::RecompiledFunction g_origSetRosterSpecialCondition = nullptr;
    void setRosterSpecialConditionGroups(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu,
                       ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        g_origSetRosterSpecialCondition(rdram, ctx, runtime); // stock Taker pair (keeps its feud/title preference)
        if (ctx->pc != ra)
            return;
        uint32_t mode;
        std::memcpy(&mode, rdram + self + 0x2E8u, 4);
        if (mode != 0u && mode != 4u) // same gate as stock
            return;
        for (size_t g = 1; g < std::size(kOneOfGroups); ++g)
        {
            int keep = -1;
            for (uint32_t i = 0; i < 0x76u; ++i) // a locked (2) member wins, else the first one entered
                if (oneOfGroupOf(rosterIdAt(rdram, self, i)) == static_cast<int>(g) && rosterFlagAt(rdram, self, i) >= 1 &&
                    (keep < 0 || (rosterFlagAt(rdram, self, i) == 2 && rosterFlagAt(rdram, self, keep) != 2)))
                    keep = static_cast<int>(i);
            for (uint32_t i = 0; keep >= 0 && i < 0x76u; ++i)
                if (static_cast<int>(i) != keep && oneOfGroupOf(rosterIdAt(rdram, self, i)) == static_cast<int>(g) &&
                    rosterFlagAt(rdram, self, i) == 1)
                {
                    rdram[self + 0x2ECu + i] = 0;
                    std::fprintf(stderr, "[season] roster: %s only once, dropped id 0x%X%c", kOneOfGroups[g].line1,
                                 rosterIdAt(rdram, self, i), 10);
                }
        }
    }
    PS2Runtime::RecompiledFunction g_origGetWarningMsg = nullptr;
    // GetWarningMsg 0x168e40 (a0 kind, a1 msg) -> pointer to {char **lines, u32 count}
    void getWarningMsgGroups(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t kind = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])),
                       msg = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        g_origGetWarningMsg(rdram, ctx, runtime);
        if (kind != 5u || msg != 29u || g_oneOfHit <= 0 || g_oneOfHit >= static_cast<int>(std::size(kOneOfGroups)))
            return;
        uint32_t &rec = g_oneOfMsg[g_oneOfHit];
        if (!rec)
        {
            // copy the stock record (5 lines: "@C", line1, line2, "Please select one.", "") and swap lines 1-2
            const uint32_t stock = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2])) & 0x1FFFFFFu;
            uint32_t stockLines, count;
            std::memcpy(&stockLines, rdram + stock, 4);
            std::memcpy(&count, rdram + stock + 4u, 4);
            if (count != 5u)
                return;
            const OneOfGroup &grp = kOneOfGroups[g_oneOfHit];
            const size_t l1 = std::strlen(grp.line1) + 1u, l2 = std::strlen(grp.line2) + 1u;
            const uint32_t buf = runtime->guestMalloc(static_cast<uint32_t>(8u + 20u + l1 + l2));
            if (!buf)
                return;
            uint8_t *p = rdram + (buf & 0x1FFFFFFu);
            const uint32_t lines = buf + 8u, s1 = buf + 28u, s2 = s1 + static_cast<uint32_t>(l1);
            std::memcpy(p, &lines, 4);
            std::memcpy(p + 4, &count, 4);
            std::memcpy(p + 8, rdram + (stockLines & 0x1FFFFFFu), 20);
            std::memcpy(p + 12, &s1, 4);
            std::memcpy(p + 16, &s2, 4);
            std::memcpy(p + 28, grp.line1, l1);
            std::memcpy(p + 28 + l1, grp.line2, l2);
            rec = buf;
        }
        SET_GPR_U32(ctx, 2, rec);
    }

    // Select-list expansion. The character select list is MENU/CHSL: kStockSelect 0x80-byte records (+2 s16 roster id,
    // +4 name, +0x24/+0x46 finisher text with codes at +0x44/+0x66 filled by ReadEndProcess, +0x68 s16[5] costume id
    // per attire slot (-1 = none), +0x74 s16[5] second costume set, +0x72/+0x7E flags). GetSelectRoster 0x26fb90
    // returns the hard 0x41. After CSSNameListReader::ReadEndProcess the reader's list (reader+0x20) is swapped for a
    // guest-heap copy: the stock records first (every other screen keeps its indices and 65-entry copy), then one
    // record per restored wrestler. Only the exhibition select (SelectWrestlerFrame, list inline at +0x190 sized for
    // 65) is pointed at the long list: its accessor and SelectWresTable::Make read a per-frame guest-heap buffer.
    // GetSelectRoster returns the long count only inside those paths and in RosterID2IndexNo (reader-level lookups).
    // The count is dynamic (stock + appended), so more slots later = more appended records.
    constexpr uint32_t kStockSelect = 0x41, kSelectRec = 0x80, kSelectMax = 0x400;
    uint32_t g_selectCount = kStockSelect;                           // stock + appended records
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_readerLists; // reader -> (original list, long list)
    std::map<uint32_t, uint32_t> g_frameLists;                       // SelectWrestlerFrame +0x190 -> long list buffer
    int g_selectLongDepth = 0;
    PS2Runtime::RecompiledFunction g_origReadEndProcess = nullptr, g_origReaderDtor = nullptr,
                                   g_origReaderGetList = nullptr, g_origWresTableMake = nullptr;
    uint32_t gprU32(R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); }
    uint32_t rdU32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4);
        return v;
    }
    void wrU32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x1FFFFFFu), &v, 4); }
    int16_t rdS16(const uint8_t *p)
    {
        int16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }

    void expandReaderList(uint8_t *rdram, PS2Runtime *runtime, uint32_t reader);
    uint32_t rosterxCount();                                          // roster expansion section (near applyHctp)
    uint32_t rosterxAppendSelect(uint8_t *rdram, uint8_t *out, uint32_t n);
    void readEndProcessSelectList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reader = gprU32(ctx, 4);
        g_origReadEndProcess(rdram, ctx, runtime);
        expandReaderList(rdram, runtime, reader);
    }
    // Appends the restored records to the reader's CHSL list (also called lazily from GetSuperStarList, since the
    // Create-a-Moveset picker can use a reader whose ReadEndProcess we never saw).
    void expandReaderList(uint8_t *rdram, PS2Runtime *runtime, uint32_t reader)
    {
        const uint32_t list = rdU32(rdram, reader + 0x20u);
        if (list == 0u || list >= 0x2000000u)
            return;
        // A reader re-read (or freed without the dtor and rebuilt at the same address) holds a fresh stock list: the
        // old entry is stale, so expand again instead of keeping the select screen on a dead long list.
        if (auto it = g_readerLists.find(reader); it != g_readerLists.end())
        {
            if (it->second.second == list)
                return;
            std::fprintf(stderr, "[selist] reader %08x re-read; stale long list %08x dropped%c", reader,
                         it->second.second, 10);
            g_readerLists.erase(it);
        }
        std::vector<const CutWrestler *> extra;
        for (size_t i = 0; i < kCutCount; ++i)
            if (g_cutRosterHasModel[i])
                extra.push_back(&kCutRoster[i]);
        if (extra.empty() && rosterxCount() == 0u)
            return;
        const uint32_t total = kStockSelect + static_cast<uint32_t>(extra.size()) + rosterxCount();
        const uint32_t buf = runtime->guestMalloc(total * kSelectRec);
        if (buf == 0u)
            return;
        uint8_t *const out = rdram + buf;
        std::memcpy(out, rdram + list, kStockSelect * kSelectRec);
        uint32_t n = kStockSelect;
        for (const CutWrestler *w : extra)
        {
            const uint8_t *tmpl = nullptr;
            for (uint32_t s = 0; s < kStockSelect && !tmpl; ++s)
                if (rdS16(out + s * kSelectRec + 2) == w->tmpl)
                    tmpl = out + s * kSelectRec;
            if (!tmpl)
                continue;
            uint8_t *const rec = out + n * kSelectRec;
            std::memcpy(rec, tmpl, kSelectRec);
            const int16_t id = w->id;
            std::memcpy(rec + 2, &id, 2);
            std::memset(rec + 4, 0, 0x20);
            std::strncpy(reinterpret_cast<char *>(rec + 4), w->name, 0x1F);
            // Original ReadEndProcess fills only the stock 65. Never inherit
            // another wrestler's SmackDown names/situations from the template.
            if (!hctp_restored_finishers::refresh(rdram, 0x2000000u, buf + n * kSelectRec, w->id))
            {
                std::memset(rec + 0x24, 0, 0x44); // blank, never the template wrestler's finishers (Al Snow showed 619)
                std::fprintf(stderr, "[selist] restored finisher display pending for %u%c", unsigned(w->id), 10);
            }
            // +0x72 = special-referee flag (MakeSpecialRefereeTable 0x2bdd50 keeps records with it set). Templates HBK /
            // Vince / Steph / Bischoff are referees, so Hogan, Linda, Lawler, Cole... inherited it and crashed as
            // referee (no referee attire). Restored wrestlers are never referees (Jordan Nov 5).
            rec[0x72] = 0;
            for (uint32_t set : {0x68u, 0x74u}) // costume ids: template ids for the attire slots that have models
            {
                const int16_t first = rdS16(tmpl + set);
                for (uint32_t slot = 0; slot < 5; ++slot)
                {
                    int16_t v = -1;
                    if (slot < w->attires && first != -1)
                        v = rdS16(tmpl + set + slot * 2) != -1 ? rdS16(tmpl + set + slot * 2) : first;
                    std::memcpy(rec + set + slot * 2, &v, 2);
                }
            }
            ++n;
        }
        n = rosterxAppendSelect(rdram, out, n); // roster expansion (HCTP_ROSTER_EXPAND) new slots
        g_selectCount = n;
        g_readerLists[reader] = {list, buf};
        wrU32(rdram, reader + 0x20u, buf);
        std::cerr << "[hctp] select list: " << n << " superstars (" << (n - kStockSelect) << " restored)" << std::endl;
    }
    // CSSNameListReader dtor 0x26fc20: hand the game back its own CHSL list pointer before it frees it
    void readerDtorSelectList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reader = gprU32(ctx, 4);
        if (auto it = g_readerLists.find(reader); it != g_readerLists.end())
        {
            wrU32(rdram, reader + 0x20u, it->second.first);
            runtime->guestFree(it->second.second);
            g_readerLists.erase(it);
        }
        g_origReaderDtor(rdram, ctx, runtime);
    }
    // CSSNameListReader::GetSuperStarList 0x26fe60 (a0 reader, a1 dest): the stock 65 always go to dest; a call from
    // SelectWrestlerFrame::ChangeIntroduction (0x24e410-0x24e810) also fills that frame's long-list buffer.
    void readerGetListSelectList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reader = gprU32(ctx, 4), dest = gprU32(ctx, 5), ra = gprU32(ctx, 31);
        g_origReaderGetList(rdram, ctx, runtime);
        // SelectWrestlerFrame::ChangeIntroduction, or CEasySelectWrestler::WaitAction 0x2fd8a4 (Create-a-Moveset
        // picker: reader +0x28, inline 65-record list at +0x2C; its record addresses go through hctpEasyListAddr).
        const bool longCaller = (ra >= 0x24e410u && ra < 0x24e810u) || ra == 0x2fd8acu;
        {
            static int calls = 0; // where each screen gets its superstar list (Create-a-Moveset picker diagnosis)
            if (calls++ < 40)
                std::fprintf(stderr, "[selist] GetSuperStarList ra %08x reader %08x dest %08x known %d long %d%c", ra,
                             reader, dest, int(g_readerLists.count(reader)), int(longCaller), 10);
        }
        if (longCaller && !g_readerLists.count(reader))
            expandReaderList(rdram, runtime, reader);
        auto rl = g_readerLists.find(reader);
        if (rl != g_readerLists.end() && !longCaller)
        {
            static int logged = 0; // callers that only get the stock 65
            if (logged++ < 32)
                std::fprintf(stderr, "[selist] GetSuperStarList stock-only ra %08x dest %08x%c", ra, dest, 10);
        }
        if (!longCaller || rl == g_readerLists.end() || g_selectCount > kSelectMax)
            return;
        // CAM can change a finisher while this reader survives. Refresh the
        // appended display fields from live assignments before copying again.
        for (uint32_t s = kStockSelect; s < g_selectCount; ++s)
        {
            const uint32_t p = rl->second.second + s * kSelectRec;
            const int16_t id = rdS16(rdram + p + 2);
            if (id >= 0)
                hctp_restored_finishers::refresh(rdram, 0x2000000u, p, uint16_t(id));
        }
        // Sized to the real list and freed by the screen's dtor: a lingering block splits the guest heap and
        // Create-a-Moveset's big moves-data allocation then never fits (endless Now Loading).
        uint32_t &buf = g_frameLists[dest];
        if (buf == 0u)
            buf = runtime->guestMalloc(g_selectCount * kSelectRec);
        if (buf == 0u)
        {
            g_frameLists.erase(dest);
            return;
        }
        // Alphabetical like the stock list (which files "The Rock" under R): each restored record goes in front of the
        // first stock record whose name sorts after it, ignoring a leading "The " and case; stock order is untouched.
        const uint8_t *const src = rdram + rl->second.second;
        const uint8_t *const stockEnd = src + kStockSelect * kSelectRec;
        auto key = [](const uint8_t *rec) {
            std::string s(reinterpret_cast<const char *>(rec + 4), strnlen(reinterpret_cast<const char *>(rec + 4), 0x20));
            if (s.rfind("The ", 0) == 0)
                s.erase(0, 4);
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::vector<const uint8_t *> order;
        for (uint32_t s = 0; s < kStockSelect; ++s)
            order.push_back(src + s * kSelectRec);
        for (uint32_t e = kStockSelect; e < g_selectCount; ++e)
        {
            const std::string k = key(src + e * kSelectRec);
            auto at = std::find_if(order.begin(), order.end(),
                                   [&](const uint8_t *r) { return r < stockEnd && key(r) > k; });
            order.insert(at, src + e * kSelectRec);
        }
        for (uint32_t i = 0; i < order.size(); ++i)
            std::memcpy(rdram + buf + i * kSelectRec, order[i], kSelectRec);
    }
    // SelectWrestlerFrame::GetSuperStarList(i) 0x24fa40 (leaf replacement)
    void frameGetListSelectList(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t frame = gprU32(ctx, 4), i = gprU32(ctx, 5);
        uint32_t base = frame + 0x190u, count = kStockSelect;
        if (auto it = g_frameLists.find(base); it != g_frameLists.end() && it->second != 0u)
            base = it->second, count = g_selectCount;
        SET_GPR_U32(ctx, 2, i < count ? base + i * kSelectRec : 0u);
        ctx->pc = gprU32(ctx, 31);
    }
    // SelectWresTable::Make 0x2bd880 (a2 = superstar list)
    void wresTableMakeSelectList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto it = g_frameLists.find(gprU32(ctx, 6));
        if (it == g_frameLists.end() || it->second == 0u)
        {
            g_origWresTableMake(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U32(ctx, 6, it->second);
        ++g_selectLongDepth;
        g_origWresTableMake(rdram, ctx, runtime);
        --g_selectLongDepth;
    }
    // SelectWrestlerFrame dtor 0x24da10 (list +0x190) / CEasySelectWrestler dtor 0x2fd460 (list +0x2C): free the long list
    PS2Runtime::RecompiledFunction g_origFrameDtor = nullptr, g_origEasyDtor = nullptr;
    void freeFrameList(PS2Runtime *runtime, uint32_t key)
    {
        if (auto it = g_frameLists.find(key); it != g_frameLists.end())
        {
            if (it->second != 0u)
                runtime->guestFree(it->second);
            g_frameLists.erase(it);
        }
    }
    void frameDtorSelectList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        freeFrameList(runtime, gprU32(ctx, 4) + 0x190u);
        g_origFrameDtor(rdram, ctx, runtime);
    }
    void easyDtorSelectList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        freeFrameList(runtime, gprU32(ctx, 4) + 0x2Cu);
        g_origEasyDtor(rdram, ctx, runtime);
    }
    // Create-a-Moveset picker: CSuperStarListDrawer::SetSuperStarList 0x3934c0 walks roster ids itself and keeps an id
    // only if IsSmackDownEntry 0x12cfd0 and !IsRosterHidden 0x12d160 (buffer sized for all 162 ids, no 65 limit). Cut
    // wrestlers fail both, so inside that builder restored ids with a model pass. Other callers keep stock answers.
    PS2Runtime::RecompiledFunction g_origIsSmackDownEntry = nullptr, g_origIsRosterHidden = nullptr;
    bool restoredFromSSList(R5900Context *ctx)
    {
        const uint32_t ra = gprU32(ctx, 31), id = gprU32(ctx, 4);
        if (ra < 0x3934c0u || ra >= 0x3938b8u)
            return false;
        for (size_t i = 0; i < kCutCount; ++i)
            if (g_cutRosterHasModel[i] && kCutRoster[i].id == id)
                return true;
        return false;
    }
    void isSmackDownEntrySSList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!restoredFromSSList(ctx))
            return g_origIsSmackDownEntry(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 2, 1u);
        ctx->pc = gprU32(ctx, 31);
    }
    void isRosterHiddenSSList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!restoredFromSSList(ctx))
            return g_origIsRosterHidden(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 2, 0u);
        ctx->pc = gprU32(ctx, 31);
    }
    // GetSelectRoster 0x26fb90 (leaf replacement)
    // settings.toml unlock_all (the launcher's "Unlock everything", VENDETTA_UNLOCKALL=1): HideCtrl::IsHideOpen
    // (0x306060), the one check for every shop / hidden item (legends, attires, arenas, movies), answers "open".
    // The save is not changed: buying or opening still writes the stock flags.
    void isHideOpenUnlockAll(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U32(ctx, 2, 1u);
        ctx->pc = gprU32(ctx, 31);
    }

    void getSelectRosterSelectList(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t ra = gprU32(ctx, 31);
        // RosterID2IndexNo; CEasySelectWrestler::SelectWresAction's cursor bound (0x2fdd28) when its list is long
        const bool longList = g_selectLongDepth > 0 || (ra >= 0x26fea0u && ra < 0x26ff1cu) ||
                              (ra == 0x2fdd30u && g_frameLists.count(gprU32(ctx, 17) + 0x2Cu));
        SET_GPR_U32(ctx, 2, longList ? g_selectCount : kStockSelect);
        ctx->pc = ra;
    }
    bool g_selectListInstalled = false;
    void installSelectList(PS2Runtime &runtime)
    {
        if (g_selectListInstalled)
            return;
        g_selectListInstalled = true;
        g_origReadEndProcess = runtime.lookupFunction(0x26fcb0u);
        g_origReaderDtor = runtime.lookupFunction(0x26fc20u);
        g_origReaderGetList = runtime.lookupFunction(0x26fe60u);
        g_origWresTableMake = runtime.lookupFunction(0x2bd880u);
        const bool ok = g_origReadEndProcess && g_origReaderDtor && g_origReaderGetList && g_origWresTableMake &&
                        runtime.replaceFunction(0x26fcb0u, readEndProcessSelectList) &&
                        runtime.replaceFunction(0x26fc20u, readerDtorSelectList) &&
                        runtime.replaceFunction(0x26fe60u, readerGetListSelectList) &&
                        runtime.replaceFunction(0x24fa40u, frameGetListSelectList) &&
                        runtime.replaceFunction(0x2bd880u, wresTableMakeSelectList) &&
                        runtime.replaceFunction(0x26fb90u, getSelectRosterSelectList);
        if (ok && (g_origFrameDtor = runtime.lookupFunction(0x24da10u)) != nullptr)
            runtime.replaceFunction(0x24da10u, frameDtorSelectList);
        if (ok && (g_origEasyDtor = runtime.lookupFunction(0x2fd460u)) != nullptr)
            runtime.replaceFunction(0x2fd460u, easyDtorSelectList);
        if (const char *u = std::getenv("VENDETTA_UNLOCKALL"); u && *u == '1')
        {
            runtime.replaceFunction(0x306060u, isHideOpenUnlockAll);
            std::fprintf(stderr, "[hctp] unlock all: every hidden item open (HideCtrl::IsHideOpen)%c", 10);
        }
        if (ok && (g_origIsSmackDownEntry = runtime.lookupFunction(0x12cfd0u)) != nullptr)
            runtime.replaceFunction(0x12cfd0u, isSmackDownEntrySSList);
        if (ok && (g_origIsRosterHidden = runtime.lookupFunction(0x12d160u)) != nullptr)
            runtime.replaceFunction(0x12d160u, isRosterHiddenSSList);
        std::cerr << "[hctp] select list expansion " << (ok ? "on" : "FAILED") << std::endl;
    }

    void startPcProfiler()
    {
        if (!std::getenv("HCTP_PCPROF"))
            return;
        std::thread([] {
            using namespace ps2_host_profile;
            auto report = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            uint32_t tick = 0;
            // the game thread's host call stack every 50 ms (recompiled functions by name): where it waits
            constexpr size_t kMaxStacks = 100, kDepth = 24;
            static uint64_t stacks[kMaxStacks][kDepth];
            size_t lens[kMaxStacks] = {};
            size_t stackCount = 0;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const Track track = static_cast<Track>(tick++ % TrackCount);
                samplePc(track, g_phases[track].load(std::memory_order_relaxed));
                if (tick % 50u == 0u && stackCount < kMaxStacks)
                {
                    lens[stackCount] = stackScan(GameTrack, stacks[stackCount], kDepth);
                    ++stackCount;
                }
                if (std::chrono::steady_clock::now() < report)
                    continue;
                report += std::chrono::seconds(5);
                std::cerr << pcReport(15u, 0u) << std::flush;
                std::map<std::string, uint32_t> seen;
                for (size_t i = 0; i < stackCount; ++i)
                    ++seen[describeStack(stacks[i], lens[i])];
                std::vector<std::pair<uint32_t, std::string>> ranked;
                for (auto &[stack, count] : seen)
                    ranked.emplace_back(count, stack);
                std::sort(ranked.rbegin(), ranked.rend());
                for (size_t i = 0; i < ranked.size() && i < 4u; ++i)
                    std::cerr << "[hctpstack] " << ranked[i].first << "/" << stackCount << " " << ranked[i].second << "\n";
                std::cerr << std::flush;
                stackCount = 0;
            }
        }).detach();
    }


    // TEMP diag (HCTP_FONTLOG=1): CFontDraw::ShowString 0x162a20 (this, const uchar *str, ...) - each distinct string
    // (hex + text) with the font object's table pointer, for the garbled HUD / season dialogue text.
    PS2Runtime::RecompiledFunction g_origShowString = nullptr;
    void showStringLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::set<std::string> seen;
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        const uint32_t str = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & 0x1FFFFFFu;
        if (seen.size() < 400 && str != 0u)
        {
            std::string raw;
            for (uint32_t i = 0; i < 80u && rdram[str + i] != 0u; ++i)
                raw.push_back(static_cast<char>(rdram[str + i]));
            char key[16];
            std::snprintf(key, sizeof(key), "%x:", self);
            if (seen.insert(std::string(key) + raw).second)
            {
                uint32_t font = 0, count = 0, table = 0;
                std::memcpy(&font, rdram + self + 0x154u, 4);
                font &= 0x1FFFFFFu;
                if (font)
                {
                    std::memcpy(&count, rdram + font + 0x14u, 4);
                    std::memcpy(&table, rdram + font + 0x20u, 4);
                }
                std::string hex, text;
                char b[8];
                for (unsigned char c : raw)
                {
                    std::snprintf(b, sizeof(b), "%02x", c);
                    hex += b;
                    text.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.');
                }
                if (font && table && count && count < 5000u)
                {
                    static std::set<uint32_t> dumped;
                    if (dumped.insert(font).second)
                    {
                        std::cerr << "[fontlog] font 0x" << std::hex << font << " header:";
                        for (uint32_t o = 0; o < 0x40u; o += 4)
                        {
                            uint32_t w;
                            std::memcpy(&w, rdram + font + o, 4);
                            std::cerr << " " << w;
                        }
                        std::cerr << " codes:";
                        for (uint32_t i = 0; i < count && i < 120u; ++i)
                        {
                            uint16_t c;
                            std::memcpy(&c, rdram + ((table + i * 2u) & 0x1FFFFFFu), 2);
                            std::cerr << " " << c;
                        }
                        std::cerr << std::dec << std::endl;
                    }
                }
                std::cerr << "[fontlog] obj=0x" << std::hex << self << " font=0x" << font << " n=" << std::dec << count
                          << " tab=0x" << std::hex << table << " ra=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]))
                          << std::dec << " \"" << text << "\" " << hex << std::endl;
            }
        }
        g_origShowString(rdram, ctx, runtime);
    }

    // TEMP diag (HCTP_FONTLOG=1): CFontDraw::GetFontCell 0x1625b0 (this, c1, c2, const uchar *&, FontCell *&, int &, int &)
    PS2Runtime::RecompiledFunction g_origGetFontCell = nullptr;
    void getFontCellLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto reg = [&](int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); };
        const uint32_t self = reg(4) & 0x1FFFFFFu, c1 = reg(5) & 0xFFu, c2 = reg(6) & 0xFFu, ra = reg(31);
        const uint32_t pCell = reg(8) & 0x1FFFFFFu, pW = reg(9) & 0x1FFFFFFu, pH = reg(10) & 0x1FFFFFFu;
        g_origGetFontCell(rdram, ctx, runtime);
        static std::set<uint64_t> seen;
        const uint64_t key = (static_cast<uint64_t>(ra) << 32) | (self << 8) ^ (c1 << 8) | c2;
        static const auto cellT0 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - cellT0).count() > 400.0 && seen.size() < 600 && seen.insert(key).second)
        {
            uint32_t cell = 0, w = 0, h = 0, cf[4] = {};
            std::memcpy(&cell, rdram + pCell, 4);
            std::memcpy(&w, rdram + pW, 4);
            std::memcpy(&h, rdram + pH, 4);
            cell &= 0x1FFFFFFu;
            if (cell)
                std::memcpy(cf, rdram + cell, 16);
            static std::set<uint32_t> objSeen;
            if (objSeen.insert(self).second)
            {
                uint64_t t100 = 0, t140 = 0, t148 = 0;
                std::memcpy(&t100, rdram + self + 0x100u, 8);
                std::memcpy(&t140, rdram + self + 0x140u, 8);
                std::memcpy(&t148, rdram + self + 0x148u, 8);
                auto desc = [](uint64_t t, char *out, size_t n) {
                    std::snprintf(out, n, "tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x", (unsigned)(t & 0x3FFF),
                                  (unsigned)((t >> 14) & 0x3F), (unsigned)((t >> 20) & 0x3F), (unsigned)((t >> 26) & 0xF),
                                  (unsigned)((t >> 30) & 0xF), (unsigned)((t >> 37) & 0x3FFF));
                };
                char a[96], b[96], c3[96];
                desc(t100, a, sizeof(a));
                desc(t140, b, sizeof(b));
                desc(t148, c3, sizeof(c3));
                std::fprintf(stderr, "[fontobj] obj=0x%x +100 %s | +140 %s | +148 %s\n", self, a, b, c3);
            }
            std::fprintf(stderr, "[fontcell] ra=0x%x obj=0x%x c=%02x '%c' %02x -> v0=0x%x cell=0x%x [%08x %08x %08x %08x] w=%d h=%d\n",
                         ra, self, c1, (c1 >= 0x20 && c1 < 0x7f) ? static_cast<char>(c1) : '.', c2, reg(2), cell, cf[0], cf[1],
                         cf[2], cf[3], static_cast<int>(w), static_cast<int>(h));
        }
    }

    // TEMP diag (HCTP_FONTLOG=1): FontCtrlData*::SetTex0(u64 &tex0) - the TEX0 each font kind draws with
    PS2Runtime::RecompiledFunction g_origSetTex0[4] = {};
    const uint32_t kSetTex0Addr[4] = {0x194f60u, 0x3a5ba0u, 0x2d3550u, 0x233710u};
    template <int I>
    void setTex0Logged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        const uint32_t out = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & 0x1FFFFFFu;
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        g_origSetTex0[I](rdram, ctx, runtime);
        uint64_t tex0 = 0;
        std::memcpy(&tex0, rdram + out, 8);
        static std::set<uint64_t> seen;
        if (seen.size() < 200 && seen.insert(tex0 ^ (static_cast<uint64_t>(I) << 62)).second)
        {
            static const auto t0 = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[settex0] t=%.1f kind=%d (0x%x) obj=0x%x ra=0x%x tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u cbp=0x%x\n",
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), I, kSetTex0Addr[I], self, ra,
                         static_cast<unsigned>(tex0 & 0x3FFF), static_cast<unsigned>((tex0 >> 14) & 0x3F),
                         static_cast<unsigned>((tex0 >> 20) & 0x3F), static_cast<unsigned>((tex0 >> 26) & 0xF),
                         static_cast<unsigned>((tex0 >> 30) & 0xF), static_cast<unsigned>((tex0 >> 37) & 0x3FFF));
        }
    }

    // TEMP diag (HCTP_SPRLOG=1): CSpriteDraw::ShowFast 0x15f450 object fields (positions 0x90-0xD4) for the ATTENTION
    // screen's missing plate/icon sprites.
    PS2Runtime::RecompiledFunction g_origShowFast = nullptr;
    const auto start = std::chrono::steady_clock::now(); // process start (static init)
    void showFastLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logged = 0;
        const uint32_t raNow = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        if (logged < 120 && raNow == 0x15eec8u)
        {
            ++logged;
            const uint32_t obj = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
            const uint32_t vec = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & 0x1FFFFFFu;
            char line[1024];
            int n = std::snprintf(line, sizeof(line), "[sprlog] obj=0x%x vec=0x%x ra=0x%x v:", obj, vec, raNow);
            uint32_t parent; std::memcpy(&parent, rdram + obj + 0x80, 4);
            parent &= 0x1FFFFFFu;
            n += std::snprintf(line + n, sizeof(line) - n, " v0=(%g,%g) M:", *reinterpret_cast<float *>(rdram + vec), *reinterpret_cast<float *>(rdram + vec + 4));
            for (uint32_t off = 0x40; off < 0x80 && n < 600; off += 4)
                n += std::snprintf(line + n, sizeof(line) - n, " %g", *reinterpret_cast<float *>(rdram + obj + off));
            n += std::snprintf(line + n, sizeof(line) - n, " parent=0x%x", parent);
            if (parent)
                for (uint32_t off = 0x40; off < 0x80 && n < 900; off += 4)
                    n += std::snprintf(line + n, sizeof(line) - n, " %g", *reinterpret_cast<float *>(rdram + parent + off));
            std::cerr << line << std::endl;
        }
        g_origShowFast(rdram, ctx, runtime);
    }

    // TEMP diag (HCTP_SPRLOG=1): CSprAnimData::SetData(const void *data, uint size) 0x149940 - raw sprite-anim data
    PS2Runtime::RecompiledFunction g_origSetData = nullptr;
    void setDataLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logged = 0;
        if (logged++ < 12)
        {
            const uint32_t data = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & 0x1FFFFFFu;
            const uint32_t size = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            char line[1024];
            int n = std::snprintf(line, sizeof(line), "[sprdata] this=0x%x data=0x%x size=%u:",
                                  static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])), data, size);
            for (uint32_t off = 0; off < 160 && n < 980; off += 4)
            {
                uint32_t w; std::memcpy(&w, rdram + data + off, 4);
                n += std::snprintf(line + n, sizeof(line) - n, " %08x", w);
            }
            std::cerr << line << std::endl;
        }
        g_origSetData(rdram, ctx, runtime);
    }

    // Movie skip hacks (ported from the good-base fork): Sofdec playback does not work yet, so the game would wait on
    // movies forever (stuck on the "Now Loading" screen). HCTP_MOVIES=1 runs the game's own movie code instead.
    void titleMovieLoopEndActSkip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // TitleMovieSequence::LoopEndAct 0x1377c0: mark the sequence done (state 6) and go to the next one
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        std::cerr << "[skipmovie] TitleMovieSequence skip self=0x" << std::hex << self << std::dec << std::endl;
        const uint32_t state = 6u;
        std::memcpy(rdram + ((self + 0x18u) & 0x1FFFFFFu), &state, 4);
        ctx->pc = 0x137ef0u;
        runtime->lookupFunction(0x137ef0u)(rdram, ctx, runtime); // TitleMovieSequence::GoNext
    }


    // TEMP diag (HCTP_ADXTLOG=1): every ADXT start/stop/create call with a0..a3, ra and (Fname) the file name -
    // which API music / SE use, for the native ADX player
    struct AdxtHook { uint32_t addr; const char *name; PS2Runtime::RecompiledFunction orig; };
    AdxtHook g_adxtHooks[] = {
        {0x3bd778u, "Create", nullptr}, {0x3bdab8u, "Destroy", nullptr}, {0x3bf760u, "StartFname", nullptr},
        {0x3b6918u, "StartFnameLp", nullptr}, {0x3bf668u, "StartAfs", nullptr}, {0x3bf800u, "StartMem", nullptr},
        {0x3bf8f8u, "StartMemIdx", nullptr}, {0x3bdeb8u, "StartSj", nullptr}, {0x3b6838u, "StartSeamless", nullptr},
        {0x3bdfe0u, "Stop", nullptr}, {0x3bdf40u, "StopWithoutLsc", nullptr}, {0x3b6710u, "EntryFname", nullptr},
        {0x3b6770u, "EntryAfs", nullptr}, {0x3b68e8u, "SetSeamlessLp", nullptr}, {0x3be960u, "SetOutVol", nullptr}};

    template <int I>
    void adxtLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const AdxtHook &h = g_adxtHooks[I];
        auto reg = [ctx](int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); };
        char line[400];
        int n = std::snprintf(line, sizeof(line), "[adxt] %s a0=%x a1=%x a2=%x a3=%x ra=%x", h.name, reg(4), reg(5), reg(6), reg(7), reg(31));
        const uint32_t strArg = I == 2 || I == 3 || I == 11 ? reg(5) : 0u; // StartFname/Lp/EntryFname (adxt, fname)
        if (strArg && strArg < 0x2000000u)
            std::snprintf(line + n, sizeof(line) - n, " \"%.120s\"", reinterpret_cast<const char *>(rdram + strArg));
        std::cerr << line << std::endl;
        h.orig(rdram, ctx, runtime);
        if (I == 0)
            std::cerr << "[adxt] Create -> " << std::hex << reg(2) << std::dec << std::endl;
    }

    template <int... I>
    void installAdxtLog(PS2Runtime &runtime, std::integer_sequence<int, I...>)
    {
        ((g_adxtHooks[I].orig = runtime.lookupFunction(g_adxtHooks[I].addr),
          g_adxtHooks[I].orig && runtime.replaceFunction(g_adxtHooks[I].addr, adxtLogged<I>)), ...);
    }

    // TEMP diag (HCTP_CHEERLOG=1, Nov 4): entrance crowd cheer (SFX 0x12D, PlayBaseCheerInEntrance) start/stop paths -
    // Modern Hogan at WrestleMania has no crowd. stage = current stage byte 0x4922C0.
    AdxtHook g_cheerHooks[] = {
        {0x3adf30u, "PlayBaseCheerInEntrance", nullptr}, {0x3adef0u, "PlayEnvInGame", nullptr},
        {0x1adfb0u, "EnvSounds::Play", nullptr}, {0x1ae0a0u, "EnvSounds::FadeStop", nullptr},
        {0x1adaf0u, "EnvSounds::StartBG_Env", nullptr}, {0x1adf30u, "EnvSounds::EndBG_Env", nullptr},
        {0x2b60e0u, "CEventBGM::Start", nullptr}, {0x2b6190u, "CEventBGM::StartDoramock", nullptr},
        {0x2b6220u, "CEventBGM::Stop", nullptr}, {0x2e4660u, "CSkCtrl::PlayMidi", nullptr},
        {0x2e46e0u, "CSkCtrl::StopMidi", nullptr}, {0x2e47a0u, "CSkCtrl::StopMidiBank", nullptr},
        {0x2e4600u, "CSkCtrl::StopAll", nullptr}, {0x2e4630u, "CSkCtrl::SoundOff", nullptr},
        {0x2e47e0u, "CSkCtrl::SoundOffMidiBank", nullptr}, {0x1acdc0u, "SuperStarCheer::FadeStop", nullptr},
        {0x1acd40u, "SuperStarCheer::PlayByIndex", nullptr}};

    template <int I>
    void cheerLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const AdxtHook &h = g_cheerHooks[I];
        auto reg = [ctx](int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); };
        std::fprintf(stderr, "[cheer] %s a0=%x a1=%x a2=%x ra=%x stage=%u%c", h.name, reg(4), reg(5), reg(6), reg(31),
                     rdram[0x4922C0u], 10);
        h.orig(rdram, ctx, runtime);
    }

    template <int... I>
    void installCheerLog(PS2Runtime &runtime, std::integer_sequence<int, I...>)
    {
        ((g_cheerHooks[I].orig = runtime.lookupFunction(g_cheerHooks[I].addr),
          g_cheerHooks[I].orig && runtime.replaceFunction(g_cheerHooks[I].addr, cheerLogged<I>)), ...);
    }

    void returnValue(R5900Context *ctx, int32_t value)
    {
        ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }

    #include "hctp_media_routes.inl"
    #include "hctp_media_host.inl"
    #include "hctp_media_defaults.inl"

    // ---- Native ADX music (HCTP_NATIVE_ADX=1) ----------------------------------------------------------------------
    // ADXT_StartAfs(adxt, partition, file) for the BGM / CHEER / BG_ENV partitions is played on the host: the ADX file
    // is read from the .afs, decoded here (CRI ADX v3/v4, unencrypted) and played on its own raylib stream. The ADXT
    // handle stays stopped inside the game's library (no EE decode, no IOP rna/sj traffic); GetStat answers PLAYING /
    // PLAYEND for it, Stop / Destroy end it, SetOutVol sets its volume. SE (StartMemIdx) and movies (StartSj) unchanged.
    // Partition ids = the order of the .afs names in the ELF (0-4 BGM000-004, 10 PBP000, 11 CHEER, 12 BG_ENV).
    const char *adxPartitionFile(uint32_t partition)
    {
        static const char *names[] = {"BGM/BGM000.afs", "BGM/BGM001.afs", "BGM/BGM002.afs", "BGM/BGM003.afs",
                                      "BGM/BGM004.afs", nullptr, nullptr, nullptr, nullptr, nullptr,
                                      "BGM/PBP000.afs", "BGM/CHEER.afs", "BGM/BG_ENV.afs",
                                      "BGM/PBPSVR.afs", // 13 = host-only SvR commentary pack (never used by the game)
                                      "BGM/FINKEL.afs", // 14 = host-only Howard Finkel pack (tools/build_finkel_pack.py)
                                      "BGM/PBPBETA.afs", // 15 = host-only SvR public beta commentary (Jul 2004 pbppac.afs)
                                      "BGM/PBPEXTRA.afs"}; // 16 = host-only extra lines (SYM / SvR 2006 / 2K14)
        return partition < sizeof(names) / sizeof(names[0]) ? names[partition] : nullptr;
    }

    uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

    struct AdxPlayer
    {
        std::mutex m;
        uint32_t adxt = 0u; // the game's handle (0 = free)
        bool playing = false, ended = false;
        std::vector<uint8_t> file;
        uint32_t dataOff = 0u, channels = 0u, rate = 0u, total = 0u;
        int32_t c1 = 0, c2 = 0;
        bool loop = false;
        uint32_t loopStart = 0u, loopEnd = 0u;
        uint64_t pos = 0u;       // next sample index
        uint32_t blkIndex = ~0u; // decoded frame (32 samples) in blk
        int16_t blk[2][32] = {};
        int32_t hist[2][2] = {};
        int32_t loopHist[2][2] = {};
        bool loopHistSaved = false;
        float gain = 1.0f;
        AudioStream stream{};
        bool streamOpen = false;
        uint32_t streamRate = 0u, streamChannels = 0u;
    };
    constexpr int kAdxPlayers = 8;
    AdxPlayer g_adxPlayers[kAdxPlayers];
    std::mutex g_adxAlloc;
    bool g_nativeAdxLog = false;

    void adxDecodeFrame(AdxPlayer &pl, uint32_t frame)
    {
        if (pl.loop && !pl.loopHistSaved && frame == pl.loopStart / 32u)
        {
            std::memcpy(pl.loopHist, pl.hist, sizeof(pl.hist));
            pl.loopHistSaved = true;
        }
        for (uint32_t c = 0; c < pl.channels; ++c)
        {
            const size_t off = pl.dataOff + (size_t(frame) * pl.channels + c) * 18u;
            int32_t h1 = pl.hist[c][0], h2 = pl.hist[c][1];
            if (off + 18u > pl.file.size())
            {
                std::memset(pl.blk[c], 0, sizeof(pl.blk[c]));
                continue;
            }
            const uint8_t *b = pl.file.data() + off;
            const int32_t scale = (be16(b) & 0x1FFF) + 1;
            for (int k = 0; k < 32; ++k)
            {
                const uint8_t byte = b[2 + k / 2];
                int32_t nib = (k & 1) ? (byte & 0xF) : (byte >> 4);
                if (nib & 8)
                    nib -= 16;
                int32_t smp = nib * scale + ((pl.c1 * h1 + pl.c2 * h2) >> 12);
                smp = std::clamp(smp, -32768, 32767);
                h2 = h1;
                h1 = smp;
                pl.blk[c][k] = static_cast<int16_t>(smp);
            }
            pl.hist[c][0] = h1;
            pl.hist[c][1] = h2;
        }
        pl.blkIndex = frame;
    }

    void adxFill(AdxPlayer &pl, void *buffer, unsigned int frames)
    {
        // raylib calls this from its audio thread WITH its audio lock held, while the game thread holds pl.m when it
        // (re)opens a stream (Load/Unload/PlayAudioStream take that audio lock): blocking here deadlocked the game
        // (freezes at Finkel's entrance line / commentary after a finisher - a clip at another sample rate reopens
        // the stream). Never wait: a busy player gives one buffer of silence.
        std::unique_lock<std::mutex> lock(pl.m, std::try_to_lock);
        int16_t *out = static_cast<int16_t *>(buffer);
        const uint32_t ch = pl.streamChannels ? pl.streamChannels : 2u;
        if (!lock.owns_lock())
        {
            std::memset(out, 0, static_cast<size_t>(frames) * ch * sizeof(int16_t));
            return;
        }
        for (unsigned int i = 0; i < frames; ++i)
        {
            if (pl.playing && !pl.ended && pl.channels != 0u && pl.loop && pl.loopEnd > pl.loopStart &&
                pl.pos >= pl.loopEnd && pl.loopHistSaved)
            {
                // back to the loop start: re-decode its frame from the history saved when it was first decoded
                pl.pos = pl.loopStart;
                std::memcpy(pl.hist, pl.loopHist, sizeof(pl.hist));
                adxDecodeFrame(pl, static_cast<uint32_t>(pl.pos / 32u));
            }
            if (pl.playing && !pl.ended && pl.pos >= pl.total)
            {
                pl.ended = true;
                if (g_nativeAdxLog)
                    std::fprintf(stderr, "[nadx] adxt=%x ended%c", pl.adxt, 10);
            }
            if (!pl.playing || pl.ended || pl.channels == 0u)
            {
                for (uint32_t c = 0; c < ch; ++c)
                    out[i * ch + c] = 0;
                continue;
            }
            const uint32_t frame = static_cast<uint32_t>(pl.pos / 32u);
            if (frame != pl.blkIndex)
                adxDecodeFrame(pl, frame);
            const uint32_t k = static_cast<uint32_t>(pl.pos % 32u);
            for (uint32_t c = 0; c < ch; ++c)
            {
                int32_t v = static_cast<int32_t>(pl.blk[std::min(c, pl.channels - 1u)][k] * pl.gain);
                if (pl.gain > 1.0f && (v > 24576 || v < -24576)) // soft knee for boosted commentary (no hard clipping)
                    v = v > 0 ? 24576 + (v - 24576) / 4 : -24576 + (v + 24576) / 4;
                out[i * ch + c] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
            }
            ++pl.pos;
        }
    }

    template <int I>
    void adxStreamCallback(void *buffer, unsigned int frames) { adxFill(g_adxPlayers[I], buffer, frames); }
    using AdxCb = void (*)(void *, unsigned int);
    const AdxCb g_adxCallbacks[kAdxPlayers] = {adxStreamCallback<0>, adxStreamCallback<1>, adxStreamCallback<2>, adxStreamCallback<3>,
                                               adxStreamCallback<4>, adxStreamCallback<5>, adxStreamCallback<6>, adxStreamCallback<7>};

    AdxPlayer *adxFind(uint32_t adxt)
    {
        for (auto &pl : g_adxPlayers)
            if (pl.adxt == adxt && adxt != 0u)
                return &pl;
        return nullptr;
    }

    bool adxSetup(AdxPlayer &pl, std::vector<uint8_t> &&data, const char *label);

    // RMS of the whole loaded clip (channel 0), same decoder as adxDecodeFrame - for per-clip commentary gain
    double adxClipRms(const AdxPlayer &pl)
    {
        int32_t h1 = 0, h2 = 0;
        double sum = 0.0;
        uint64_t n = 0;
        for (uint32_t frame = 0; frame * 32u < pl.total; ++frame)
        {
            const size_t off = pl.dataOff + size_t(frame) * pl.channels * 18u;
            if (off + 18u > pl.file.size())
                break;
            const uint8_t *b = pl.file.data() + off;
            const int32_t scale = (be16(b) & 0x1FFF) + 1;
            for (int k = 0; k < 32; ++k)
            {
                int32_t nib = (k & 1) ? (b[2 + k / 2] & 0xF) : (b[2 + k / 2] >> 4);
                if (nib & 8)
                    nib -= 16;
                const int32_t smp = std::clamp(nib * scale + ((pl.c1 * h1 + pl.c2 * h2) >> 12), -32768, 32767);
                h2 = h1;
                h1 = smp;
                sum += double(smp) * smp;
                ++n;
            }
        }
        return n ? std::sqrt(sum / double(n)) : 0.0;
    }

    bool adxLoad(AdxPlayer &pl, uint32_t partition, uint32_t fid)
    {
        if (const std::string *imported = hostMediaPath('M', partition, fid))
        {
            std::ifstream file(*imported, std::ios::binary | std::ios::ate);
            const auto length = file ? file.tellg() : std::streampos(-1);
            if (length < 0x40 || length > 0x8000000) return false;
            std::vector<uint8_t> data(static_cast<size_t>(length));
            file.seekg(0);
            if (!file.read(reinterpret_cast<char *>(data.data()), data.size())) return false;
            return adxSetup(pl, std::move(data), imported->c_str());
        }
        const char *path = adxPartitionFile(partition);
        if (!path)
            return false;
        FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        uint8_t head[8];
        uint32_t entry[2] = {};
        bool ok = std::fread(head, 1, 8, f) == 8 && std::memcmp(head, "AFS", 3) == 0;
        uint32_t count = 0u;
        if (ok)
            std::memcpy(&count, head + 4, 4);
        ok = ok && fid < count && std::fseek(f, 8 + long(fid) * 8, SEEK_SET) == 0 && std::fread(entry, 4, 2, f) == 2;
        std::vector<uint8_t> data;
        if (ok && entry[1] > 0x40u && entry[1] < 0x10000000u)
        {
            data.resize(entry[1]);
            ok = std::fseek(f, long(entry[0]), SEEK_SET) == 0 && std::fread(data.data(), 1, data.size(), f) == data.size();
        }
        else
            ok = false;
        std::fclose(f);
        if (!ok)
            return false;
        char label[96];
        std::snprintf(label, sizeof(label), "%s #%u", path, fid);
        return adxSetup(pl, std::move(data), label);
    }

    // an ADX file in memory -> the player (header parsed, decoder reset); false when it is not plain ADX v3/v4
    bool adxSetup(AdxPlayer &pl, std::vector<uint8_t> &&data, const char *label)
    {
        if (data.size() < 0x40u || be16(data.data()) != 0x8000u || data[4] != 3u || data[5] != 18u || data[6] != 4u || data[0x13] >= 8u)
        {
            std::fprintf(stderr, "[nadx] cannot play %s (not plain ADX)%c", label, 10);
            return false;
        }
        pl.file = std::move(data);
        const uint8_t *d = pl.file.data();
        pl.dataOff = be16(d + 2) + 4u;
        pl.channels = std::clamp<uint32_t>(d[7], 1u, 2u);
        pl.rate = be32(d + 8);
        pl.total = be32(d + 12);
        const double hp = be16(d + 16);
        const double a = std::sqrt(2.0) - std::cos(2.0 * 3.14159265358979 * hp / pl.rate);
        const double b = std::sqrt(2.0) - 1.0;
        const double c = (a - std::sqrt((a + b) * (a - b))) / b;
        pl.c1 = static_cast<int32_t>(c * 2.0 * 4096.0);
        pl.c2 = static_cast<int32_t>(-(c * c) * 4096.0);
        const uint32_t loopBase = d[0x12] == 4u ? 0x24u : (d[0x12] == 3u ? 0x18u : 0u);
        pl.loop = loopBase && pl.dataOff >= loopBase + 0x14u && be32(d + loopBase) != 0u;
        pl.loopStart = pl.loop ? be32(d + loopBase + 4u) : 0u;
        pl.loopEnd = pl.loop ? std::min(be32(d + loopBase + 12u), pl.total) : 0u;
        pl.pos = 0u;
        pl.blkIndex = ~0u;
        std::memset(pl.hist, 0, sizeof(pl.hist));
        pl.loopHistSaved = false;
        pl.ended = false;
        pl.gain = 1.0f;
        if (g_nativeAdxLog)
            std::fprintf(stderr, "[nadx] load %s: %u ch %u Hz %u samples loop=%d %u-%u%c", label, pl.channels,
                         pl.rate, pl.total, pl.loop ? 1 : 0, pl.loopStart, pl.loopEnd, 10);
        return true;
    }

    uint8_t *g_pbpRdram = nullptr; // guest RAM, set by the commentary hooks
    // stop the commentary line now and give its player back (match / demo teardown): the commentary never keeps
    // a native ADX player once it is not speaking
    void hctpNativeStopCommentaryImpl()
    {
        std::lock_guard<std::mutex> alloc(g_adxAlloc);
        for (int i = 0; i < kAdxPlayers; ++i)
        {
            AdxPlayer &pl = g_adxPlayers[i];
            if (pl.adxt < 0xC0000000u)
                continue;
            std::lock_guard<std::mutex> lock(pl.m);
            pl.playing = false;
            pl.adxt = 0u;
        }
    }

    bool hctpNativePlayAfsClipImpl(uint32_t partition, uint32_t fid)
    {
        static uint32_t nextHandle = 0xC0000000u;
        std::lock_guard<std::mutex> alloc(g_adxAlloc);
        // commentary keeps ONE player: reuse the one holding a commentary handle (lines are queued back to back by the
        // engine), else take a free one - never more, so music/crowd players are never starved
        int own = -1;
        for (int i = 0; i < kAdxPlayers && own < 0; ++i)
            if (g_adxPlayers[i].adxt >= 0xC0000000u)
                own = i;
        for (int i = 0; i < kAdxPlayers; ++i)
        {
            AdxPlayer &pl = g_adxPlayers[i];
            if (own >= 0 ? i != own : pl.adxt != 0u)
                continue;
            std::lock_guard<std::mutex> lock(pl.m);
            if (!adxLoad(pl, partition, fid))
                return false;
            if (!pl.streamOpen || pl.streamRate != pl.rate || pl.streamChannels != pl.channels)
            {
                if (pl.streamOpen)
                    UnloadAudioStream(pl.stream);
                pl.stream = LoadAudioStream(pl.rate, 16, pl.channels);
                SetAudioStreamCallback(pl.stream, g_adxCallbacks[i]);
                pl.streamRate = pl.rate;
                pl.streamChannels = pl.channels;
                pl.streamOpen = true;
                PlayAudioStream(pl.stream);
            }
            // commentary sits under music + crowd: its own gain (HCTP_PBP_VOLUME, default 2.5; samples are clamped)
            static const float kPbpGain = [] {
                const char *v = std::getenv("HCTP_PBP_VOLUME");
                return v ? float(std::atof(v)) : 2.5f;
            }();
            // Howard Finkel (partition 14) a bit quieter than the commentary (Jordan): HCTP_FINKEL_VOLUME, default 1.6
            static const float kFinkelGain = [] {
                const char *v = std::getenv("HCTP_FINKEL_VOLUME");
                return v ? float(std::atof(v)) : 1.2f; // 1.6 was still a bit loud (Jordan, Oct 20)
            }();
            // COMMENTARY VOLUME option (restored, In-Game Options): save byte g_SaveData+0x4550a+0x1b, stored
            // inverted (0 = MAX = the gains above, 1 NORMAL, 2 MIN, 3 OFF) so old / fresh saves default to MAX
            float optScale = 1.0f;
            if (g_pbpRdram)
            {
                static const float kLevel[4] = {1.0f, 0.65f, 0.35f, 0.0f};
                optScale = kLevel[g_pbpRdram[0x4fb95au + 0x1bu] & 3u];
            }
            // the SvR public beta clips (partition 15, most Cole/Tazz lines) are mastered ~9 dB quieter than retail and
            // the other packs (measured median -25 vs -16 dBFS): x2.8 matched it, but Jordan found Cole/Tazz a bit loud -> x2.0
            pl.gain = (partition == 14u ? kFinkelGain : partition == 15u ? kPbpGain * 2.0f : kPbpGain) * optScale;
            // some Cole/Tazz clips are mastered far quieter than the rest (Jordan: Booker T vs Big Show): lift any
            // commentary clip whose level (after the partition gain) is under the retail median -16 dBFS (RMS ~5190),
            // at most x2.5 more. Loud clips are left alone. Finkel (14) has his own level.
            if (partition != 14u && pl.gain > 0.0f)
            {
                const double rms = adxClipRms(pl) * (pl.gain / (kPbpGain * optScale));
                if (rms > 50.0 && rms < 5190.0)
                {
                    const float lift = std::min(2.5f, float(5190.0 / rms));
                    pl.gain *= lift;
                    static unsigned s_quietLogs = 0;
                    if (s_quietLogs++ < 40)
                        std::fprintf(stderr, "[hctp-pbp] clip %u/%u quiet (rms %.0f): gain x%.2f%c", partition, fid, rms, lift, 10);
                }
            }
            // the game's Options sound block (CSound* at 0x526768, CSound::GetOption = +4; byte 1 = BGM volume,
            // byte 3 = SFX volume): log it when it changes, to find the COMMENTARY VOLUME byte
            {
                static uint8_t s_lastOpt[12] = {0xFF};
                uint32_t sound = 0;
                if (g_pbpRdram)
                    std::memcpy(&sound, g_pbpRdram + 0x526768u, 4);
                if (g_pbpRdram && sound >= 0x100000u && sound < 0x1FFFFF0u)
                {
                    uint8_t opt[12];
                    std::memcpy(opt, g_pbpRdram + sound + 4u, 12);
                    if (std::memcmp(opt, s_lastOpt, 12) != 0)
                    {
                        std::memcpy(s_lastOpt, opt, 12);
                        std::fprintf(stderr, "[hctp-pbp] sound options:");
                        for (uint8_t b : opt)
                            std::fprintf(stderr, " %02X", b);
                        std::fprintf(stderr, "%c", 10);
                    }
                }
            }
            pl.adxt = nextHandle++;
            pl.playing = true;
            std::fprintf(stderr, "[hctp-pbp] native clip %u/%u started on player %d%c", partition, fid, i, 10);
            return true;
        }
        return false;
    }

    PS2Runtime::RecompiledFunction g_origAdxtStartAfs, g_origAdxtGetStat, g_origAdxtStop, g_origAdxtDestroy, g_origAdxtSetOutVol;
    uint32_t regU(R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); }

    void adxStopNative(uint32_t adxt)
    {
        std::lock_guard<std::mutex> alloc(g_adxAlloc);
        if (AdxPlayer *pl = adxFind(adxt))
        {
            std::lock_guard<std::mutex> lock(pl->m);
            pl->playing = false;
            pl->adxt = 0u;
            pl->file.clear();
            pl->file.shrink_to_fit();
            if (g_nativeAdxLog)
                std::fprintf(stderr, "[nadx] stop adxt=%x%c", adxt, 10);
        }
    }


    // start player i on adxt (its stream opened at the file's rate / channels); called with g_adxAlloc + pl.m held
    void adxPlayOn(int i, uint32_t adxt)
    {
        AdxPlayer &pl = g_adxPlayers[i];
        if (!pl.streamOpen || pl.streamRate != pl.rate || pl.streamChannels != pl.channels)
        {
            if (pl.streamOpen)
                UnloadAudioStream(pl.stream);
            pl.stream = LoadAudioStream(pl.rate, 16, pl.channels);
            SetAudioStreamCallback(pl.stream, g_adxCallbacks[i]);
            pl.streamRate = pl.rate;
            pl.streamChannels = pl.channels;
            pl.streamOpen = true;
            PlayAudioStream(pl.stream);
        }
        pl.adxt = adxt;
        pl.playing = true;
    }

    PS2Runtime::RecompiledFunction g_origAdxtStartMem, g_origAdxtStartMemIdx;

    // the ADX bytes an in-memory start names: StartMem (adxt, buf, size) / StartMemIdx (adxt, acx, index) - an ACX
    // archive: BE u32 0, BE u32 count, then BE (offset, size) pairs relative to the archive
    bool adxFromGuest(uint8_t *rdram, uint32_t addr, uint32_t size, std::vector<uint8_t> &out)
    {
        const uint32_t a = addr & 0x1FFFFFFu;
        if (size < 0x40u || size > 0x1000000u || a + size > 0x2000000u)
            return false;
        out.assign(rdram + a, rdram + a + size);
        return true;
    }

    // ADXT_Stop(adxt) as the hook's tail call: it returns to the hook's caller itself (or pauses there cleanly)
    void adxTailStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t adxt, uint32_t ra)
    {
        ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(adxt));
        ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(ra));
        g_origAdxtStop(rdram, ctx, runtime);
    }

    void nativeStartMemCommon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool indexed)
    {
        const uint32_t adxt = regU(ctx, 4), a1 = regU(ctx, 5), a2 = regU(ctx, 6);
        const uint32_t ra = regU(ctx, 31);
        std::vector<uint8_t> data;
        bool ok = false;
        if (indexed)
        {
            const uint32_t base = a1 & 0x1FFFFFFu;
            if (base + 8u <= 0x2000000u)
            {
                const uint32_t count = be32(rdram + base + 4u);
                if (be32(rdram + base) == 0u && a2 < count && count < 4096u && base + 8u + (a2 + 1u) * 8u <= 0x2000000u)
                {
                    const uint32_t off = be32(rdram + base + 8u + a2 * 8u), size = be32(rdram + base + 12u + a2 * 8u);
                    ok = adxFromGuest(rdram, a1 + off, size, data);
                }
            }
        }
        else
            ok = adxFromGuest(rdram, a1, a2, data);
        if (ok && be16(data.data()) == 0x8000u)
        {
            adxStopNative(adxt);
            // Native player first, the library's ADXT_Stop LAST as a tail call: Stop can pause in SVM_Lock ->
            // ResumeThread (thread switch); code after it would run on a half-unwound stack (six-man freeze at 0x5260A0).
            std::unique_lock<std::mutex> alloc(g_adxAlloc);
            for (int i = 0; i < kAdxPlayers; ++i)
            {
                AdxPlayer &pl = g_adxPlayers[i];
                if (pl.adxt != 0u)
                    continue;
                std::unique_lock<std::mutex> lock(pl.m);
                char label[64];
                std::snprintf(label, sizeof(label), "%s %08x[%u]", indexed ? "StartMemIdx" : "StartMem", a1, indexed ? a2 : 0u);
                if (!adxSetup(pl, std::move(data), label))
                    break;
                adxPlayOn(i, adxt);
                if (g_nativeAdxLog)
                    std::fprintf(stderr, "[nadx] %s adxt=%x -> player %d%c", label, adxt, i, 10);
                lock.unlock();
                alloc.unlock();
                adxTailStop(rdram, ctx, runtime, adxt, ra);
                return;
            }
            // no free player / not plain ADX: the library plays it (arguments untouched)
        }
        (indexed ? g_origAdxtStartMemIdx : g_origAdxtStartMem)(rdram, ctx, runtime);
    }
    void nativeStartMem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { nativeStartMemCommon(rdram, ctx, runtime, false); }
    void nativeStartMemIdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { nativeStartMemCommon(rdram, ctx, runtime, true); }

    void nativeStartAfs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t adxt = regU(ctx, 4), partition = regU(ctx, 5), fid = regU(ctx, 6);
        const uint32_t ra = regU(ctx, 31);
        if (adxPartitionFile(partition))
        {
            adxStopNative(adxt);
            // native player first; the library's ADXT_Stop (no older stream may keep playing on this handle) runs
            // LAST as a tail call - it can pause in SVM_Lock -> ResumeThread (see nativeStartMemCommon)
            std::unique_lock<std::mutex> alloc(g_adxAlloc);
            for (int i = 0; i < kAdxPlayers; ++i)
            {
                AdxPlayer &pl = g_adxPlayers[i];
                if (pl.adxt != 0u)
                    continue;
                std::unique_lock<std::mutex> lock(pl.m);
                if (!adxLoad(pl, partition, fid))
                    break;
                adxPlayOn(i, adxt);
                if (g_nativeAdxLog)
                    std::fprintf(stderr, "[nadx] StartAfs adxt=%x pt=%u fid=%u -> player %d%c", adxt, partition, fid, i, 10);
                lock.unlock();
                alloc.unlock();
                adxTailStop(rdram, ctx, runtime, adxt, ra);
                return;
            }
            std::fprintf(stderr, "[nadx] StartAfs adxt=%x pt=%u fid=%u: not played natively%c", adxt, partition, fid, 10);
        }
        g_origAdxtStartAfs(rdram, ctx, runtime);
    }

    void nativeGetStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> alloc(g_adxAlloc);
            if (AdxPlayer *pl = adxFind(regU(ctx, 4)))
            {
                std::lock_guard<std::mutex> lock(pl->m);
                returnValue(ctx, pl->ended ? 5 : 3); // ADXT_STAT_PLAYEND / PLAYING
                return;
            }
        }
        g_origAdxtGetStat(rdram, ctx, runtime);
    }

    void nativeStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        adxStopNative(regU(ctx, 4));
        g_origAdxtStop(rdram, ctx, runtime);
    }

    void nativeDestroy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        adxStopNative(regU(ctx, 4));
        g_origAdxtDestroy(rdram, ctx, runtime);
    }

    void nativeSetOutVol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> alloc(g_adxAlloc);
            if (AdxPlayer *pl = adxFind(regU(ctx, 4)))
            {
                const int32_t vol = static_cast<int32_t>(regU(ctx, 5)); // 0.1 dB, 0 = full, -999 = silent
                std::lock_guard<std::mutex> lock(pl->m);
                pl->gain = vol <= -960 ? 0.0f : std::pow(10.0f, static_cast<float>(vol) / 200.0f);
            }
        }
        g_origAdxtSetOutVol(rdram, ctx, runtime);
    }

    // Commentary: every bell goes through the referee's CAcxPlayer (s_pInstance__14CRefereePlayer at
    // 0x526730) as PlayByIndex(ref, 0x10, 0) - GameSE_gong is only one of its callers. Match start/end for the commentary engine.
    PS2Runtime::RecompiledFunction g_origAcxPlayByIndex = nullptr;
    void commentaryAcxPlayByIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_pbpRdram = rdram;
        const uint32_t obj = regU(ctx, 4), index = regU(ctx, 5);
        uint32_t referee = 0;
        std::memcpy(&referee, rdram + 0x526730u, 4);
        g_origAcxPlayByIndex(rdram, ctx, runtime);
        if (obj == 0u || obj != referee)
            return;
        std::fprintf(stderr, "[hctp-pbp] referee sound %u%c", index, 10);
        if (index == 0x10u)
        {
            // who is in the match: entrant cast codes (u16 at 0x4921CA + slot * 0x20, roster id * 100)
            // all 8 cast slots (six-man / Rumble / Chamber fill more than 4; the old 4-slot loop stopped at the first
            // empty slot and left the 3-v-3 partners out of the superstar banter and the all-women check)
            uint32_t ids[8];
            uint32_t n = 0;
            for (uint32_t slot = 0; slot < 8u; ++slot)
            {
                if (!(hctpRuleWrestlerSlotMask(rdram, rdram[0x4920C0u]) & (1u << slot)))
                    continue;
                uint16_t code = 0;
                std::memcpy(&code, rdram + 0x4921CAu + slot * 0x20u, 2);
                if (code >= 25500u) // empty slot (cast code 0 is The Rock)
                    continue;
                const uint32_t id = hctpRosterCodeId(rdram, code);
                if (id < 0x56u || (id >= 0xA2u && id < 0x100u))
                    ids[n++] = code < 10000u ? id | (code % 100u) << 8 : id; // + attire variant (nWo Hogan)
            }
            const int8_t stage = int8_t(rdram[0x4922C0u]); // GetNowStage 0x1f53d0 (arena / PPV)
            // rule byte too: cage / Hell in a Cell / Elimination Chamber have no entrances, so the bell is the only
            // place their match type is seen (GetRule 0x1f53f0 -> 0x4920C0, first byte)
            std::fprintf(stderr, "[hctp-pbp] arena (stage) %d, match rule 0x%02X%c", int(stage), rdram[0x4920C0u], 10);
            // season mode picks the team by the player's brand, not the arena (Jordan). GetMode 0x188ae0
            // = *(*(0x526628) + 4) in a match. Brand = the Raw/SmackDown roster editor's byte
            // (CRosterFrame::SetProgram 0x349a80 writes 0/1 to GetCharaGameProp 0x12cec0 record +0x161; regular
            // records 0x4C0F54 + id*0x170). Which value is SmackDown is unverified: SvR uses 1 = SmackDown;
            // HCTP_PBP_BRAND_SD=0 flips it. (IsSmackDownEntry's +0x163 != 6 is roster membership, not the brand.)
            uint32_t ctrl = 0, mode = 0xFFFFFFFFu;
            std::memcpy(&ctrl, rdram + 0x526628u, 4);
            if (ctrl != 0u && ctrl < 0x2000000u)
                std::memcpy(&mode, rdram + ctrl + 4u, 4);
            static const int sdValue = [] {
                const char *e = std::getenv("HCTP_PBP_BRAND_SD");
                return e && e[0] == '0' ? 0 : 1;
            }();
            int sd = 0, raw = 0;
            std::fprintf(stderr, "[hctp-pbp] game mode %d, brand bytes (+0x161):", int(mode));
            for (uint32_t i = 0; i < n; ++i)
            {
                const int8_t b = int8_t(rdram[0x4C0F54u + (ids[i] & 0xFFu) * 0x170u + 0x161u]);
                std::fprintf(stderr, " 0x%02X=%d", ids[i] & 0xFFu, int(b));
                if (b == sdValue)
                    ++sd;
                else if (b == 1 - sdValue)
                    ++raw;
            }
            std::fprintf(stderr, "%c", 10);
            const int ppvBrand = ppvCommentaryBrand(); // item 18: PPV card BRAND row (RAW 0 / SMACKDOWN 1) wins
            if (ppvBrand >= 0)
                std::fprintf(stderr, "[hctp-pbp] PPV brand %d overrides the roster majority%c", ppvBrand, 10);
            hctpCommentaryBrand(ppvBrand >= 0 ? ppvBrand : sd > raw ? 1 : raw > sd ? 0 : -1);
            hctpCommentaryArena(stage);
            hctpCommentaryParticipants(ids, n);
            g_entranceSnapFresh = false; // [COMMFIX Nov 6] the next entrance without a setup = a preview (CAM)
            hctpCommentaryBellRule(rdram[0x4920C0u], rdram[0x4920E7u]); // B&P = rule +0x27 (MakeKindBit2 0x1d37b0)
            hctpCommentaryEvent(1u);
        } // kPbpBell: match start / match end (debounced in hctp_commentary.cpp)
    }

    // Match / demo teardown -> commentary stops at once (Jordan: Cole kept talking over the black screen after a
    // demo skip). Every exit path calls the original first, then stops the commentary.
    PS2Runtime::RecompiledFunction g_origReleaseGameLink = nullptr, g_origDemoDtor = nullptr, g_origPauseExit = nullptr,
                                   g_origExMatchEnd = nullptr;
    void commentaryTeardown(const char *why)
    {
        std::fprintf(stderr, "[hctp-pbp] teardown (%s): commentary stopped%c", why, 10);
        hctpCommentaryEvent(5u); // kPbpStop
        hctpNativeStopCommentaryImpl();
    }
    void commentaryReleaseGameLink(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origReleaseGameLink(rdram, ctx, runtime);
        commentaryTeardown("ReleaseGameLinkObj");
    }
    void commentaryDemoDtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origDemoDtor(rdram, ctx, runtime);
        commentaryTeardown("title demo end");
    }
    void commentaryPauseExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origPauseExit(rdram, ctx, runtime);
        commentaryTeardown("pause exit");
    }
    void commentaryExMatchEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origExMatchEnd(rdram, ctx, runtime);
        commentaryTeardown("exhibition match end");
    }

    // Koutai(CChar*, CChar*) = tag partners exchange places -> "the tag's been made" line.
    PS2Runtime::RecompiledFunction g_origKoutai = nullptr;
    void commentaryKoutai(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origKoutai(rdram, ctx, runtime);
        std::fprintf(stderr, "[hctp-pbp] tag exchange%c", 10);
        hctpCommentaryEvent(2u); // kPbpTag
    }

    // Entrances: CSd5EventSequence::SetEvent(this) sets up event #(this+0x64) of type (this+0x38; 2 = entrance) while
    // the loading screen is still up; CEventSystem::Play starts it once loaded. The entrant's cast code is the u16 at
    // 0x4921CA + order * 0x20 (CSd5EventData::GetEntranceEventNoFromSystem): roster id * 100 (+ variant), or
    // 10000 + (id - 0x56) * 100 for the created wrestlers. Finkel speaks when the entrance starts playing.
    PS2Runtime::RecompiledFunction g_origSetEvent = nullptr, g_origEventPlay = nullptr;
    std::atomic<int> g_pendingOrder{-1};
    // the match setup block (rule 0x4920C0, cast team bytes 0x4921C9 + slot*0x20) is valid while SetEntranceEventNo asks
    // GetEntranceEventNoFromSystem per cast order, but reads 0 by the time the entrance events play (log Nov 2: entrance #0
    // "rule 0x00 -> singles", bell "rule 0x04") -> cache them there with the cast codes
    uint8_t g_entranceRule = 0xFFu;
    uint32_t g_announcedSlots = 0u; // cast slots already introduced since entrance #0 was set up (bit per slot)
    std::array<int8_t, 8> g_entranceTeam = {-1, -1, -1, -1, -1, -1, -1, -1};
    std::array<uint16_t, 8> g_entranceCast = {25500u, 25500u, 25500u, 25500u,
                                               25500u, 25500u, 25500u, 25500u};
    // [COMMFIX Nov 6] true from the match-setup snapshot (GetEntranceEventNoFromSystem) until the opening bell.
    // Create-a-Moveset entrance previews never run that setup -> the snapshot was the LAST MATCH's cast (Jordan Nov 6:
    // A-Train = The Rock, Al Snow/Animal/Batista/Big Show silent = 'already introduced this show' from the six-man).
    // g_entranceSnapFresh declared earlier ([COMMFIX Nov 6] build fix)
    // [/COMMFIX]
    void commentarySetEvent(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_pbpRdram = rdram;
        const uint32_t self = regU(ctx, 4);
        uint32_t order = 0, type = 0;
        std::memcpy(&order, rdram + ((self + 0x64u) & 0x1FFFFFFu), 4);
        std::memcpy(&type, rdram + ((self + 0x38u) & 0x1FFFFFFu), 4);
        g_origSetEvent(rdram, ctx, runtime);
        if (type == 2u && order < 8u)
            std::fprintf(stderr, "[hctp-pbp] entrance #%u set up%c", order, 10);
        if (type == 2u && order == 0u)
        {
            hctpCommentaryResetEntrances();
            g_announcedSlots = 0u;
        }
        g_pendingOrder = (type == 2u && order < 8u) ? int(order) : -1;
    }
    void commentaryEventPlay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origEventPlay(rdram, ctx, runtime);
        // ActionWaitGimmickReady calls Play every frame while the loading screen is up; it returns 1 only on the
        // call that really starts the entrance (state 1 -> 2). Speak then, never on the waiting calls.
        if (regU(ctx, 2) != 1u)
            return;
        const int order = g_pendingOrder.exchange(-1);
        if (order < 0)
            return;
        // [COMMFIX Nov 6] no match setup since the last bell = an entrance preview (Create-a-Moveset): the wrestler on
        // screen is CChar slot 0 (*(0x526EA0)+0x30 cast code, as hctpRumbleSpawn reads it); live cast slot = fallback.
        // Both logged so Jordan's CAM run shows which one is right. One wrestler, no match rule/title, its own show.
        if (!g_entranceSnapFresh)
        {
            uint32_t chara = 0;
            uint16_t charCode = 0xFFFFu, liveCode = 0xFFFFu;
            std::memcpy(&chara, rdram + 0x526EA0u, 4);
            if (chara >= 0x100000u && chara < 0x2000000u)
                std::memcpy(&charCode, rdram + chara + 0x30u, 2);
            std::memcpy(&liveCode, rdram + 0x4921CAu + uint32_t(order) * 0x20u, 2);
            const uint16_t code = charCode < 10000u ? charCode : liveCode;
            std::fprintf(stderr, "[hctp-pbp] preview entrance #%d (no match setup): CChar 0x%X code %u, live slot code %u -> %u%c",
                         order, chara, charCode, liveCode, code, 10);
            if (code < 10000u)
                hctpCommentaryPreviewEntrance(hctpRosterCodeId(rdram, code) | (code % 100u) << 8);
            return;
        }
        // [/COMMFIX]
        // Some events clear the live cast table before Play. GetEntranceEventNoFromSystem sees the authoritative
        // value earlier, so use that captured value when the live slot has already become empty.
        // every entrance (was #0 only, before the setup snapshot was complete -> "rule 0x00 -> singles" all match)
        {
            // count distinct wrestlers only: the referee's cast (Hebner 5100 / Chioda 4900) sits in the table too,
            // which made every singles match a "tag team contest" (log: participants 0x20 0x12C 0xA33)
            uint32_t present = 0;
            std::array<uint32_t, 8> seen{};
            for (uint32_t i = 0; i < 8u; ++i)
            {
                uint16_t candidate = g_entranceCast[i]; // setup snapshot first: the live table changes during entrances
                if (candidate >= 25500u)
                    std::memcpy(&candidate, rdram + 0x4921CAu + i * 0x20u, 2);
                if (candidate >= 25500u)
                    continue;
                const uint32_t id = candidate >= 10000u ? 0x100u + (candidate - 10000u) / 100u : candidate / 100u;
                if (!(hctpRuleWrestlerSlotMask(rdram, g_entranceRule != 0xFFu ? g_entranceRule : rdram[0x4920C0u]) & (1u << i)))
                    continue;
                if (id == 0x31u || id == 0x33u || std::find(seen.begin(), seen.begin() + present, id) != seen.begin() + present)
                    continue;
                seen[present++] = id;
            }
            // tag vs multi-man comes from the game's own rule table (0x45AF60 + rule*0x20: +1 cast count, +0xC 6 x
            // {slot, team, role}; role 0/1 = wrestler, 2 = special referee, 3 = lumberjack). Counting wrestlers made
            // every 3+ man match (triple threat, fatal 4-way, battle royal, lumberjack) a "tag team contest".
            const uint8_t ruleType = g_entranceRule != 0xFFu ? g_entranceRule : rdram[0x4920C0u];
            bool tag = present > 2u;
            uint32_t teamSize = present >= 6u ? 3u : 2u;
            g_entranceSlotMask = 0xFFu;
            if (ruleType < 0x56u)
            {
                const uint8_t *const rt = rdram + 0x45AF60u + uint32_t(ruleType) * 0x20u;
                uint32_t sizes[6] = {};
                for (uint32_t k = 0; k < rt[1] && k < 6u; ++k)
                    if (rt[0xC + 3 * k + 2] < 2u && rt[0xC + 3 * k + 1] < 6u)
                        ++sizes[rt[0xC + 3 * k + 1]];
                uint32_t maxTeam = 0, minTeam = 99;
                for (uint32_t s : sizes)
                    if (s)
                        maxTeam = std::max(maxTeam, s), minTeam = std::min(minTeam, s);
                tag = maxTeam >= 2u && minTeam == maxTeam; // equal teams of 2+; handicap = singles-style intros
                g_entranceSlotMask = 0;
                for (uint32_t k = 0; k < rt[1] && k < 6u; ++k)
                    if (rt[0xC + 3 * k + 2] < 2u && rt[0xC + 3 * k] < 8u)
                        g_entranceSlotMask |= 1u << rt[0xC + 3 * k];
                teamSize = maxTeam >= 3u ? 3u : 2u;
            }
            // the cast's own team bytes (what SetEntranceEventNo groups by) beat the rule table when they are known
            {
                uint32_t sizes[8] = {}, known = 0;
                for (uint32_t i = 0; i < 8u; ++i)
                {
                    const uint16_t c = g_entranceCast[i];
                    if (!(g_entranceSlotMask & (1u << i)))
                        continue;
                    const uint32_t id = c >= 10000u ? 0x100u : c / 100u;
                    if (c >= 25500u || id == 0x31u || id == 0x33u || g_entranceTeam[i] < 0 || g_entranceTeam[i] >= 8)
                        continue;
                    ++sizes[g_entranceTeam[i]], ++known;
                }
                uint32_t maxTeam = 0, minTeam = 99;
                for (uint32_t sz : sizes)
                    if (sz)
                        maxTeam = std::max(maxTeam, sz), minTeam = std::min(minTeam, sz);
                if (known >= 2u)
                {
                    tag = maxTeam >= 2u && minTeam == maxTeam;
                    teamSize = maxTeam >= 3u ? 3u : 2u;
                }
                std::fprintf(stderr, "[hctp-pbp] entrance teams from cast: %u wrestlers, team sizes max %u min %u%c", known, maxTeam,
                             minTeam == 99 ? 0 : minTeam, 10);
            }
            std::fprintf(stderr, "[hctp-pbp] entrance wrestlers %u rule 0x%02X -> %s%c", present, ruleType, tag ? "tag" : "singles", 10);
            hctpCommentarySetTagMatch(tag ? 1u : 0u);
            hctpCommentarySetTeamSize(teamSize);
            // GetRule() returns the live HCTP match-rule object at 0x4920C0;
            // its first byte is the eRuleType used by the game's rule helpers.
            hctpCommentarySetMatchRule(g_entranceRule != 0xFFu ? g_entranceRule : rdram[0x4920C0u]);
        }
        // Entrance #order is NOT cast slot #order: CSd5EventData::SetEntranceEventNo 0x2ace70 walks the cast team by
        // team (+0x109 team byte) and appends each slot to data+0x728[count]; SetPreEvent 0x2a8360 reads
        // *(0x52A0B8)+0x728+order*4 back. The cast table is interleaved (A,B,A,B..) so 6-man named the wrong men.
        uint32_t slot = uint32_t(order);
        {
            uint32_t data = 0, mapped = 0xFFFFFFFFu;
            std::memcpy(&data, rdram + 0x52A0B8u, 4);
            if (data != 0u && data < 0x2000000u)
                std::memcpy(&mapped, rdram + data + 0x728u + uint32_t(order) * 4u, 4);
            if (mapped < 8u)
                slot = mapped;
            // title-match probe: CSd5EventData::GetTitle 0x2af090 returns slot record (+slot*0xD4) +0x8C as the
            // entrance title graphic when it is not 0xFF (else the belt the wrestler holds). A title match should set
            // it (challenger / belt at stake) -> wire SYM's title-match time tables 12-27 from this once logged.
            uint32_t title8c = 0xFFFFFFFFu;
            if (data != 0u && data < 0x2000000u)
                std::memcpy(&title8c, rdram + data + slot * 0xD4u + 0x8Cu, 4);
            std::fprintf(stderr, "[hctp-pbp] entrance #%d -> cast slot %u (event data 0x%X, title+0x8C 0x%X)%c", order, slot,
                         data, title8c, 10);
        }
        // the setup snapshot (GetEntranceEventNoFromSystem hook) first: by entrance time the live table has been
        // rewritten (Nov 4 six-man log: Benjamin's slot 3 read Cena's code at Cena's entrance -> Cena announced twice)
        uint16_t code = slot < 8u ? g_entranceCast[slot] : 25500u;
        if (code >= 25500u)
        {
            std::memcpy(&code, rdram + 0x4921CAu + slot * 0x20u, 2);
            std::fprintf(stderr, "[hctp-pbp] entrance #%d: no snapshot, live cast slot -> %u%c", order, code, 10);
        }
        if (code >= 25500u && order > 0)
        {
            // Some entrance events leave their own slot empty. Pick the first present cast that is not already
            // announced instead of blindly reusing slot 0 (which duplicated Bubba for the Dudleys).
            for (uint32_t i = 0; i < 8u && code >= 25500u; ++i)
            {
                uint16_t candidate = 0;
                std::memcpy(&candidate, rdram + 0x4921CAu + i * 0x20u, 2);
                bool already = false;
                for (int j = 0; j < order; ++j)
                {
                    uint16_t prior = 0;
                    std::memcpy(&prior, rdram + 0x4921CAu + uint32_t(j) * 0x20u, 2);
                    already = already || (candidate == prior);
                }
                if (candidate < 25500u && !already)
                    code = candidate;
            }
            std::fprintf(stderr, "[hctp-pbp] entrance #%d: resolved cast slot -> %u%c", order, code, 10);
        }
        if (code >= 25500u)
        {
            std::fprintf(stderr, "[hctp-pbp] entrance #%d starts: cast code %u = empty slot, silent%c", order, code, 10);
            return;
        }
        // belt held (CTitleHistoryData::CheckTitleHold 0x324110): u16 holder ids at GetSaveData() 0x4B6450 + 0x9EFC,
        // 11 slots for 9 belts (belts 2 and 3 = tag titles, two holders each) -> belt + 1 above bit 16 for Finkel
        auto rosterOf = [&](uint16_t c, int &belt) {
            const int r = c >= 10000u ? int((c - 10000u) / 100u + 0x56u) : int(hctpRosterCodeId(rdram, c) | (c % 100u) << 8);
            static const uint8_t kSlotBelt[11] = {0, 1, 2, 2, 3, 3, 4, 5, 6, 7, 8};
            belt = -1;
            for (uint32_t s = 0; s < 11u && belt < 0; ++s)
            {
                uint16_t holder = 0;
                std::memcpy(&holder, rdram + 0x4C034Cu + s * 2u, 2);
                if (holder == uint16_t(r & 0xFF))
                    belt = kSlotBelt[s];
            }
            return r;
        };
        int belt = -1;
        const int roster = rosterOf(code, belt);
        std::fprintf(stderr, "[hctp-pbp] entrance #%d starts: cast code %u -> roster 0x%02X attire %d belt %d%c", order, code,
                     roster & 0xFF, (roster >> 8) & 0xFF, belt, 10);
        // SvR ring announcer (SLUS_210.60 0x211c10): one entrance = every cast member walking out in it; 2+ members =
        // tag-team category -> a known team gets ONE team call, else each member's intro back to back. HCTP: a shared
        // (stable) entrance gives the partners no event of their own (SetEntranceEventNo 0x2ace70), so their slots
        // are missing from data+0x728[0 .. data+0x84); they share this slot's team byte (0x4920C0 + slot*0x20 + 0x109).
        uint32_t ids[8];
        int belts[8];
        uint32_t n = 0;
        ids[n] = uint32_t(roster), belts[n++] = belt;
        if (slot < 8u)
            g_announcedSlots |= 1u << slot;
        {
            uint32_t data = 0, count = 0;
            std::memcpy(&data, rdram + 0x52A0B8u, 4);
            if (data != 0u && data < 0x2000000u)
                std::memcpy(&count, rdram + data + 0x84u, 4);
            const auto teamOf = [&](uint32_t i) {
                return g_entranceTeam[i] >= 0 ? g_entranceTeam[i] : int8_t(rdram[0x4921C9u + i * 0x20u]);
            };
            const int8_t myTeam = teamOf(slot);
            // only this match's real wrestlers (rule table slots): leftover cast slots from earlier matches and
            // default partners are not in the match (Jordan Nov 5: Jericho -> 'Christian', Piper -> 'Sean O'Haire')
            for (uint32_t i = 0; i < 8u && count != 0u && count <= 8u; ++i)
            {
                uint16_t c = g_entranceCast[i]; // setup snapshot; the live slot is rewritten by entrance time
                if (c >= 25500u)
                    std::memcpy(&c, rdram + 0x4921CAu + i * 0x20u, 2);
                if (!((g_entranceSlotMask >> i) & 1u) || i == slot || c >= 25500u || c == code || teamOf(i) != myTeam || (g_announcedSlots >> i) & 1u)
                    continue; // (a partner who already walked out in an earlier entrance is not walking out again)
                bool ownEntrance = false;
                for (uint32_t k = 0; k < count && !ownEntrance; ++k)
                {
                    uint32_t sl = 0;
                    std::memcpy(&sl, rdram + data + 0x728u + k * 4u, 4);
                    ownEntrance = sl == i;
                }
                if (ownEntrance)
                    continue;
                int b = -1;
                g_announcedSlots |= 1u << i;
                ids[n] = uint32_t(rosterOf(c, b)), belts[n++] = b;
                std::fprintf(stderr, "[hctp-pbp] entrance #%d: cast slot %u (roster 0x%02X) walks out in this entrance%c", order,
                             i, ids[n - 1] & 0xFF, 10);
            }
        }
        // tag belts (2 WWE Tag, 3 World Tag): both holders in this one entrance -> the team call ("They are the WWE Tag
        // Team Champions!") on the last member (bit 24); a holder whose partner has his own entrance -> "one half of"
        bool together = false;
        for (uint32_t a = 0; a < n; ++a)
            for (uint32_t b = a + 1; b < n; ++b)
                together = together || ((belts[a] == 2 || belts[a] == 3) && belts[a] == belts[b]);
        for (uint32_t k = 0; k < n; ++k)
        {
            if (belts[k] >= 0)
                ids[k] |= uint32_t(belts[k] + 1) << 16;
            if (together && (belts[k] == 2 || belts[k] == 3))
                ids[k] |= 1u << 24; // no "one half of" for this member; lib plays the team call after the group
        }
        // title match = Season only: CSeasonSystem::nvGameInit 0x2579e0 copies the card's belt (CheckTitleMatch 0x30fdd0:
        // save + 0x9C8C + match*0xD0 + 4, 0xFF = none; ConvertBeltNo2ScriptNo 0x33b460) into match setup 0x4922C5.
        // Exhibition never writes that byte -> also require the season event struct *(0x529400) (log both to confirm).
        {
            uint32_t season = 0;
            std::memcpy(&season, rdram + 0x529400u, 4);
            const uint32_t stake = rdram[0x4922C5u];
            const uint32_t titleBelt = season != 0u && stake < 9u ? stake : 0xFFu;
            std::fprintf(stderr, "[hctp-pbp] title check: 0x4922C5=0x%02X season*0x529400=0x%X -> belt %d%c", stake, season,
                         titleBelt == 0xFFu ? -1 : int(titleBelt), 10);
            hctpCommentarySetTitleMatch(titleBelt);
        }
        hctpCommentaryEntranceGroup(ids, n);
    }

    // GameSE(CChar*, se, ...) plays a wrestler's move sound: CChar+0xA4 = the move being performed (the id SvR's
    // finisher tables use) -> finisher / signature move calls.
    PS2Runtime::RecompiledFunction g_origGameSe = nullptr;
    void commentaryGameSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t chara = regU(ctx, 4);
        if (chara >= 0x100000u && chara < 0x2000000u)
        {
            uint32_t moveId = 0;
            std::memcpy(&moveId, rdram + chara + 0xA4u, 4);
            // who did it: CChar+0x30 = the wrestler's cast code (roster id * 100 + attire; 10000+ = created), found by
            // probing (Triple H 201, Undertaker 301, Brock 4401, referee 5100) -> owner-only move lines
            uint16_t cast = 0;
            std::memcpy(&cast, rdram + chara + 0x30u, 2);
            if (cast < 25500u)
                hctpCommentaryCharaRoster(chara, hctpRosterCodeId(rdram, cast));
            hctpCommentaryMove(chara, moveId);
        }
        g_origGameSe(rdram, ctx, runtime);
    }

    void installNativeAdx(PS2Runtime &runtime)
    {
        g_nativeAdxLog = std::getenv("HCTP_ADXTLOG") != nullptr;
        g_origAdxtStartAfs = runtime.lookupFunction(0x3bf668u);
        g_origAdxtGetStat = runtime.lookupFunction(0x3be0a0u);
        g_origAdxtStop = runtime.lookupFunction(0x3bdfe0u);
        g_origAdxtDestroy = runtime.lookupFunction(0x3bdab8u);
        g_origAdxtSetOutVol = runtime.lookupFunction(0x3be960u);
        if (!g_origAdxtStartAfs || !g_origAdxtGetStat || !g_origAdxtStop || !g_origAdxtDestroy || !g_origAdxtSetOutVol)
        {
            std::cerr << "[nadx] ADXT functions not found - native ADX off" << std::endl;
            return;
        }
        runtime.replaceFunction(0x3bf668u, nativeStartAfs);
        runtime.replaceFunction(0x3be0a0u, nativeGetStat);
        runtime.replaceFunction(0x3bdfe0u, nativeStop);
        runtime.replaceFunction(0x3bdab8u, nativeDestroy);
        runtime.replaceFunction(0x3be960u, nativeSetOutVol);
        // in-memory ADX (sound effects): HCTP_NATIVE_ADX_MEM=0 leaves them to the library / IOP
        g_origAdxtStartMem = runtime.lookupFunction(0x3bf800u);
        g_origAdxtStartMemIdx = runtime.lookupFunction(0x3bf8f8u);
        const char *mem = std::getenv("HCTP_NATIVE_ADX_MEM");
        if (g_origAdxtStartMem && g_origAdxtStartMemIdx && !(mem && mem[0] == '0'))
        {
            runtime.replaceFunction(0x3bf800u, nativeStartMem);
            runtime.replaceFunction(0x3bf8f8u, nativeStartMemIdx);
            std::cerr << "[nadx] native in-memory ADX (StartMem / StartMemIdx) on" << std::endl;
        }
        if ((g_origAcxPlayByIndex = runtime.lookupFunction(0x1b1090u)) != nullptr)
            runtime.replaceFunction(0x1b1090u, commentaryAcxPlayByIndex);
        if ((g_origKoutai = runtime.lookupFunction(0x1e4080u)) != nullptr)
            runtime.replaceFunction(0x1e4080u, commentaryKoutai);
        if ((g_origSetEvent = runtime.lookupFunction(0x2a83b0u)) != nullptr &&
            (g_origEventPlay = runtime.lookupFunction(0x166e80u)) != nullptr)
        {
            runtime.replaceFunction(0x2a83b0u, commentarySetEvent);
            runtime.replaceFunction(0x166e80u, commentaryEventPlay);
        }
        if ((g_origGameSe = runtime.lookupFunction(0x1b0680u)) != nullptr)
            runtime.replaceFunction(0x1b0680u, commentaryGameSe);
        if ((g_origReleaseGameLink = runtime.lookupFunction(0x202840u)) != nullptr)
            runtime.replaceFunction(0x202840u, commentaryReleaseGameLink);
        if ((g_origDemoDtor = runtime.lookupFunction(0x136ff0u)) != nullptr)
            runtime.replaceFunction(0x136ff0u, commentaryDemoDtor);
        if ((g_origPauseExit = runtime.lookupFunction(0x2a24d0u)) != nullptr)
            runtime.replaceFunction(0x2a24d0u, commentaryPauseExit);
        if ((g_origExMatchEnd = runtime.lookupFunction(0x132450u)) != nullptr)
            runtime.replaceFunction(0x132450u, commentaryExMatchEnd);
        std::cerr << "[hctp-pbp] bell hook " << (g_origAcxPlayByIndex ? "on" : "NOT found") << std::endl;
        std::cerr << "[nadx] native ADX music on" << std::endl;
    }

    void menuMovieCanUse(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnValue(ctx, 1); }    // MenuMovie::CanUse 0x23ddb0
    void moviePlayerGetStat(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnValue(ctx, 2); } // CMoviePlayer::GetStat 0x1b0ac0 = READY

    // newlib reentrant allocator (_malloc_r 0x124448, _calloc_r 0x121ae0, _free_r 0x123570, _realloc_r 0x126c20) ->
    // the runtime guest heap, like malloc/free (bound in hctp.toml). newlib's own sbrk arena had no room after the
    // ymem heap grab -> _Balloc NULL -> strtod looped forever in _lshift (sscanf %f after pac\m.pac, Startup). a0=reent.
    void libcAllocFailed(const char *what, uint32_t size)
    {
        static int count = 0;
        if (count++ < 20)
            std::cerr << "[heap] libc " << what << " FAILED size 0x" << std::hex << size << std::dec << std::endl;
    }
    void libcMallocR(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t size = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const uint32_t addr = runtime->guestMalloc(size ? size : 1u);
        if (!addr) libcAllocFailed("_malloc_r", size);
        returnValue(ctx, static_cast<int32_t>(addr));
    }
    void libcCallocR(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const uint32_t size = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
        const uint32_t addr = (n && size) ? runtime->guestCalloc(n, size) : runtime->guestCalloc(1u, 1u);
        if (!addr) libcAllocFailed("_calloc_r", n * size);
        returnValue(ctx, static_cast<int32_t>(addr));
    }
    void libcFreeR(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->guestFree(static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])));
        returnValue(ctx, 0);
    }
    void libcReallocR(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t old = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const uint32_t size = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
        const uint32_t addr = old ? runtime->guestRealloc(old, size) : runtime->guestMalloc(size ? size : 1u);
        if (!addr && size) libcAllocFailed("_realloc_r", size);
        returnValue(ctx, static_cast<int32_t>(addr));
    }

    // sceCdLayerSearchFile 0x11d810 (dual-layer lookup: same file/path args + a layer number) -> the runtime's
    // sceCdSearchFile (bound at 0x11db28), as the good-base fork did. Running libcdvd's own code here never found the
    // file and the game retried forever on the first "Now Loading" screen.
    void cdLayerSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->lookupFunction(0x11db28u)(rdram, ctx, runtime);
    }

    // Loose pac sub-files (HCTP_NO_LOOSE_PAC=1 disables): CFile::Search 0x1482f0 maps "/TAG/KEY" (e.g. "/EVET/3600",
    // "/ENTR/SEVP") to a FILE_INFO {u32 sector, u32 size/0x100, u32 m_EntryFiles index, char path[]} at m_FilePos
    // 0x524DE0; the read opens m_EntryFiles 0x523DE0 + index*0x20 (128 x 0x20 names) with ADXF_Open and reads from that
    // sector. A host file MODS\PAC\<TAG>\<KEY> in the game folder replaces (or adds) that sub-file: it gets its own
    // m_EntryFiles slot (from 127 down) and sector 0, so evtEnt.pac and the other pacs stay stock. Extra entrances =
    // MODS\PAC\EVET\36xx (+ 37xx stage-plane variant) plus a MODS\PAC\ENTR\SEVP that carries their script rows.
    std::map<std::string, std::pair<uint32_t, int>> g_loosePac; // "/TAG/KEY" -> (size bytes, entry slot or -1)
    int g_loosePacNextSlot = 127;
    std::deque<int> g_loosePacSlotOrder; // slots in hand-out order (oldest first) for reuse when 127..32 are taken
    PS2Runtime::RecompiledFunction g_origCFileSearch = nullptr;
    void scanLoosePac()
    {
        std::error_code ec;
        for (const auto &dir : std::filesystem::directory_iterator("MODS/PAC", ec))
        {
            if (!dir.is_directory(ec))
                continue;
            for (const auto &f : std::filesystem::directory_iterator(dir.path(), ec))
            {
                std::string tag = dir.path().filename().string(), key = f.path().filename().string();
                if (!f.is_regular_file(ec) || tag.size() > 4 || key.size() > 4 || f.file_size(ec) == 0)
                    continue;
                for (char &c : tag) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                g_loosePac["/" + tag + "/" + key] = {static_cast<uint32_t>(f.file_size(ec)), -1};
            }
        }
        // Nov 7/8: HCTP_VICTORY_EVTK add-on lists MODS/PAC/MOVK/<k>. Nov 10 entrance port: MOVK/LIST (CAM entrance list,
        // tools\stage_entrance_port.py) is served in EVERY run; WAZA stays opt-in.
        for (const char *k : {"WAZA", "LIST"})
            if ((std::getenv("HCTP_VICTORY_EVTK") != nullptr || std::strcmp(k, "LIST") == 0) &&
                std::filesystem::exists(std::string("MODS/PAC/MOVK/") + k, ec))
                g_loosePac[std::string("/MOVE/") + k] = {
                    static_cast<uint32_t>(std::filesystem::file_size(std::string("MODS/PAC/MOVK/") + k, ec)), -1};
    }
    // ===== SvR ENTRANCE READER (Nov 7, Jordan: HCTP must READ original SvR entrances, no converted files) =====
    // Original SvR04 pacs dropped as-is in <game>\MODS\SVR04\ (evtEnt.pac, bg.pac). "/TAG/KEY" sub-files the reader
    // registers (EVET banks of SvR events under their virtual HCTP event numbers, SvR props under free SOBJ ids) are
    // served straight from those files: FILE_INFO points at the pac's own sector, nothing is copied or converted.
    struct SvrPacFile { std::string host, name; int slot = -1; };
    std::vector<SvrPacFile> g_svrPacFiles;                         // 0 = evtEnt.pac, 1 = bg.pac
    struct SvrPacRef { int file; uint32_t sector, size100; };      // sector = absolute 0x800 sector in that pac
    std::map<std::string, SvrPacRef> g_svrPac;                     // "/EVET/3850" -> SvR evtEnt.pac EVET 3077
    bool svrPacServe(uint8_t *rdram, R5900Context *ctx, const char *path, uint32_t pathAddr)
    {
        auto it = g_svrPac.find(path);
        if (it == g_svrPac.end())
            return false;
        SvrPacFile &f = g_svrPacFiles[static_cast<size_t>(it->second.file)];
        char *const names = reinterpret_cast<char *>(rdram + 0x523DE0u);
        if (f.slot < 0)
        {
            const std::string stock(names, strnlen(names, 0x20));
            const size_t sepAt = stock.find_first_of("\\/");
            const char sep = sepAt == std::string::npos ? '\\' : stock[sepAt];
            const bool device = sepAt != std::string::npos && (sepAt == 0 || stock[sepAt - 1] == ':');
            std::string name = (device ? stock.substr(0, sepAt + 1) : std::string()) + f.name +
                               (stock.find(";1") != std::string::npos ? ";1" : "");
            for (char &c : name)
                if (c == '/' || c == '\\') c = sep;
            int slot = g_loosePacNextSlot;
            if (name.size() >= 0x20 || slot < 32 || names[slot * 0x20] != 0)
            {
                std::fprintf(stderr, "[svrent] %s: no entry slot for %s%c", path, name.c_str(), 10);
                return false;
            }
            --g_loosePacNextSlot; // permanent (never handed to the loose-slot reuse queue)
            std::memcpy(names + slot * 0x20, name.c_str(), name.size() + 1);
            f.slot = slot;
            std::fprintf(stderr, "[svrent] %s = entry %d%c", name.c_str(), slot, 10);
        }
        uint8_t *const info = rdram + 0x524DE0u;
        const uint32_t slot = static_cast<uint32_t>(f.slot);
        std::memcpy(info, &it->second.sector, 4);
        std::memcpy(info + 4, &it->second.size100, 4);
        std::memcpy(info + 8, &slot, 4);
        std::memcpy(info + 12, rdram + pathAddr, strnlen(reinterpret_cast<char *>(rdram + pathAddr), 0x3F) + 1);
        static int logged = 0;
        if (logged++ < 40)
            std::fprintf(stderr, "[svrent] %s -> %s sector %u (%u x 0x100)%c", path, f.name.c_str(), it->second.sector,
                         it->second.size100, 10);
        returnValue(ctx, 0x524DE0);
        return true;
    }
    uint8_t g_evtkSavedStage = 0;
    bool g_evtkStagePending = false; // restore 0x4922C0 at the scene's own /STG/ search (loads run after the dtor)
    void cfileSearchLoose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pathAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        char path[33] = {};
        for (int i = 0; i < 32 && rdram[pathAddr + i]; ++i)
            path[i] = static_cast<char>(std::toupper(rdram[pathAddr + i]));
        if (g_hoganTearBody != 0 && std::strcmp(path, "/EMD2/3200") == 0)
            std::snprintf(path, sizeof(path), "/EMD2/%04u", unsigned(g_hoganTearBody));
        // Nov 7 Legend Hogan: loose SEVP rows 3078/3278 objs 0+6 now use the literal 7804 (his own retail entrance body; no redirect)
        // (was 7800 = his ring code: serving the entrance body under the ring name poisoned the match model cache).
        if (g_evtkStagePending && std::strncmp(path, "/STG/", 5) == 0)
        {
            g_evtkStagePending = false; // Nov 9 evtk: the scene's set path is built; give CAM its stage byte back
            std::fprintf(stderr, "[victory-evtk] %s: stage byte %u -> %u%c", path, rdram[0x4922C0u], g_evtkSavedStage, 10);
            rdram[0x4922C0u] = g_evtkSavedStage;
        }
        if (svrPacServe(rdram, ctx, path, pathAddr))
            return;
        auto it = g_loosePac.find(path);
        static const bool logAll = std::getenv("HCTP_FILELOG") != nullptr; // TEMP diag Nov 4: every CFile::Search path
        if (logAll)
            std::fprintf(stderr, "[filelog] Search %s ra=%x%c", path, static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])), 10);
        if (it == g_loosePac.end())
            return g_origCFileSearch(rdram, ctx, runtime);
        char *const names = reinterpret_cast<char *>(rdram + 0x523DE0u);
        if (it->second.second < 0)
        {
            // Name in the stock entries' style (device prefix up to the first separator, ";1" suffix if they use it).
            const std::string stock(names, strnlen(names, 0x20));
            const size_t sepAt = stock.find_first_of("\\/");
            const char sep = sepAt == std::string::npos ? '\\' : stock[sepAt];
            const bool device = sepAt != std::string::npos && (sepAt == 0 || stock[sepAt - 1] == ':'); // "\" / "cdrom0:\"
            std::string name = (device ? stock.substr(0, sepAt + 1) : std::string()) + "MODS" + sep +
                               "PAC" + (path + 0) + (stock.find(";1") != std::string::npos ? ";1" : "");
            if ((std::strcmp(path, "/MOVE/LIST") == 0 || (std::strcmp(path, "/MOVE/WAZA") == 0 &&
                                                         std::getenv("HCTP_VICTORY_EVTK") != nullptr)) &&
                std::filesystem::exists(std::string("MODS/PAC/MOVK/") + (path + 6)))
                name.replace(name.find("MOVE"), 4, "MOVK"); // Nov 7: HCTP_VICTORY_EVTK add-on victory list
            for (char &c : name)
                if (c == '/' || c == '\\') c = sep;
            int slot = g_loosePacNextSlot;
            if (name.size() < 0x20 && (slot < 32 || names[slot * 0x20] != 0) && !g_loosePacSlotOrder.empty())
            {
                // Slots 127..32 used up (the Content Pack has ~130 loose files and restored models need theirs): reuse
                // the slot handed out longest ago; its file is re-assigned a slot when the game asks for it again.
                slot = g_loosePacSlotOrder.front();
                g_loosePacSlotOrder.pop_front();
                for (auto &e : g_loosePac)
                    if (e.second.second == slot)
                        e.second.second = -1;
                std::fprintf(stderr, "[loosepac] entry slots full: reusing entry %d%c", slot, 10);
            }
            else if (name.size() < 0x20 && slot >= 32 && names[slot * 0x20] == 0)
                --g_loosePacNextSlot;
            else
                slot = -1;
            if (slot < 0)
            {
                std::fprintf(stderr, "[loosepac] %s: no free entry slot (name %s), stock file used%c", path, name.c_str(), 10);
                g_loosePac.erase(it);
                return g_origCFileSearch(rdram, ctx, runtime);
            }
            std::memcpy(names + slot * 0x20, name.c_str(), name.size() + 1);
            it->second.second = slot;
            g_loosePacSlotOrder.push_back(slot);
            std::fprintf(stderr, "[loosepac] %s -> %s (%u bytes, entry %d)%c", path, name.c_str(), it->second.first, slot, 10);
        }
        uint8_t *const info = rdram + 0x524DE0u;
        const uint32_t sector = 0, size = (it->second.first + 0xFFu) >> 8, slot = static_cast<uint32_t>(it->second.second);
        std::memcpy(info, &sector, 4);
        std::memcpy(info + 4, &size, 4);
        std::memcpy(info + 8, &slot, 4);
        std::memcpy(info + 12, rdram + pathAddr, strnlen(reinterpret_cast<char *>(rdram + pathAddr), 0x3F) + 1);
        returnValue(ctx, 0x524DE0);
    }

    // Per-attire entrances: MODS\PAC\entrances.txt lines "<roster id> <attire digit> <event>" (decimal, '#' comments).
    // CSd5EventData::GetEntranceEventNoFromSystem 0x2ad1b0 (a1 = cast order) takes the event from roster record +0xEC
    // (0x4C0F54 + id*0x170), +200 (event < 3400) / +100 on stage-plane arenas; the cast code at 0x4921CA + order*0x20
    // = id*100 + attire*10. A listed (id, attire) gets its own event (extras: 3600-3699, plane variant +100).
    std::map<uint32_t, uint32_t> g_attireEntrance; // id*10 + attire -> event
    int rosterxEntranceForModel(uint32_t model); // roster-expansion slot entrance (0 = none), defined below
    PS2Runtime::RecompiledFunction g_origGetEntranceEventNo = nullptr;
    void getEntranceEventNoPerAttire(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        const uint32_t order = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        g_origGetEntranceEventNo(rdram, ctx, runtime);
        if (order >= 8)
            return;
        // called from CSd5EventData::SetEntranceEventNo 0x2ace70, which reads GetRule() 0x1f53f0 (= 0x4920C0) and the
        // cast table (+0x10A code, +0x109 team) right then -> snapshot the whole setup block, not just this order
        if (order == 0)
        {
            g_entranceCast.fill(25500u);
            g_entranceTeam.fill(-1);
        }
        g_entranceRule = rdram[0x4920C0u];
        for (uint32_t i = 0; i < 8u; ++i)
        {
            uint16_t c; std::memcpy(&c, rdram + 0x4921CAu + i * 0x20u, 2);
            if (c < 25500u)
                g_entranceCast[i] = c, g_entranceTeam[i] = int8_t(rdram[0x4921C9u + i * 0x20u]);
        }
        g_entranceSnapFresh = true; // [COMMFIX Nov 6]
        std::fprintf(stderr, "[hctp-pbp] entrance setup snapshot (order %u): rule 0x%02X teams %d %d %d %d %d %d%c", order,
                     g_entranceRule, g_entranceTeam[0], g_entranceTeam[1], g_entranceTeam[2], g_entranceTeam[3],
                     g_entranceTeam[4], g_entranceTeam[5], 10);
        uint32_t season; std::memcpy(&season, rdram + self + order * 0xD4u + 0x14Cu, 4);
        uint16_t code; std::memcpy(&code, rdram + 0x4921CAu + order * 0x20u, 2);
        if (season != 0 || code >= 10000u)
            return;
        // Nov 7: slot code (model 86..99 * 100, < 10000 = never a CAW code) -> slot entrance. The GetCharaGameProp
        // redirect refuses id 0x56+ while that CAW block is used, so the stock path read the CAW's event (3071).
        // Stage plane = IsStagePlane 0x2af240 (arena byte 0x4922C0 in A,B,E,F,12,13,30,31,34,35): +200 (<3400) / +100.
        if (const int slotEv = rosterxEntranceForModel(code / 100u); slotEv > 0)
        {
            const int8_t arena = static_cast<int8_t>(rdram[0x4922C0u]);
            static const int8_t kPlane[] = {0xA, 0xB, 0xE, 0xF, 0x12, 0x13, 0x30, 0x31, 0x34, 0x35};
            const bool plane = std::find(std::begin(kPlane), std::end(kPlane), arena) != std::end(kPlane);
            const uint32_t ev = static_cast<uint32_t>(slotEv) + (plane ? (slotEv < 3400 ? 200u : 100u) : 0u);
            std::fprintf(stderr, "[entrance] slot cast %u: event %d -> %u (arena 0x%X)%c", code,
                         static_cast<int>(_mm_cvtsi128_si32(ctx->r[2])), ev, unsigned(uint8_t(arena)), 10);
            SET_GPR_U32(ctx, 2, ev);
            return;
        }
        const uint32_t id = code / 100u, attire = (code / 10u) % 10u;
        auto it = g_attireEntrance.find(id * 10u + attire);
        if (it == g_attireEntrance.end())
            return;
        int16_t own; std::memcpy(&own, rdram + 0x4C0F54u + id * 0x170u + 0xECu, 2);
        const int32_t got = static_cast<int32_t>(_mm_cvtsi128_si32(ctx->r[2]));
        const int32_t delta = got - own; // 0 = normal, 100/200 = stage plane, else a special case (left alone)
        if (delta != 0 && delta != 100 && delta != 200)
            return;
        const uint32_t ev = it->second + (delta ? (it->second < 3400u ? 200u : 100u) : 0u);
        std::fprintf(stderr, "[entrance] cast %u id 0x%x attire %u: event %d -> %u%c", code, id, attire, got, ev, 10);
        SET_GPR_U32(ctx, 2, ev);
    }
    // Victory motions: a Create-a-Moveset / match victory list number n (moves.pac MOVE/LIST sub-entry 7) becomes
    // doramock event 6200 + table[n] (u8 x 86 at 0x421830) in ConvertDoramockFromMoves 0x39dba0 (evtDora.pac, EVDR).
    // Cut wrestlers' entries point at the shared 59 (6259) although many own events are still in evtDora.
    // MODS\PAC\victory.txt "<n> <event 6000-6455>" re-points entries; HCTP_VICTORY_TEST=<event> plays that event for
    // every victory preview (to identify the unreferenced 62xx / 63xx events in CAM).
    std::map<uint32_t, uint32_t> g_victoryEvent; // n -> event
    PS2Runtime::RecompiledFunction g_origConvertDoramock = nullptr;
    // HCTP_VICTORY_EVTK=1 (Nov 7, opt-in): victory preview of an evtTkme scene 4000-4999. drDoramock::SetDoramockEvent
    // 0x39c0c0 builds CSd5EventMng(type 3 = DORA/EVDR); the ctor 0x2a7a20 itself maps type 1 + event (common+0x56,
    // common = *0x529400) >= 4000 to type 4 (/TKME/SEVP + /EVTK/n). So for a pending 4xxx doramock number (0x58CCF8,
    // SetMovesDoramock 0x39d180) the wrapper passes type 1 and writes the number at common+0x56. Log tag [victory-evtk].
    // Nov 8: menu ids >= 0x1C34 skip ConvertDoramockFromMoves (MovesNext 0x342704 treats them as WAZA moves) and reach
    // drDoramock::SetMovesDoramock 0x39d180 (a0 -> 0x58CCF8) as the raw id -> /EVDR/<garbage> hang. Any id that
    // victory.txt / victory_evtk.txt maps (appended list entries) is replaced by its event here.
    PS2Runtime::RecompiledFunction g_origSetMovesDoramock = nullptr;
    void setMovesDoramockMapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // MovesNext 0x342710 calls this with a0 = 0, a1 = WAZA id (stored at 0x58CD00; SetDoramockEvent then builds id*3).
        const uint32_t a0 = uint32_t(_mm_cvtsi128_si32(ctx->r[4]));
        const uint32_t a1 = uint32_t(_mm_cvtsi128_si32(ctx->r[5]));
        auto it = (a0 == 0u && a1 >= 0x1C34u) ? g_victoryEvent.find(a1) : g_victoryEvent.end();
        if (it != g_victoryEvent.end())
        {
            SET_GPR_U32(ctx, 4, it->second);
            SET_GPR_U32(ctx, 5, 0u);
            std::fprintf(stderr, "[victory] list id %u (>= 0x1C34) -> event %u%c", a1, it->second, 10);
        }
        g_origSetMovesDoramock(rdram, ctx, runtime);
    }
    // Nov 9 (HCTP_VICTORY_EVTK): cast + stage + skip for TKME 4000s / DORA 6050-6055 in CAM. MODS/PAC/evtk_sevp.txt
    // (tools/sevp_table.py) = "<event> <SEVP stageNo> <code>:<type hex> ..." (type 0x1000000 = wrestler).
    // InitEventSystem 0x2ad340 (called later by CSd5EventSequence::SetEventDoramock/SetPreEvent, NOT by the ctor) counts
    // casts as nonzero bytes common+0xC9..+0xD0 and sets the event stage from (s8)*0x4922C0 (func_167320). So the lent
    // common block stays alive and is lent again around InitEventSystem with stage byte = SEVP stage. Fixed SEVP codes
    // (>= 100 = roster id*100) go into setup casts 0x4921CA+i*0x20 (restored at the dtor 0x2a7c80).
    // Skip: ActionEvent 0x2a9c90 only runs IsEntranceEventSkip 0x2ab390 (pad *0x4FFA00 +4 & 0x800) for seq+0x38 2/3;
    // while START is held a type-1/4 sequence is shown to it as type 3 (the CAM doramock skip path).
    struct EvtkRow { int stage = 0; std::vector<std::pair<uint32_t, uint32_t>> chars; };
    std::map<uint32_t, EvtkRow> g_evtkRows;
    uint32_t g_evtkBlk = 0, g_evtkEvent = 0;
    bool g_evtkCageDone = false; // Nov 9 Chamber cage spawned for this scene
    uint16_t g_evtkSavedCasts[8] = {};
    void evtkLoadTable()
    {
        std::ifstream f("MODS/PAC/evtk_sevp.txt");
        for (std::string line; std::getline(f, line);)
        {
            std::istringstream is(line);
            uint32_t ev = 0; EvtkRow r;
            if (!(is >> ev >> r.stage)) continue;
            for (std::string tok; is >> tok;)
            {
                unsigned code = 0, type = 0;
                if (std::sscanf(tok.c_str(), "%u:%x", &code, &type) == 2) r.chars.push_back({code, type});
            }
            g_evtkRows[ev] = r;
        }
        std::fprintf(stderr, "[victory-evtk] evtk_sevp.txt rows %zu%c", g_evtkRows.size(), 10);
    }
    void evtkArm(uint8_t *rdram, uint32_t blk, uint32_t ev, uint32_t prm)
    {
        // Try 2 (Jordan: still wrong): the scene loads its models from tagEVENTMNGPARAM casts (a3 +0x24..+0x30, u32,
        // 0x639C = empty; same layout the PPV opening fills) and its set from /STG/<0x4922C0> at load time (after the
        // ctor). SEVP codes are literal cast codes (0 = Rock), so all are written as-is; stage byte held until the dtor.
        g_evtkEvent = ev;
        g_evtkCageDone = false;
        g_evtkSavedStage = rdram[0x4922C0u];
        prm &= 0x1FFFFFFu;
        for (uint32_t i = 0; i < 8u; ++i) std::memcpy(&g_evtkSavedCasts[i], rdram + 0x4921CAu + i * 0x20u, 2);
        auto it = g_evtkRows.find(ev);
        if (it == g_evtkRows.end()) { std::fprintf(stderr, "[victory-evtk] %u not in evtk_sevp.txt%c", ev, 10); return; }
        uint32_t w = 0;
        for (const auto &c : it->second.chars)
        {
            if (c.second != 0x1000000u || w >= 8u) continue;
            if (blk) rdram[(blk & 0x1FFFFFFu) + 0xC9u + w] = 1;
            {
                // Try 3: codes < 100 (6051 '40', 6050 '20') hung on /EMD/40 -> CAM dummy 9100 until decoded.
                const uint32_t lit = (c.first < 100u && ev >= 6000u) ? 9100u : c.first; // Nov 9: TKME 4000s use today's ids (0 = Rock); only DORA 6050+ rows are old ids
                const uint16_t code = uint16_t(lit);
                std::memcpy(rdram + 0x4921CAu + w * 0x20u, &code, 2);
                if (prm && prm < 0x1FFFF00u && w < 4u) std::memcpy(rdram + prm + 0x24u + w * 4u, &lit, 4);
            }
            uint16_t now; std::memcpy(&now, rdram + 0x4921CAu + w * 0x20u, 2);
            std::fprintf(stderr, "[victory-evtk] %u cast %u sevp %u -> setup %u%c", ev, w, c.first, now, 10);
            ++w;
        }
        for (uint32_t k = w; prm && prm < 0x1FFFF00u && k < 4u; ++k)
        {
            const uint32_t empty = 0x639Cu;
            std::memcpy(rdram + prm + 0x24u + k * 4u, &empty, 4);
        }
        rdram[0x4922C0u] = uint8_t(int8_t(it->second.stage));
        g_evtkStagePending = true;
        std::fprintf(stderr, "[victory-evtk] %u stage %d (was %u) wrestlers %u param 0x%X%c", ev, it->second.stage,
                     g_evtkSavedStage, w, prm, 10);
    }
    PS2Runtime::RecompiledFunction g_origSd5EventMngDt = nullptr;
    void sd5EventMngDtEvtk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_evtkEvent != 0u)
        {
            for (uint32_t i = 0; i < 8u; ++i) std::memcpy(rdram + 0x4921CAu + i * 0x20u, &g_evtkSavedCasts[i], 2);
            std::fprintf(stderr, "[victory-evtk] %u done, setup casts restored (stage at its /STG/ load)%c", g_evtkEvent, 10);
            g_evtkEvent = 0u;
        }
        g_origSd5EventMngDt(rdram, ctx, runtime);
    }
    void initEventSystemEvtk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t common; std::memcpy(&common, rdram + 0x529400u, 4);
        const bool lend = g_evtkEvent < 5000u; // 6050-6055 = native DORA type 3: stage only
        if (g_evtkEvent == 0u || (lend && (g_evtkBlk == 0u || common != 0u)))
        {
            initEventSystemShirtTear(rdram, ctx, runtime);
            return;
        }
        const uint8_t stage = rdram[0x4922C0u];
        auto it = g_evtkRows.find(g_evtkEvent);
        if (it != g_evtkRows.end()) rdram[0x4922C0u] = uint8_t(int8_t(it->second.stage));
        if (lend) std::memcpy(rdram + 0x529400u, &g_evtkBlk, 4);
        std::fprintf(stderr, "[victory-evtk] InitEventSystem %u: lent common, stage byte %u -> %u%c", g_evtkEvent, stage,
                     rdram[0x4922C0u], 10);
        initEventSystemShirtTear(rdram, ctx, runtime);
        if (lend) std::memcpy(rdram + 0x529400u, &common, 4);
        rdram[0x4922C0u] = stage;
    }
    PS2Runtime::RecompiledFunction g_origActionEvent = nullptr;
    void actionEventEvtk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t seq = uint32_t(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        uint32_t type = 0, pad = 0;
        if (g_evtkEvent != 0u && seq != 0u && seq < 0x1FFFF00u)
        {
            std::memcpy(&type, rdram + seq + 0x38u, 4);
            std::memcpy(&pad, rdram + 0x4FFA00u, 4);
            pad &= 0x1FFFFFFu;
        }
        if (g_evtkEvent != 0u && seq != 0u && seq < 0x1FFFF00u)
        {
            // Nov 9 measure + end: type 1/4 ends via CSeasonSystem::HowToWait 0x253620 -> seq+0x84 = 4 countdown; once
            // counting, show type 3 so the last-event path (0x2aa340) takes the CAM doramock return like victory scenes.
            uint32_t cnt, idx, num; std::memcpy(&cnt, rdram + seq + 0x84u, 4); std::memcpy(&idx, rdram + seq + 0x64u, 4);
            std::memcpy(&num, rdram + seq + 0x68u, 4);
            static uint32_t s_frame = 0;
            if ((++s_frame % 60u) == 0u || cnt != 0u)
                std::fprintf(stderr, "[victory-evtk] act seq 0x%X type %u cnt %u idx %u/%u pad 0x%X%c", seq, type, cnt, idx, num, pad, 10);
            if (g_evtkBlk != 0u && g_evtkEvent < 5000u && cnt != 0u && (type == 1u || type == 4u))
            {
                const uint32_t three = 3u;
                std::memcpy(rdram + seq + 0x38u, &three, 4);
                type = 3u;
            }
        }
        uint16_t btn = 0;
        if ((type == 1u || type == 4u) && pad != 0u && pad < 0x1FFFF00u)
            std::memcpy(&btn, rdram + pad + 4u, 2);
        if (btn & 0x800u)
        {
            const uint32_t three = 3u;
            std::memcpy(rdram + seq + 0x38u, &three, 4);
            std::fprintf(stderr, "[victory-evtk] %u START -> skip path (type %u as 3)%c", g_evtkEvent, type, 10);
            g_origActionEvent(rdram, ctx, runtime);
            uint32_t now; std::memcpy(&now, rdram + seq + 0x38u, 4);
            if (now == 3u) std::memcpy(rdram + seq + 0x38u, &type, 4);
            return;
        }
        g_origActionEvent(rdram, ctx, runtime);
    }
    // Nov 9 Chamber cage for evtDora 6050-6055 (HCTP_VICTORY_EVTK only). gm.pac GMMC/GOBJ bank 0x14 tag 570 = type 92
    // GmChamber (2 models, 569 = 93 BKFire, 580 = 94 GmCharAction); no GMEV/STG list in gm.pac/bg.pac places tag 570, so
    // the scene never spawns it. After CGmControl::CreateGimmick 0x1914d0 / CreateEventGimmick 0x1916d0 really return,
    // call CreateObject 0x1b1bf0(ctl, 570, locate) once; locate = GmObjLocate 0x20 B (pos f32x3, tag s16, rot f32x3)
    // copied from STG0's ring-origin entry (tag 580).
    uint32_t guestCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, std::initializer_list<uint32_t> args);
    PS2Runtime::RecompiledFunction g_origCreateGimmick = nullptr, g_origCreateEventGimmick = nullptr;
    void evtkSpawnCage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ctl, const char *from)
    {
        if (g_evtkCageDone || g_evtkEvent < 6050u || g_evtkEvent > 6055u || ctl == 0u) return;
        g_evtkCageDone = true;
        const uint32_t loc = guestCall(rdram, ctx, runtime, 0x1001E0u, {0x20u}) & 0x1FFFFFFu;
        if (loc == 0u || loc >= 0x1FFFF00u) { std::fprintf(stderr, "[victory-evtk] cage: new failed%c", 10); return; }
        std::memset(rdram + loc, 0, 0x20);
        const float rot[3] = {1.570796251296997f, -7.93289274270137e-08f, -1.1968239732595976e-07f};
        const int16_t tag = 570;
        std::memcpy(rdram + loc + 0xCu, &tag, 2);
        std::memcpy(rdram + loc + 0x10u, rot, 12);
        const uint32_t obj = guestCall(rdram, ctx, runtime, 0x1b1bf0u, {ctl, 570u, loc});
        uint32_t gctl = 0; std::memcpy(&gctl, rdram + 0x526640u, 4);
        std::fprintf(stderr, "[victory-evtk] %u cage: CreateObject(ctl 0x%X, 570, 0x%X) via %s -> 0x%X (gCtl 0x%X)%c", g_evtkEvent,
                     ctl, loc, from, obj, gctl, 10);
    }
    void createGimmickEvtk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ctl = uint32_t(_mm_cvtsi128_si32(ctx->r[4])), ra = uint32_t(_mm_cvtsi128_si32(ctx->r[31]));
        g_origCreateGimmick(rdram, ctx, runtime);
        if (ctx->pc == ra) evtkSpawnCage(rdram, ctx, runtime, ctl, "CreateGimmick");
    }
    void createEventGimmickEvtk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ctl = uint32_t(_mm_cvtsi128_si32(ctx->r[4])), ra = uint32_t(_mm_cvtsi128_si32(ctx->r[31]));
        g_origCreateEventGimmick(rdram, ctx, runtime);
        if (ctx->pc == ra) evtkSpawnCage(rdram, ctx, runtime, ctl, "CreateEventGimmick");
    }
    PS2Runtime::RecompiledFunction g_origHowToWait = nullptr;
    void howToWaitEvtk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // CSeasonSystem::HowToWait 0x253620 (a0 = common): Season waits here for X at the scene end. CAM (lent common)
        // has no Season driver -> end when CEventSystem (common+0x28) play state +0x10 == 4 (finished) or START held.
        const uint32_t common = uint32_t(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        g_origHowToWait(rdram, ctx, runtime);
        if (g_evtkEvent == 0u || g_evtkEvent >= 5000u || g_evtkBlk == 0u || common != (g_evtkBlk & 0x1FFFFFFu)) return;
        if (_mm_cvtsi128_si32(ctx->r[2]) & 0xFF) return;
        uint32_t es = 0, st = 0, pad = 0; uint16_t btn = 0;
        std::memcpy(&es, rdram + common + 0x28u, 4); es &= 0x1FFFFFFu;
        if (es != 0u && es < 0x1FFFF00u) std::memcpy(&st, rdram + es + 0x10u, 4);
        std::memcpy(&pad, rdram + 0x4FFA00u, 4); pad &= 0x1FFFFFFu;
        if (pad != 0u && pad < 0x1FFFF00u) std::memcpy(&btn, rdram + pad + 4u, 2);
        if (st == 4u || (btn & 0x800u))
        {
            std::fprintf(stderr, "[victory-evtk] %u HowToWait -> end (play state %u, START %d)%c", g_evtkEvent, st,
                         (btn & 0x800u) ? 1 : 0, 10);
            ctx->r[2] = _mm_set_epi64x(0, 1);
        }
    }
    PS2Runtime::RecompiledFunction g_origSd5EventMngCt = nullptr;
    void sd5EventMngCtEvtk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t type = uint32_t(_mm_cvtsi128_si32(ctx->r[5]));
        int32_t pending; std::memcpy(&pending, rdram + 0x58CCF8u, 4);
        uint32_t common; std::memcpy(&common, rdram + 0x529400u, 4);
        common &= 0x1FFFFFFu;
        uint16_t cur = 0;
        if (common != 0u && common < 0x1FFFF00u)
            std::memcpy(&cur, rdram + common + 0x56u, 2);
        // Nov 9 HCTP_SVR06_PYRO: CAM entrance row 3944 = Brock's stock 3044 + SvR06 PCEN/GMEV 4044 pyro (host-drawn,
        // hctp_svr06_pyro.cpp). Any other entrance preview clears it. Stock 3044 (Brock's slot) never arms it.
        if (type == 2u && !(std::getenv("HCTP_SVR06_PYRO") && std::getenv("HCTP_SVR06_PYRO")[0] == '0')) // Nov 10: default on
        {
            // CAM entrance preview: MovesNext 0x3425a0 -> InitEventParam 0x342e30 puts the number at param(a3)+4 and in
            // the cast's game prop +0xEA (prop = 0x4C0F54 + id*0x170, or GetCommon+0x12F44 for the CAW/edit slot).
            uint32_t param = uint32_t(_mm_cvtsi128_si32(ctx->r[7])) & 0x1FFFFFFu, pnum = 0;
            if (param != 0u && param < 0x1FFFF00u)
                std::memcpy(&pnum, rdram + param + 4u, 4);
            if (cur == 3944u || pnum == 3944u)
            {
                const uint16_t stock = g_svr06CamEvent > 0 ? static_cast<uint16_t>(g_svr06CamEvent) : uint16_t(3044u);
                if (cur == 3944u)
                    std::memcpy(rdram + common + 0x56u, &stock, 2), cur = stock;
                if (pnum == 3944u)
                {
                    const uint32_t s32 = stock;
                    std::memcpy(rdram + param + 4u, &s32, 4);
                }
                int props = 0;
                for (uint32_t id = 0; id < 0x56u; ++id)
                {
                    uint16_t v; std::memcpy(&v, rdram + 0x4C0F54u + id * 0x170u + 0xEAu, 2);
                    if (v == 3944u) std::memcpy(rdram + 0x4C0F54u + id * 0x170u + 0xEAu, &stock, 2), ++props;
                }
                // CAW blocks 0x4B6460 + n*0x460 (ids 0x56+)
                for (uint32_t a = 0x4B6460u; a < 0x4B6460u + 0x4Cu * 0x460u; a += 0x460u)
                {
                    uint16_t v; std::memcpy(&v, rdram + a + 0xEAu, 2);
                    if (v == 3944u) std::memcpy(rdram + a + 0xEAu, &stock, 2), ++props;
                }
                std::fprintf(stderr, "[pyro] CAM row 3944 -> %u (param %u, common %u, props %d)%c", unsigned(stock), pnum, cur, props, 10);
                // STEP 2: SetEvent of the SvR06 event arms the pyro (hctpPyroHas); 3944 = old Brock row (old CAM saves)
            }
            else
                hctpPyroDisarm();
        }
        if (type == 2u && cur >= 4000u && cur <= 4999u) // Nov 8: 4000s in the entrance list (add-on MOVK/LIST)
        {
            SET_GPR_U32(ctx, 5, 1u);
            std::fprintf(stderr, "[victory-evtk] entrance preview %u -> event type 1/4 (mode %d)%c", cur,
                         int(_mm_cvtsi128_si32(ctx->r[6])), 10);
        }
        else if (type == 3u && pending >= 4000 && pending <= 4999 && common != 0u && common < 0x1FFFF00u)
        {
            uint16_t old; std::memcpy(&old, rdram + common + 0x56u, 2);
            const uint16_t ev = uint16_t(pending);
            std::memcpy(rdram + common + 0x56u, &ev, 2);
            SET_GPR_U32(ctx, 5, 1u);
            std::fprintf(stderr, "[victory-evtk] doramock %d -> event type 1/4 (common+0x56 %u -> %u, mode %d)%c", pending,
                         old, ev, int(_mm_cvtsi128_si32(ctx->r[6])), 10);
        }
        else if (type == 3u && pending >= 4000 && pending <= 4999 && common == 0u)
        {
            // Nov 9: CAM has no Season block (*0x529400 == 0) but the ctor (0x2a7a9c..) and SetSeasonEventNo 0x2ac920
            // read the event number from common+0x56. Lend a zeroed guest block (game operator new 0x1001e0) for the
            // ctor only, then restore 0 so nothing later thinks a Season is running.
            __m128i saved[8];
            for (int i = 0; i < 8; ++i) saved[i] = ctx->r[4 + i];
            const uint32_t blk = ppvCall(rdram, ctx, runtime, 0x1001e0u, {0x400u});
            for (int i = 0; i < 8; ++i) ctx->r[4 + i] = saved[i];
            if (blk)
            {
                std::memset(rdram + (blk & 0x1FFFFFFu), 0, 0x400u);
                g_evtkBlk = blk; // kept alive: lent again around InitEventSystem
                evtkArm(rdram, blk, uint32_t(pending), uint32_t(_mm_cvtsi128_si32(ctx->r[7])));
                const uint16_t ev = uint16_t(pending);
                std::memcpy(rdram + (blk & 0x1FFFFFFu) + 0x56u, &ev, 2);
                std::memcpy(rdram + 0x529400u, &blk, 4);
                SET_GPR_U32(ctx, 5, 1u);
                std::fprintf(stderr, "[victory-evtk] doramock %d -> type 1/4 with lent common 0x%X (CAM)%c", pending, blk, 10);
                g_origSd5EventMngCt(rdram, ctx, runtime);
                const uint32_t zero = 0u;
                std::memcpy(rdram + 0x529400u, &zero, 4);
                return;
            }
            std::fprintf(stderr, "[victory-evtk] lend common FAILED for %d%c", pending, 10);
        }
        else if (type == 3u && pending >= 6050 && pending <= 6055)
            evtkArm(rdram, 0u, uint32_t(pending), uint32_t(_mm_cvtsi128_si32(ctx->r[7]))); // Nov 9: Chamber DORA scenes keep type 3; casts + stage only
        else
            std::fprintf(stderr, "[victory-evtk] CSd5EventMng type %u mode %d pending %d common+0x56 %u%c", type,
                         int(_mm_cvtsi128_si32(ctx->r[6])), pending, cur, 10);
        g_origSd5EventMngCt(rdram, ctx, runtime);
    }
    #include "hctp_victory_safe.inl"
    // In-match victory (Nov 7): SetWinner 0x39cf00 never calls ConvertDoramockFromMoves. It reads the winner's roster
    // record +0xEA (victory chosen in CAM; record = GetSaveData 0x4B6450 + 0xAB04 + id*0x170, id = cast code/100 via
    // GetProp 0x39b480): nonzero + single winner (tagWinner+0x10 == 1) + no 0x58ccf0 -> event 6248 + GetWinBaseNo
    // 0x39ceb0 (WAZA id >= 0x1C34 only), else the DORA row of record +0x166 (Hogan 32 -> generic 6259). Post-hook:
    // where victory.txt maps +0xEA (or the roster id when +0xEA = 0), under the same stock conditions, that event wins.
    // Nov 10: roster-expansion slots (cast/100 = pool model no 86..99) -> slot id + slot record +0xEA (defined below).
    bool rosterxVictoryKey(uint32_t modelNo, uint32_t &slotId, int16_t &sel);
    PS2Runtime::RecompiledFunction g_origSetWinner = nullptr;
    void setWinnerVictoryTxt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = uint32_t(_mm_cvtsi128_si32(ctx->r[31]));
        const uint32_t outP = uint32_t(_mm_cvtsi128_si32(ctx->r[5])) & 0x1FFFFFFu;
        const uint32_t winP = uint32_t(_mm_cvtsi128_si32(ctx->r[6])) & 0x1FFFFFFu;
        g_origSetWinner(rdram, ctx, runtime);
        if (ctx->pc != ra || rdram[0x4B6439u] != 0 || winP > 0x1FFFFF0u || outP > 0x1FFFFFCu)
            return;
        uint32_t kind, prop, flag;
        std::memcpy(&kind, rdram + winP + 0x10u, 4);
        std::memcpy(&prop, rdram + winP, 4);
        std::memcpy(&flag, rdram + 0x58CCF0u, 4);
        prop &= 0x1FFFFFFu;
        if (kind != 1u || flag != 0u || prop == 0u || prop > 0x1FFFFF0u)
            return;
        uint16_t cast; std::memcpy(&cast, rdram + prop + 0xAu, 2);
        uint32_t id = cast / 100u;
        int16_t sel = 0;
        if (id >= 0x56u)
        {
            // slot: victory.txt "<slot id decimal> <event>" (or the slot record's +0xEA list number)
            if (!rosterxVictoryKey(id, id, sel))
                return;
        }
        else
            std::memcpy(&sel, rdram + 0x4B6450u + 0xAB04u + id * 0x170u + 0xEAu, 2);
        if (sel >= 0x1C34)
            return; // a real WAZA victory: stock GetWinBaseNo path
        auto it = g_victoryEvent.find(sel != 0 ? uint32_t(sel) : id);
        if (it == g_victoryEvent.end() || it->second < 6000u || it->second > 6455u)
            return;
        int32_t old; std::memcpy(&old, rdram + outP, 4);
        const int32_t ev = int32_t(it->second);
        std::memcpy(rdram + outP, &ev, 4);
        std::fprintf(stderr, "[victory] match winner id %u (+0xEA %d): event %d -> %d (victory.txt)%c", id, sel, old, ev, 10);
    }
    void installLooseEntrances(PS2Runtime &runtime)
    {
        {
            std::ifstream vcfg("MODS/PAC/victory.txt");
            for (std::string line; std::getline(vcfg, line);)
            {
                unsigned n, ev;
                if (!line.empty() && line[0] != '#' && std::sscanf(line.c_str(), "%u %u", &n, &ev) == 2)
                    g_victoryEvent[n] = ev;
            }
            if (std::getenv("HCTP_VICTORY_EVTK") != nullptr)
            {
                std::ifstream ecfg("MODS/PAC/victory_evtk.txt");
                for (std::string line; std::getline(ecfg, line);)
                {
                    unsigned n, ev;
                    if (!line.empty() && line[0] != '#' && std::sscanf(line.c_str(), "%u %u", &n, &ev) == 2)
                        g_victoryEvent[n] = ev;
                }
                if ((g_origSd5EventMngCt = runtime.lookupFunction(0x2a7a20u)) != nullptr)
                    runtime.replaceFunction(0x2a7a20u, sd5EventMngCtEvtk);
                evtkLoadTable();
                if ((g_origSd5EventMngDt = runtime.lookupFunction(0x2a7c80u)) != nullptr)
                    runtime.replaceFunction(0x2a7c80u, sd5EventMngDtEvtk);
                if ((g_origActionEvent = runtime.lookupFunction(0x2a9c90u)) != nullptr)
                    runtime.replaceFunction(0x2a9c90u, actionEventEvtk);
                if ((g_origHowToWait = runtime.lookupFunction(0x253620u)) != nullptr)
                    runtime.replaceFunction(0x253620u, howToWaitEvtk);
                if ((g_origCreateGimmick = runtime.lookupFunction(0x1914d0u)) != nullptr)
                    runtime.replaceFunction(0x1914d0u, createGimmickEvtk);
                if ((g_origCreateEventGimmick = runtime.lookupFunction(0x1916d0u)) != nullptr)
                    runtime.replaceFunction(0x1916d0u, createEventGimmickEvtk);
            }
            if ((!g_victoryEvent.empty() || std::getenv("HCTP_VICTORY_TEST")) &&
                (g_origConvertDoramock = runtime.lookupFunction(0x39dba0u)) != nullptr)
                runtime.replaceFunction(0x39dba0u, convertDoramockFromMoves);
            if (!g_victoryEvent.empty() && (g_origSetMovesDoramock = runtime.lookupFunction(0x39d180u)) != nullptr)
                runtime.replaceFunction(0x39d180u, setMovesDoramockMapped);
            if (!g_victoryEvent.empty() && (g_origSetWinner = runtime.lookupFunction(0x39cf00u)) != nullptr)
                runtime.replaceFunction(0x39cf00u, setWinnerVictoryTxt);
        }
        if (std::getenv("HCTP_NO_LOOSE_PAC") != nullptr)
            return;
        scanLoosePac();
        if (!g_loosePac.empty() && (g_origCFileSearch = runtime.lookupFunction(0x1482f0u)) != nullptr)
            runtime.replaceFunction(0x1482f0u, cfileSearchLoose);
        std::ifstream cfg("MODS/PAC/entrances.txt");
        for (std::string line; std::getline(cfg, line);)
        {
            unsigned id, attire, ev;
            if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%u %u %u", &id, &attire, &ev) != 3 || attire > 9)
                continue;
            g_attireEntrance[id * 10u + attire] = ev;
        }
        if ((g_origGetEntranceEventNo = runtime.lookupFunction(0x2ad1b0u)) != nullptr) // always: roster-slot entrances too
            runtime.replaceFunction(0x2ad1b0u, getEntranceEventNoPerAttire);
        std::cerr << "[hctp] loose pac files: " << g_loosePac.size() << ", per-attire entrances: "
                  << g_attireEntrance.size() << std::endl;
    }

    // TEMP diag (HCTP_SPRLOG=1): who calls Sofdec's mwPlySwitchToIdle 0x3ea680 (distinct return addresses)
    PS2Runtime::RecompiledFunction g_origSwitchToIdle = nullptr;
    void switchToIdleLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::set<uint32_t> seen;
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29])) & 0x1FFFFFFu;
        if (seen.insert(ra).second)
        {
            char line[512];
            int n = std::snprintf(line, sizeof(line), "[idlecaller] ra=0x%x stack:", ra);
            for (uint32_t off = 0; off < 0x200 && n < 480; off += 4) // code-looking words on the stack = return addresses
            {
                uint32_t w; std::memcpy(&w, rdram + sp + off, 4);
                if (w >= 0x100000u && w < 0x400000u && (w & 3u) == 0u)
                    n += std::snprintf(line + n, sizeof(line) - n, " 0x%x", w);
            }
            std::cerr << line << std::endl;
        }
        g_origSwitchToIdle(rdram, ctx, runtime);
    }

    // TEMP diag (HCTP_CRILOG=1): CRI file-read chain ADXF -> CVFS -> dvCi -> SRD -> sceCdRead. Each hook prints
    // "[crilog] name a0 a1 a2 -> v0 (#calls, ms)" whenever (a0,a1,a2,v0, SRD state words) differs from its last call.
    struct CriHook { const char *name; PS2Runtime::RecompiledFunction orig; uint64_t calls; uint64_t last; };
    CriHook g_criHooks[32];
    bool g_criAlways[32];
    const auto g_criT0 = std::chrono::steady_clock::now();
    template <int I>
    void criLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CriHook &h = g_criHooks[I];
        auto reg = [ctx](int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); };
        auto word = [rdram](uint32_t a) { uint32_t w; std::memcpy(&w, rdram + a, 4); return w; };
        const uint32_t a0 = reg(4), a1 = reg(5), a2 = reg(6), ra = reg(31);
        const uint32_t srdBefore = word(0x437ee0u), lockBefore = word(0x437f1cu);
        const uint32_t s0In = reg(16), spIn = reg(29);
        h.orig(rdram, ctx, runtime);
        const uint32_t s0Out = reg(16), spOut = reg(29);
        const uint32_t v0 = reg(2);
        ++h.calls;
        const uint64_t sig = (uint64_t(a0) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(a1) << 7) ^ (uint64_t(a2) << 21) ^
                             (uint64_t(v0) << 40) ^ (uint64_t(srdBefore) << 3) ^ (uint64_t(lockBefore) << 50) ^
                             (uint64_t(word(0x437f20u)) << 13) ^ (uint64_t(word(0x437f24u)) << 29);
        if (sig == h.last && !g_criAlways[I] && (h.calls % 5000u) != 0u)
            return;
        h.last = sig;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_criT0).count();
        char line[256];
        std::snprintf(line, sizeof(line), "[crilog] %s a0=%x a1=%x a2=%x ra=%x -> v0=%x | srd %08x->%08x lock %x->%x f20=%x f24=%x (#%llu, %lld ms)",
                      h.name, a0, a1, a2, ra, v0, srdBefore, word(0x437ee0u), lockBefore, word(0x437f1cu),
                      word(0x437f20u), word(0x437f24u), static_cast<unsigned long long>(h.calls), ms);
        if (s0In != s0Out || spIn != spOut)
            std::cerr << line << " S0/SP CHANGED s0 " << std::hex << s0In << "->" << s0Out << " sp " << spIn << "->" << spOut << std::dec << "\n";
        std::cerr << line << " v0hi=" << std::hex << static_cast<uint32_t>(static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[2])) >> 32) << std::dec;
        if (I == 14 || I == 15 || I == 18 || I == 19 || I == 27 || I == 28) // open calls: a0 = file name
        {
            char name[96] = {};
            for (uint32_t k = 0; k + 1 < sizeof(name); ++k)
            {
                const char c = static_cast<char>(rdram[(a0 + k) & 0x1FFFFFFu]);
                if (!c) break;
                name[k] = c;
            }
            std::cerr << " name='" << name << "'";
        }
        // CRI thread liveness: ADXPS2_ExecVint count, vsync/fs/mwidle thread loop counts, fs-running flag, setup count
        std::snprintf(line, sizeof(line), " | vint %u vs %u fs %u mwi %u fsrun %u vsrun %u thr %d tid fs=%u vs=%u",
                      word(0x425cc4u), word(0x426db0u), word(0x426db8u), word(0x426dc0u), word(0x426d90u),
                      word(0x426d88u), static_cast<int>(word(0x426d54u)), word(0x426de4u), word(0x426de0u));
        std::cerr << line << std::endl;
    }
    template <int I>
    void criHook(PS2Runtime &runtime, uint32_t addr, const char *name, bool always = false)
    {
        g_criAlways[I] = always;
        g_criHooks[I] = {name, runtime.lookupFunction(addr), 0, ~0ull};
        if (!g_criHooks[I].orig || !runtime.replaceFunction(addr, criLogged<I>))
            std::cerr << "[crilog] hook FAILED " << name << std::endl;
    }

    // Native sound kit: the banks the game sends to the IOP (see the hook install), per bank id
    struct SkBank
    {
        uint32_t spuAddr = 0;
        std::vector<uint8_t> hd, bd, sq;
    };
    std::mutex g_skBankLock;
    std::map<int, SkBank> g_skBanks;
    PS2Runtime::RecompiledFunction g_skTransOrig = nullptr;
    void skTransCaptured(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto reg = [ctx](int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); };
        const int bank = static_cast<int>(reg(4));
        const uint32_t info = reg(5);
        auto w32 = [rdram](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFCu), 4); return v; };
        auto grab = [rdram](uint32_t adr, uint32_t size)
        {
            std::vector<uint8_t> v;
            adr &= 0x1FFFFFFu;
            if (adr != 0u && size != 0u && size < 0x1000000u && adr + size <= 0x2000000u)
                v.assign(rdram + adr, rdram + adr + size);
            return v;
        };
        SkBank b;
        b.bd = grab(w32(info + 0u), w32(info + 4u));
        b.spuAddr = w32(info + 8u);
        b.hd = grab(w32(info + 0xcu), w32(info + 0x10u));
        b.sq = grab(w32(info + 0x14u), w32(info + 0x18u));
        std::fprintf(stderr, "[skbank] bank %d HD %zu BD %zu (spu 0x%x) SQ %zu%c", bank, b.hd.size(), b.bd.size(), b.spuAddr,
                     b.sq.size(), 10);
        if (const char *dir = std::getenv("HCTP_SKDUMP"))
        {
            static std::map<std::string, bool> s_done;
            for (auto [tag, data] : {std::pair<const char *, const std::vector<uint8_t> *>{"hd", &b.hd}, {"bd", &b.bd}, {"sq", &b.sq}})
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s\\bank%02d_spu%05x.%s", dir, bank, b.spuAddr, tag);
                if (data->empty() || s_done[path])
                    continue;
                s_done[path] = true;
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(data->data(), 1, data->size(), f);
                    std::fclose(f);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_skBankLock);
            g_skBanks[bank] = std::move(b);
        }
        g_skTransOrig(rdram, ctx, runtime);
    }
}

#include "hctp_native_sk.inl"

// The game's own recompiled libvu0 functions (runner), for HCTP_VU0_RECOMP=1: A/B against the host stubs (leg twitch).
#define HCTP_VU0_FN(name, addr) void name##_##addr(uint8_t *, R5900Context *, PS2Runtime *);
#define HCTP_VU0_LIST(X) X(sceVu0RotMatrixX, 0x103ce0) X(sceVu0RotMatrixY, 0x103d88) X(sceVu0RotMatrixZ, 0x103c38) \
    X(sceVu0RotMatrix, 0x103e30) X(sceVu0MulMatrix, 0x1038a0) X(sceVu0InversMatrix, 0x1039b8)                        \
    X(sceVu0Normalize, 0x103930) X(sceVu0ApplyMatrix, 0x103870) X(sceVu0InterVector, 0x103a68)                       \
    X(sceVu0InterVectorXYZ, 0x1043d8) X(sceVu0OuterProduct, 0x1038e8) X(sceVu0InnerProduct, 0x103908)                \
    X(sceVu0TransMatrix, 0x103af0) X(sceVu0UnitMatrix, 0x103b98) X(sceVu0CopyMatrix, 0x103b30)                       \
    X(sceVu0TransposeMatrix, 0x103970) X(sceVu0ScaleVector, 0x103ad8) X(sceVu0MulVector, 0x103ac0)
HCTP_VU0_LIST(HCTP_VU0_FN)
#undef HCTP_VU0_FN

namespace
{
    // NATIVE SOUND KIT hooks (HCTP_NATIVE_SK=1). Native handles are >= hctpsk::kHandleBase; anything else goes to the
    // original (IOP) function, so the MIDI-sequence banks keep the IOP path.
    PS2Runtime::RecompiledFunction g_skOrigBindMsin = nullptr, g_skOrigMsinCtl = nullptr, g_skOrigMsinStat = nullptr,
                                   g_skOrigHsynCtl = nullptr, g_skOrigHsynUnbind = nullptr, g_skOrigMsinUnbind = nullptr;
    uint32_t skReg(R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); }
    void skRet(R5900Context *ctx, uint32_t v) { ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v))); }
    uint32_t skW32(uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFCu), 4); return v; }

    // CSkCtrl::BindMsinBank(bank) 0x2e3170 (Ghidra): HSYNTH_Bind(3, -1, HD adr +4, HD size +0x10, SPU adr +8, BD size
    // +0x14) -> bank, HSYNTH_Bind(5, -1, bank, 0) -> track, MSIN_Bind(0..) -> port, MSIN_Bind(5, track, port); the
    // handles go to *(info+0x24)+8 / +0 / +0xc and the "bound" flag info+0x2c = 1 (info = 0x53f600 + bank*0x30).
    void skBindMsinBank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t info = 0x53f600u + skReg(ctx, 4) * 0x30u;
        const uint32_t spu = skW32(rdram, info + 8u);
        std::vector<uint8_t> hd;
        {
            std::lock_guard<std::mutex> lock(g_skBankLock);
            for (const auto &kv : g_skBanks)
                if (kv.second.spuAddr == spu && !kv.second.hd.empty())
                    hd = kv.second.hd;
        }
        hctpsk::Bank bank;
        bank.hd = std::move(hd);
        bank.spuAddr = spu;
        bank.parse();
        if (!bank.valid)
        {
            std::fprintf(stderr, "[nativesk] bank spu 0x%x not captured / not an HD: IOP path%c", spu, 10);
            g_skOrigBindMsin(rdram, ctx, runtime);
            return;
        }
        int handle;
        {
            std::lock_guard<std::mutex> lock(hctpsk::g_lock);
            hctpsk::initTables();
            hctpsk::g_banks.push_back(std::move(bank));
            hctpsk::Track t;
            t.bank = static_cast<int>(hctpsk::g_banks.size()) - 1;
            hctpsk::g_tracks.push_back(t);
            handle = hctpsk::kHandleBase + static_cast<int>(hctpsk::g_tracks.size()) - 1;
        }
        const uint32_t tr = skW32(rdram, info + 0x24u);
        auto put = [rdram](uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x1FFFFFCu), &v, 4); };
        put(tr + 8u, static_cast<uint32_t>(handle));
        put(tr + 0u, static_cast<uint32_t>(handle));
        put(tr + 0xcu, static_cast<uint32_t>(handle));
        rdram[(info + 0x2cu) & 0x1FFFFFFu] = 1;
        std::fprintf(stderr, "[nativesk] effect bank spu 0x%x -> native track 0x%x%c", spu, handle, 10);
    }

    // sceSkSsMSIN_Control(track, msg, a2, a3, t0, t1, t2, t3, [sp]) 0x2e5038
    void skMsinControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handle = static_cast<int>(skReg(ctx, 4));
        if (handle < hctpsk::kHandleBase)
        {
            g_skOrigMsinCtl(rdram, ctx, runtime);
            return;
        }
        const uint32_t a[7] = {skReg(ctx, 6), skReg(ctx, 7), skReg(ctx, 8), skReg(ctx, 9), skReg(ctx, 10), skReg(ctx, 11),
                               skW32(rdram, skReg(ctx, 29))};
        std::lock_guard<std::mutex> lock(hctpsk::g_lock);
        skRet(ctx, hctpsk::msinControl(handle, skReg(ctx, 5), a));
    }

    // sceSkSsMSIN_Status(track, 0x11, id) 0x2e5138: non-zero while the id still sounds (skmsin: state & 0x70)
    void skMsinStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handle = static_cast<int>(skReg(ctx, 4));
        if (handle < hctpsk::kHandleBase)
        {
            g_skOrigMsinStat(rdram, ctx, runtime);
            return;
        }
        std::lock_guard<std::mutex> lock(hctpsk::g_lock);
        skRet(ctx, hctpsk::idBusy(handle, static_cast<uint8_t>(skReg(ctx, 6))) ? 0x10u : 0u);
    }

    // sceSkSsHSYNTH_Control(track, 0x1f / 0x1e ...) 0x2e4ad0: CSkCtrl's stop / sound-off -> the track's voices off
    void skHsynControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handle = static_cast<int>(skReg(ctx, 4));
        if (handle < hctpsk::kHandleBase)
        {
            g_skOrigHsynCtl(rdram, ctx, runtime);
            return;
        }
        std::lock_guard<std::mutex> lock(hctpsk::g_lock);
        hctpsk::noteOff(handle, 0, 0xff, true);
        skRet(ctx, 0u);
    }

    // sceSkSsHSYNTH_Unbind / sceSkSsMSIN_Unbind (0x2e4a28 / 0x2e4f90): a native track's voices off (its bank stays,
    // a handle is never reused)
    template <int Which>
    void skUnbind(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handle = static_cast<int>(skReg(ctx, 4));
        if (handle < hctpsk::kHandleBase)
        {
            (Which == 0 ? g_skOrigHsynUnbind : g_skOrigMsinUnbind)(rdram, ctx, runtime);
            return;
        }
        std::lock_guard<std::mutex> lock(hctpsk::g_lock);
        hctpsk::noteOff(handle, 0, 0xff, true);
        skRet(ctx, 0u);
    }

    // TEMP diag, part of HCTP_BONE_JITTER: CSkinModel::Update 0x150be0 calls owner->vfunc (owner = *(model+0),
    // fn = *(*(owner+0x98)+0x20)) for the bone at (model+0xE8)+0x44, after copying +0x40 into +0x44, then inverts the
    // (model+0xE8) matrix in place. Per frame for the 92-bone CAM model: fn address, the two indices and a hash of the
    // object's first 0x40 bytes before and after Update (logged on change, first 300 lines).
    PS2Runtime::RecompiledFunction g_skinUpdateOrig = nullptr, g_skinFastUpdateOrig = nullptr;
    // MulMatrixN2 0x1510c0 (a0 skin out, a1 scratchpad chain, a2 inverse binds, a3 count, t0 world out), called by
    // FastUpdate: for the watched model (HCTP_IKWATCH_MODEL, default c40110), the foot's (bone 89) scratchpad matrix
    // it copies to the world array, plus the scratchpad root (a1 - 0x40) and the foot's parent, in the ikring timeline.
    PS2Runtime::RecompiledFunction g_mulN2Orig = nullptr;
    struct MulN2Rec { int64_t ns; float spr89[3], spr88[3], root[3], wld89[3]; uint32_t a1, n, pcAfter, sp; };
    std::vector<MulN2Rec> g_mulN2Ring(60);
    uint64_t g_mulN2N = 0;
    void mulN2Logged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[7]));
        const uint32_t t0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[8])) & 0x1FFFFFFu;
        static const uint32_t s_model = []
        {
            const char *v = std::getenv("HCTP_IKWATCH_MODEL");
            return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : 0xc40110u;
        }();
        uint32_t world = 0;
        std::memcpy(&world, rdram + s_model + 0xD0u, 4);
        const bool watched = (world & 0x1FFFFFFu) == t0 && (a1 & 0xF0000000u) == 0x70000000u && n > 89u;
        MulN2Rec rec{};
        if (watched)
        {
            const uint8_t *spr = runtime->memory().getScratchpad();
            const uint32_t off = a1 & 0x3FFFu;
            rec.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            rec.a1 = a1;
            rec.n = n;
            if (off + 90u * 64u <= 0x4000u && off >= 0x40u)
            {
                std::memcpy(rec.spr89, spr + off + 89u * 64u + 0x30u, 12);
                std::memcpy(rec.spr88, spr + off + 88u * 64u + 0x30u, 12);
                std::memcpy(rec.root, spr + off - 0x40u + 0x30u, 12);
            }
        }
        g_mulN2Orig(rdram, ctx, runtime);
        if (!watched)
            return;
        std::memcpy(rec.wld89, rdram + t0 + 89u * 64u + 0x30u, 12);
        rec.pcAfter = ctx->pc;
        rec.sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
        g_mulN2Ring[g_mulN2N++ % g_mulN2Ring.size()] = rec;
        if (FILE *go = std::fopen("mulring.go", "rb"))
        {
            std::fclose(go);
            std::remove("mulring.go");
            for (uint64_t i = g_mulN2N > g_mulN2Ring.size() ? g_mulN2N - g_mulN2Ring.size() : 0; i < g_mulN2N; ++i)
            {
                const MulN2Rec &q = g_mulN2Ring[i % g_mulN2Ring.size()];
                std::fprintf(stderr, "[mulring] t %lld a1 %08x n %u pcAfter %06x sp %08x | spr root row3 %.3f %.3f %.3f | spr88 %.3f %.3f %.3f | spr89 %.3f %.3f %.3f | world89 after %.3f %.3f %.3f%c",
                             static_cast<long long>(q.ns), q.a1, q.n, q.pcAfter, q.sp, q.root[0], q.root[1], q.root[2], q.spr88[0], q.spr88[1], q.spr88[2], q.spr89[0],
                             q.spr89[1], q.spr89[2], q.wld89[0], q.wld89[1], q.wld89[2], 10);
            }
            std::fflush(stderr);
        }
    }
    void skinUpdateCommon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction orig)
    {
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        auto rd = [&](uint32_t a) -> uint32_t { uint32_t v = 0; a &= 0x1FFFFFFu; if (a >= 0x100000u && a < 0x1FFFFFCu) std::memcpy(&v, rdram + a, 4); return v; };
        uint32_t ymp = rd(model + 0xC8u), bones = ymp ? rd(ymp + 0x14u) : 0u;
        const uint32_t obj = rd(model + 0xE8u) & 0x1FFFFFFu;
        if (bones != 92u)
        {
            orig(rdram, ctx, runtime);
            return;
        }
        // Bone 89 (l_ashi, the worst shaker): its local (model+0xCC) and world (model+0xD0) matrices around the call,
        // and its parent's (bone entry +0x30 = parent index, as Update 0x150c54 reads it). Kept in a ring of 120 calls;
        // dumped when the file skinupd.go appears in the game folder (deleted after).
        constexpr uint32_t kBone = 89u;
        const uint32_t loc = rd(model + 0xCCu) & 0x1FFFFFFu, wld = rd(model + 0xD0u) & 0x1FFFFFFu;
        const uint32_t table = rd(ymp + 0x20u) & 0x1FFFFFFu;
        const int32_t parent = static_cast<int32_t>(rd(table + kBone * 80u + 0x30u));
        struct Rec { uint64_t call; int64_t ns; float locB[16], wldB[16], wldA[16], parA[16]; int32_t parent; uint32_t obj, ra, sp, model, ec, f0; };
        static std::vector<Rec> ring(120);
        static uint64_t calls = 0;
        Rec &r = ring[calls % ring.size()];
        r.call = calls++;
        r.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        r.parent = parent;
        r.obj = obj;
        r.model = model;
        r.ec = rd(model + 0xECu);
        r.f0 = rd(model + 0xF0u) & 0x1FFFFFFu;
        r.ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        r.sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
        std::memcpy(r.locB, rdram + loc + kBone * 64u, 64);
        std::memcpy(r.wldB, rdram + wld + kBone * 64u, 64);
        orig(rdram, ctx, runtime);
        std::memcpy(r.wldA, rdram + wld + kBone * 64u, 64);
        std::memset(r.parA, 0, 64);
        if (parent >= 0 && parent < 92)
            std::memcpy(r.parA, rdram + wld + static_cast<uint32_t>(parent) * 64u, 64);
        static uint32_t check = 0;
        if ((++check & 31u) != 0u)
            return;
        if (FILE *go = std::fopen("skinupd.go", "rb"))
        {
            std::fclose(go);
            std::remove("skinupd.go");
            for (uint64_t i = calls > ring.size() ? calls - ring.size() : 0; i < calls; ++i)
            {
                const Rec &q = ring[i % ring.size()];
                std::fprintf(stderr, "[skinupd] t %lld %s call %llu model %06x ec %u f0 %06x ra %06x sp %08x parent %d | local row0 %.4f %.4f %.4f row3 %.3f %.3f %.3f | world before row3 %.3f %.3f %.3f after row0 %.4f %.4f %.4f row3 %.3f %.3f %.3f | parent world row3 %.3f %.3f %.3f%c",
                             static_cast<long long>(q.ns), orig == g_skinFastUpdateOrig ? "fast" : "full", static_cast<unsigned long long>(q.call), q.model, q.ec, q.f0, q.ra, q.sp, q.parent, q.locB[0], q.locB[1], q.locB[2], q.locB[12], q.locB[13],
                             q.locB[14], q.wldB[12], q.wldB[13], q.wldB[14], q.wldA[0], q.wldA[1], q.wldA[2], q.wldA[12], q.wldA[13],
                             q.wldA[14], q.parA[12], q.parA[13], q.parA[14], 10);
            }
            std::fflush(stderr);
        }
    }

    // TEMP diag HCTP_BONE_JITTER=1 (wrestler shake): before every CSkinModel::DrawChar 0x155dc0, the model's world
    // bone matrices (GetWorldMatrix 0x157050: CSkinModel+0xD0, 64 bytes a bone; GetBoneNum 0x157140: count at
    // (CSkinModel+0xC8)+0x14). Per bone, how far its position lands off the straight line through its previous two
    // frames (|p - 2*p1 + p2|): smooth motion stays near 0, a shake does not. Every 2 s per model: the 6 worst bones,
    // their spike counts and the axis lengths of the worst matrix (a broken rotation shows as non-unit axes).
    PS2Runtime::RecompiledFunction g_boneJitterOrig = nullptr, g_hantenOrig = nullptr, g_setBoneDataOrig = nullptr;
    std::atomic<uint32_t> g_hantenCalls{0u}, g_setBoneDataCalls{0u};
    struct BoneJitterModel
    {
        uint32_t bones = 0, frames = 0, ymp = 0;
        std::vector<float> p1, p2, last; // positions: 1 and 2 frames ago, and the latest
        std::vector<float> worst, axes;  // per bone: worst jump, and |x| |y| |z| axis lengths at it
        std::vector<uint32_t> spikes;
    };
    std::map<uint32_t, BoneJitterModel> g_boneJitter;
    // TEMP diag Nov 10 (SvR06 ramp jitter, part of HCTP_BONE_JITTER): camera vs body per tick. sceVu0CameraMatrix
    // 0x103ea8 (a0 out, a1 eye, a2 look dir, a3 up) stores the latest eye/dir; every DrawChar of the model with
    // HCTP_CAMROOT_BONES bones (default 71 = SvR06 Brock) counts whether its root bone and/or the camera moved since
    // its last draw. Theory: camera steps at 30 Hz while the body moves at 60 Hz (or the reverse) -> camOnly/rootOnly.
    PS2Runtime::RecompiledFunction g_camMatOrig = nullptr;
    float g_camEye[3]{}, g_camDir[3]{};
    uint32_t g_camCalls = 0;
    void camMatLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t e = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & 0x1FFFFFFu;
        const uint32_t d = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6])) & 0x1FFFFFFu;
        if (e + 12u < 0x2000000u && d + 12u < 0x2000000u)
        {
            std::memcpy(g_camEye, rdram + e, 12);
            std::memcpy(g_camDir, rdram + d, 12);
            ++g_camCalls;
        }
        g_camMatOrig(rdram, ctx, runtime);
    }
    void camRootProbe(uint8_t *rdram, R5900Context *ctx)
    {
        static const uint32_t wantBones = [] { const char *v = std::getenv("HCTP_CAMROOT_BONES"); return v ? uint32_t(std::atoi(v)) : 71u; }();
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        if (model < 0x100000u || model >= 0x1FFFE00u) return;
        uint32_t ymp = 0, bones = 0, world = 0;
        std::memcpy(&ymp, rdram + model + 0xC8u, 4);
        std::memcpy(&world, rdram + model + 0xD0u, 4);
        ymp &= 0x1FFFFFFu; world &= 0x1FFFFFFu;
        if (ymp < 0x100000u || ymp >= 0x1FFFFF0u) return;
        std::memcpy(&bones, rdram + ymp + 0x14u, 4);
        if (bones != wantBones || world < 0x100000u || world + 64u >= 0x2000000u) return;
        static float lastRoot[3]{}, lastEye[3]{}, lastDir[3]{};
        static uint32_t lastCamCalls = 0, n[4]{}, lines = 0, draws = 0;
        // Nov 10 v2: the 0x103ea8 hook never fires here (CSmackCamera::Update 0x158680 calls it by a direct jal at
        // 0x158704 -> recompiled call, not dispatched). Read the live camera instead: *(0x526E70) = CCamera, +0xE0 =
        // view matrix written by that call (row 3 = translation, column 2 = look axis), same matrix the pyro projects with.
        {
            uint32_t camp = 0;
            std::memcpy(&camp, rdram + 0x526E70u, 4);
            camp &= 0x1FFFFFFu;
            if (camp >= 0x100000u && camp + 0x120u < 0x2000000u)
            {
                float E[4][4];
                std::memcpy(E, rdram + camp + 0xE0u, 64);
                for (int k = 0; k < 3; ++k) { g_camEye[k] = E[3][k]; g_camDir[k] = E[k][2]; }
            }
        }
        float root[3];
        std::memcpy(root, rdram + world + 0x30u, 12);
        const bool rc = std::memcmp(root, lastRoot, 12) != 0;
        const bool cc = std::memcmp(g_camEye, lastEye, 12) != 0 || std::memcmp(g_camDir, lastDir, 12) != 0;
        ++n[(rc ? 1 : 0) | (cc ? 2 : 0)];
        ++draws;
        if (lines < 400u && (rc || cc))
        {
            ++lines;
            std::fprintf(stderr, "[camroot] d%u camcalls+%u root%c(%.3f %.3f %.3f) cam%c eye(%.3f %.3f %.3f) dir(%.4f %.4f %.4f)%c",
                         draws, g_camCalls - lastCamCalls, rc ? '*' : '=', root[0], root[1], root[2], cc ? '*' : '=',
                         g_camEye[0], g_camEye[1], g_camEye[2], g_camDir[0], g_camDir[1], g_camDir[2], 10);
        }
        lastCamCalls = g_camCalls;
        std::memcpy(lastRoot, root, 12); std::memcpy(lastEye, g_camEye, 12); std::memcpy(lastDir, g_camDir, 12);
        static auto last = std::chrono::steady_clock::now();
        const auto t = std::chrono::steady_clock::now();
        if (t - last >= std::chrono::seconds(2))
        {
            last = t;
            std::fprintf(stderr, "[camroot] 2s: none %u rootOnly %u camOnly %u both %u%c", n[0], n[1], n[2], n[3], 10);
            n[0] = n[1] = n[2] = n[3] = 0;
        }
    }
    void boneJitterLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        if (model >= 0x100000u && model < 0x1FFFE00u)
        {
            uint32_t ymp = 0, bones = 0, world = 0;
            std::memcpy(&ymp, rdram + model + 0xC8u, 4);
            std::memcpy(&world, rdram + model + 0xD0u, 4);
            ymp &= 0x1FFFFFFu;
            world &= 0x1FFFFFFu;
            if (ymp >= 0x100000u && ymp < 0x1FFFFF0u)
                std::memcpy(&bones, rdram + ymp + 0x14u, 4);
            if (bones > 0u && bones <= 256u && world >= 0x100000u && world + bones * 64u < 0x2000000u)
            {
                BoneJitterModel &m = g_boneJitter[model];
                if (m.bones != bones)
                {
                    m = BoneJitterModel{};
                    m.bones = bones;
                    m.p1.assign(bones * 3u, 0.0f);
                    m.p2.assign(bones * 3u, 0.0f);
                    m.last.assign(bones * 3u, 0.0f);
                    m.worst.assign(bones, 0.0f);
                    m.axes.assign(bones * 3u, 0.0f);
                    m.spikes.assign(bones, 0u);
                }
                m.ymp = ymp;
                // DrawChar can run more than once a frame for one model (shadow, reflections): only a changed pose counts.
                bool changed = false;
                std::vector<float> now(bones * 3u);
                for (uint32_t b = 0; b < bones; ++b)
                {
                    std::memcpy(&now[b * 3u], rdram + world + b * 64u + 0x30u, 12);
                    changed |= std::memcmp(&now[b * 3u], &m.last[b * 3u], 12) != 0;
                }
                if (changed)
                {
                    if (m.frames >= 2u)
                        for (uint32_t b = 0; b < bones; ++b)
                        {
                            float d2 = 0.0f;
                            for (int k = 0; k < 3; ++k)
                            {
                                const float a = now[b * 3u + k] - 2.0f * m.p1[b * 3u + k] + m.p2[b * 3u + k];
                                d2 += a * a;
                            }
                            const float jump = std::sqrt(d2);
                            if (jump > 0.5f)
                                ++m.spikes[b];
                            if (jump > m.worst[b])
                            {
                                m.worst[b] = jump;
                                for (int k = 0; k < 3; ++k)
                                {
                                    float row[3];
                                    std::memcpy(row, rdram + world + b * 64u + k * 16u, 12);
                                    m.axes[b * 3u + k] = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                                }
                            }
                        }
                    m.p2 = m.p1;
                    m.p1 = now;
                    m.last = now;
                    ++m.frames;
                    // Which memory flips A, B, A, B with the bones: 1 MB around the model (CSkinModel, its matrices
                    // and the CChar/motion objects allocated near it), word by word, on every new pose.
                    static uint32_t scanBase = 0;
                    static std::vector<uint32_t> s1, s2, alt;
                    static uint32_t scanFrames = 0;
                    // One model only (several are drawn each frame; HCTP_ALTSCAN_BONES, default 92 = the CAM model)
                    static const uint32_t scanBones = [] { const char *v = std::getenv("HCTP_ALTSCAN_BONES"); return v ? uint32_t(std::atoi(v)) : 92u; }();
                    if (bones == scanBones)
                    {
                    const uint32_t base = (model > 0x180000u ? model - 0x80000u : 0x100000u) & ~0xFFFu;
                    constexpr uint32_t kScanWords = 0x100000u / 4u;
                    if (base != scanBase || s1.size() != kScanWords || base + 0x100000u > 0x2000000u)
                    {
                        scanBase = base + 0x100000u <= 0x2000000u ? base : 0x1F00000u;
                        s1.assign(kScanWords, 0u);
                        s2.assign(kScanWords, 0u);
                        alt.assign(kScanWords, 0u);
                        scanFrames = 0;
                    }
                    const uint32_t *mem = reinterpret_cast<const uint32_t *>(rdram + scanBase);
                    for (uint32_t i = 0; i < kScanWords; ++i)
                    {
                        const uint32_t v = mem[i];
                        // An oscillation around a moving value: as floats, the step from the previous frame reverses
                        // the step before it, and is not tiny against the value.
                        if (scanFrames >= 2u && v != s1[i])
                        {
                            float fv, f1, f2;
                            std::memcpy(&fv, &v, 4);
                            std::memcpy(&f1, &s1[i], 4);
                            std::memcpy(&f2, &s2[i], 4);
                            const float d1 = fv - f1, d0 = f1 - f2;
                            if (std::isfinite(fv) && std::isfinite(f1) && std::isfinite(f2) && d1 * d0 < 0.0f &&
                                std::fabs(d1) > 1e-3f * (std::fabs(fv) + 1e-3f) && std::fabs(fv) < 1e7f)
                                ++alt[i];
                        }
                        s2[i] = s1[i];
                        s1[i] = v;
                    }
                    ++scanFrames;
                    if (scanFrames >= 122u)
                    {
                        // Report runs of words that alternated on most frames (only while the bones shake hard).
                        uint32_t top = 0;
                        for (uint32_t b = 0; b < bones; ++b)
                            top = std::max(top, m.spikes[b]);
                        if (top > 60u || true) // always (the spike counter is reset by the 2 s bone report)
                        {
                            uint32_t world0 = 0;
                            std::memcpy(&world0, rdram + model + 0xD0u, 4);
                            std::fprintf(stderr, "[altscan] model %06x world %06x ymp %06x base %06x:", model, world0 & 0x1FFFFFFu,
                                         m.ymp, scanBase);
                            uint32_t runs = 0;
                            for (uint32_t i = 0; i < kScanWords && runs < 40u;)
                            {
                                if (alt[i] < 90u)
                                {
                                    ++i;
                                    continue;
                                }
                                uint32_t j = i;
                                while (j < kScanWords && (alt[j] >= 90u || (j + 1u < kScanWords && alt[j + 1u] >= 90u)))
                                    ++j;
                                float a, b2;
                                std::memcpy(&a, &s1[i], 4);
                                std::memcpy(&b2, &s2[i], 4);
                                std::fprintf(stderr, " %06x+%x(%g|%g)", scanBase + i * 4u, (j - i) * 4u, a, b2);
                                ++runs;
                                i = j + 1u;
                            }
                            std::fprintf(stderr, "%c", 10);
                        }
                        std::fill(alt.begin(), alt.end(), 0u);
                        scanFrames = 2u;
                    }
                    }
                }
            }
        }
        camRootProbe(rdram, ctx);
        g_boneJitterOrig(rdram, ctx, runtime);
        static auto lastReport = std::chrono::steady_clock::now();
        const auto t = std::chrono::steady_clock::now();
        if (t - lastReport < std::chrono::seconds(2))
            return;
        lastReport = t;
        for (auto &[addr, m] : g_boneJitter)
        {
            if (m.frames < 3u)
                continue;
            std::vector<uint32_t> order(m.bones);
            for (uint32_t b = 0; b < m.bones; ++b)
                order[b] = b;
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return m.worst[a] > m.worst[b]; });
            std::fprintf(stderr, "[bonejit] model %06x bones %u frames %u (Hanten %u, SetBoneData %u calls):", addr, m.bones,
                         m.frames, g_hantenCalls.exchange(0u), g_setBoneDataCalls.exchange(0u));
            uint32_t boneTable = 0;
            if (m.ymp >= 0x100000u && m.ymp < 0x1FFFFF0u)
                std::memcpy(&boneTable, rdram + m.ymp + 0x20u, 4);
            boneTable &= 0x1FFFFFFu;
            for (uint32_t i = 0; i < 6u && i < m.bones; ++i)
            {
                const uint32_t b = order[i];
                // FindBone 0x150b10: a bone's name is the first 16 bytes of its 80-byte entry
                char name[17] = "?";
                if (boneTable >= 0x100000u && boneTable + (b + 1u) * 80u < 0x2000000u)
                {
                    std::memcpy(name, rdram + boneTable + b * 80u, 16);
                    name[16] = 0;
                }
                std::fprintf(stderr, " b%u %s jump %.2f spikes %u axes %.3f/%.3f/%.3f;", b, name, m.worst[b], m.spikes[b],
                             m.axes[b * 3u], m.axes[b * 3u + 1u], m.axes[b * 3u + 2u]);
            }
            std::fprintf(stderr, "%c", 10);
            std::fill(m.worst.begin(), m.worst.end(), 0.0f);
            std::fill(m.spikes.begin(), m.spikes.end(), 0u);
            m.frames = 2u;
        }
    }

    // TEMP diag, part of HCTP_BONE_JITTER: the shaking bones are the IK limbs (l/r_ashi, l/r_sune, r_te, r_kote_x) and
    // they alternate between two poses every frame. After each CCharIK::ActionLink 0x1b8860 call, the first 0x200
    // bytes of its object; a word that goes A, B, A, B on consecutive calls is counted. Every 2 s: the words that
    // alternated on most calls, with both values (as float and hex).
    PS2Runtime::RecompiledFunction g_ikAltOrig = nullptr;
    struct IkAltState
    {
        std::array<uint32_t, 128> s1{}, s2{};
        std::array<uint32_t, 128> alt{};
        uint32_t calls = 0;
    };
    std::map<uint32_t, IkAltState> g_ikAlt;
    void ikAltLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ik = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        // Foot (bone 89) of the watched model (HCTP_IKWATCH_MODEL, hex, default c40110 = the shaking CAM model):
        // local and world matrices right before and after every IK call, in a timestamped ring of 120, dumped when
        // ikring.go appears in the game folder.
        static const uint32_t s_model = []
        {
            const char *v = std::getenv("HCTP_IKWATCH_MODEL");
            return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : 0xc40110u;
        }();
        auto rdw = [&](uint32_t a) -> uint32_t { uint32_t v = 0; a &= 0x1FFFFFFu; if (a >= 0x100000u && a < 0x1FFFFFCu) std::memcpy(&v, rdram + a, 4); return v; };
        const uint32_t footLoc = (rdw(s_model + 0xCCu) & 0x1FFFFFFu) + 89u * 64u, footWld = (rdw(s_model + 0xD0u) & 0x1FFFFFFu) + 89u * 64u;
        struct IkRec { int64_t ns; uint32_t ik, sp; float locB[16], locA[16], wldB[3], wldA[3]; };
        static std::vector<IkRec> ikRing(120);
        static uint64_t ikN = 0;
        IkRec &rec = ikRing[ikN++ % ikRing.size()];
        rec.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        rec.ik = ik;
        rec.sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
        const bool footOk = footLoc > 0x100000u + 89u * 64u && footWld > 0x100000u + 89u * 64u && footLoc < 0x1FFFF00u && footWld < 0x1FFFF00u;
        if (footOk)
        {
            std::memcpy(rec.locB, rdram + footLoc, 64);
            std::memcpy(rec.wldB, rdram + footWld + 0x30u, 12);
        }
        g_ikAltOrig(rdram, ctx, runtime);
        if (footOk)
        {
            std::memcpy(rec.locA, rdram + footLoc, 64);
            std::memcpy(rec.wldA, rdram + footWld + 0x30u, 12);
        }
        if ((ikN & 31u) == 0u)
            if (FILE *go = std::fopen("ikring.go", "rb"))
            {
                std::fclose(go);
                std::remove("ikring.go");
                for (uint64_t i = ikN > ikRing.size() ? ikN - ikRing.size() : 0; i < ikN; ++i)
                {
                    const IkRec &q = ikRing[i % ikRing.size()];
                    std::fprintf(stderr, "[ikring] t %lld ik %06x sp %08x | foot local row0 %.4f %.4f %.4f -> %.4f %.4f %.4f row3 %.3f %.3f %.3f -> %.3f %.3f %.3f | world row3 %.3f %.3f %.3f -> %.3f %.3f %.3f%c",
                                 static_cast<long long>(q.ns), q.ik, q.sp, q.locB[0], q.locB[1], q.locB[2], q.locA[0], q.locA[1], q.locA[2], q.locB[12],
                                 q.locB[13], q.locB[14], q.locA[12], q.locA[13], q.locA[14], q.wldB[0], q.wldB[1], q.wldB[2], q.wldA[0], q.wldA[1],
                                 q.wldA[2], 10);
                }
                std::fflush(stderr);
            }
        if (ik < 0x100000u || ik > 0x1FFFE00u)
            return;
        IkAltState &s = g_ikAlt[ik];
        std::array<uint32_t, 128> now;
        std::memcpy(now.data(), rdram + ik, 0x200);
        if (s.calls >= 2u)
            for (uint32_t i = 0; i < 128u; ++i)
                if (now[i] == s.s2[i] && now[i] != s.s1[i])
                    ++s.alt[i];
        s.s2 = s.s1;
        s.s1 = now;
        ++s.calls;
        static auto lastReport = std::chrono::steady_clock::now();
        const auto t = std::chrono::steady_clock::now();
        if (t - lastReport < std::chrono::seconds(2))
            return;
        lastReport = t;
        for (auto &[addr, st] : g_ikAlt)
        {
            std::fprintf(stderr, "[ikalt] ik %06x calls %u:", addr, st.calls);
            for (uint32_t i = 0; i < 128u; ++i)
                if (st.calls > 20u && st.alt[i] * 2u > st.calls)
                {
                    float a, b;
                    std::memcpy(&a, &st.s1[i], 4);
                    std::memcpy(&b, &st.s2[i], 4);
                    std::fprintf(stderr, " +%03x %g|%g (%08x|%08x) x%u;", i * 4u, a, b, st.s1[i], st.s2[i], st.alt[i]);
                }
            std::fprintf(stderr, "%c", 10);
            st.alt.fill(0u);
            st.calls = 2u;
        }
    }

    // TEMP diag HCTP_IKFLIP=1 (leg twitch): CCharIK::ActionLink 0x1b8860 restarts a 9-frame target ease when a limb's
    // bone-tip index (+0x88+i*4) differs from +0x9c+i*4. Logs every change of those per IK object.
    PS2Runtime::RecompiledFunction g_ikFlipOrig = nullptr;
    struct IkTips { int32_t v[8]; };
    std::map<uint32_t, IkTips> g_ikFlipLast;
    void ikFlipLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ik = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        g_ikFlipOrig(rdram, ctx, runtime);
        if (ik < 0x100000u || ik > 0x1FFFE00u)
            return;
        IkTips now;
        std::memcpy(now.v, rdram + ik + 0x88u, 16);
        std::memcpy(now.v + 4, rdram + ik + 0x9cu, 16);
        static unsigned long long calls = 0, changes = 0;
        ++calls;
        auto [it, fresh] = g_ikFlipLast.try_emplace(ik, now);
        if (!fresh && std::memcmp(&it->second, &now, sizeof(now)) != 0)
        {
            ++changes;
            static int logged = 0;
            if (logged++ < 400)
            {
                int32_t ease;
                std::memcpy(&ease, rdram + ik + 0x16cu, 4);
                std::fprintf(stderr, "[ikflip] ik %06x tip %d %d %d %d | 9c %d %d %d %d | 16c %d (call %llu)%c", ik,
                             now.v[0], now.v[1], now.v[2], now.v[3], now.v[4], now.v[5], now.v[6], now.v[7], ease,
                             calls, 10);
            }
        }
        it->second = now;
        if (calls % 20000 == 0)
            std::fprintf(stderr, "[ikflip] calls=%llu changes=%llu objects=%zu%c", calls, changes, g_ikFlipLast.size(),
                         10);
    }
    // TEMP diag HCTP_IKLOG=1: see the hook install. "[iklog] acosf x=.. -> .. ra=.." (first 200 odd calls), and every
    // 600 calls a count line (calls, out of range, NaN results).
    PS2Runtime::RecompiledFunction g_ikAcosOrig = nullptr;
    uint64_t g_ikCalls = 0, g_ikOdd = 0, g_ikNan = 0;
    void ikAcosLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float x = ctx->f[12];
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        g_ikAcosOrig(rdram, ctx, runtime);
        const float y = ctx->f[0];
        ++g_ikCalls;
        const bool odd = !(x >= -1.0f && x <= 1.0f);
        const bool nan = std::isnan(y);
        g_ikOdd += odd;
        g_ikNan += nan;
        static uint32_t s_lines = 0;
        if ((odd || nan) && s_lines < 200u)
        {
            ++s_lines;
            uint32_t xb, yb;
            std::memcpy(&xb, &x, 4);
            std::memcpy(&yb, &y, 4);
            std::fprintf(stderr, "[iklog] acosf x=%.9g (%08x) -> %.9g (%08x) ra=0x%06x%c", x, xb, y, yb, ra, 10);
        }
        if (g_ikCalls % 600u == 0u)
            std::fprintf(stderr, "[iklog] calls=%llu out_of_range=%llu nan=%llu%c", static_cast<unsigned long long>(g_ikCalls),
                         static_cast<unsigned long long>(g_ikOdd), static_cast<unsigned long long>(g_ikNan), 10);
    }

    // TEMP diag (HCTP_TRACEFN="addr[:name],..."; up to 32): "[trfn] name #n a0 a1 a2 a3 ra -> v0 (ms)" for the first
    // 40 calls, then whenever (a0,a1,v0) changes (max 400 lines per function). v0 is wrong if the thread was
    // preempted inside the function (see CRILOG caveat) - trust entry args.
    struct TraceHook { std::string name; PS2Runtime::RecompiledFunction orig; uint64_t calls; uint64_t last; uint32_t lines; };
    TraceHook g_trHooks[32];
    template <int I>
    void trLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TraceHook &h = g_trHooks[I];
        auto reg = [ctx](int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); };
        const uint32_t a0 = reg(4), a1 = reg(5), a2 = reg(6), a3 = reg(7), ra = reg(31);
        h.orig(rdram, ctx, runtime);
        const uint32_t v0 = reg(2);
        ++h.calls;
        uint64_t sig = (uint64_t(a0) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(a1) << 17) ^ (uint64_t(v0) << 40) ^ ra;
        const bool dumpA0 = h.name.find('@') != std::string::npos; // name with '@': also dump 32 words at a0
        auto w32 = [rdram](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & 0x1FFFFFCu), 4); return v; };
        if (dumpA0)
            for (uint32_t k = 0; k < 32u; ++k)
                sig = (sig ^ w32(a0 + k * 4u)) * 0x100000001B3ull;
        const bool periodic = (h.calls % 600u) == 0u; // ~10 s at 60 calls/s: proves it is still being called
        if (h.calls > 40u && !periodic && (sig == h.last || h.lines >= 400u))
            return;
        h.last = sig;
        ++h.lines;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_criT0).count();
        char line[256];
        std::snprintf(line, sizeof(line), "[trfn] %s #%llu a0=%x a1=%x a2=%x a3=%x ra=%x -> v0=%x (%lld ms)", h.name.c_str(),
                      static_cast<unsigned long long>(h.calls), a0, a1, a2, a3, ra, v0, ms);
        std::cerr << line;
        if (dumpA0)
        {
            std::cerr << " [a0]";
            for (uint32_t k = 0; k < 32u; ++k)
            {
                char wb[12];
                std::snprintf(wb, sizeof(wb), " %x", w32(a0 + k * 4u));
                std::cerr << wb;
            }
        }
        if (h.name.find('$') != std::string::npos) // name with '$': also print the C string at a1
        {
            char text[64] = {};
            for (uint32_t k = 0; k + 1 < sizeof(text); ++k)
            {
                const char c = static_cast<char>(rdram[(a1 + k) & 0x1FFFFFFu]);
                if (!c) break;
                text[k] = (c >= 32 && c < 127) ? c : '.';
            }
            std::cerr << " str='" << text << "'";
        }
        std::cerr << std::endl;
    }
    template <int I>
    void trHook(PS2Runtime &runtime, uint32_t addr, const std::string &name)
    {
        g_trHooks[I] = {name, runtime.lookupFunction(addr), 0, ~0ull, 0};
        if (!g_trHooks[I].orig || !runtime.replaceFunction(addr, trLogged<I>))
            std::cerr << "[trfn] hook FAILED " << name << std::endl;
    }
    template <int I>
    void trHookN(PS2Runtime &runtime, int n, uint32_t addr, const std::string &name)
    {
        if constexpr (I < 32)
        {
            if (n == I)
                trHook<I>(runtime, addr, name);
            else
                trHookN<I + 1>(runtime, n, addr, name);
        }
    }
    void installTraceHooks(PS2Runtime &runtime)
    {
        const char *env = std::getenv("HCTP_TRACEFN");
        if (!env)
            return;
        std::string list(env);
        int n = 0;
        size_t pos = 0;
        while (pos < list.size() && n < 32)
        {
            size_t end = list.find(',', pos);
            if (end == std::string::npos)
                end = list.size();
            std::string item = list.substr(pos, end - pos);
            pos = end + 1;
            if (item.empty())
                continue;
            const size_t colon = item.find(':');
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16));
            const std::string name = colon == std::string::npos ? item : item.substr(colon + 1);
            trHookN<0>(runtime, n++, addr, name);
        }
        std::cerr << "[trfn] " << n << " hooks installed" << std::endl;
    }

    // ---- restored In-Game Options entries: IN-GAME MUSIC TRACK + COMMENTARY VOLUME (deleted in retail HCTP) ----
    // COptionFrame mode 2 (In-Game Options). Retail: 12 rows, param ids 0xa..0x13 + 0x16 (default) + 0x17 (decision);
    // labels = RCstrIDs (0x7e "IN-GAME MUSIC TRACK" and 0x83 "COMMENTARY VOLUME" survive unused in the RC table).
    // Music track = tagOptState byte 0x11 (frame +0x59), still copied to 0x4922c4 by the exhibition setup and read by
    // SndPlayMatchBGM (0 = random, n = track n) - only the menu row was gone. Commentary volume = spare byte 0x1b
    // (frame +0x63, inside the 0x20-byte option block saved with the system data), stored inverted (see clip gain).
    // New param ids 0x14 (music) / 0x18 (commentary). The popup is the game's own COptionWindow: DecisionNext runs as
    // for unused param 9 (a match-rule byte, frame +0x51) with its value list redirected, WindowEnd moves the result.
    constexpr uint32_t kOptMusic = 0x14u, kOptComm = 0x18u, kOptDonor = 9u;
    PS2Runtime::RecompiledFunction g_optOrigItemNum, g_optOrigItem, g_optOrigInit, g_optOrigStrList, g_optOrigIndexNo,
        g_optOrigDecisionNext, g_optOrigWindowEnd, g_optOrigSetDef;
    uint32_t g_optItems = 0, g_optMusicList = 0, g_optCommList = 0;
    struct OptRedir
    {
        bool active;
        uint32_t frame, row, id;
        uint8_t donorSaved;
    } g_optRedir = {};
    inline uint32_t optArg(R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); }
    inline uint8_t *optP(uint8_t *rdram, uint32_t a) { return rdram + (a & 0x1FFFFFFu); }
    inline uint32_t optR32(uint8_t *rdram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, optP(rdram, a), 4);
        return v;
    }
    inline void optW32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(optP(rdram, a), &v, 4); }
    uint32_t optCurId(uint8_t *rdram, uint32_t frame, uint32_t *row)
    {
        *row = optR32(rdram, optR32(rdram, frame + 0x18u) + 0x18u);
        return optR32(rdram, optR32(rdram, frame + 0x38u) + *row * 8u);
    }
    uint8_t optValue(uint8_t *rdram, uint32_t frame, uint32_t id)
    {
        return id == kOptMusic ? *optP(rdram, frame + 0x59u)
                               : static_cast<uint8_t>(3u - (*optP(rdram, frame + 0x63u) & 3u));
    }
    void optEnsureTables(uint8_t *rdram, PS2Runtime *runtime)
    {
        if (g_optItems)
            return;
        g_optItems = runtime->guestMalloc(14u * 8u);
        g_optMusicList = runtime->guestMalloc(8u + 8u * 4u);
        g_optCommList = runtime->guestMalloc(8u);
        static const uint32_t labels[14] = {0x79, 0x7a, 0x7b, 0x7c, 0x82, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x83, 0x84, 0x77, 0x5b};
        for (uint32_t i = 0; i < 14u; ++i)
        {
            optW32(rdram, g_optItems + i * 8u, 0u);
            optW32(rdram, g_optItems + i * 8u + 4u, labels[i]);
        }
        optW32(rdram, g_optMusicList, g_optMusicList + 8u); // RANDOM, TRACK1..TRACK7
        optW32(rdram, g_optMusicList + 4u, 8u);
        optW32(rdram, g_optMusicList + 8u, 0x8fu);
        for (uint32_t i = 0; i < 7u; ++i)
            optW32(rdram, g_optMusicList + 12u + i * 4u, 0x9cu + i);
        optW32(rdram, g_optCommList, optR32(rdram, 0x411020u + 0x10u * 8u)); // BGM VOLUME's OFF/MIN/NORMAL/MAX
        optW32(rdram, g_optCommList + 4u, optR32(rdram, 0x411020u + 0x10u * 8u + 4u));
        std::fprintf(stderr, "[hctp-opt] tables at items=%x music=%x comm=%x%c", g_optItems, g_optMusicList, g_optCommList, 10);
    }
    void optItemNum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // GetOptMenuItemNum 0x181a90
    {
        if (optArg(ctx, 4) == 2u)
        {
            returnValue(ctx, 14);
            return;
        }
        g_optOrigItemNum(rdram, ctx, runtime);
    }
    void optItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // GetOptMenuItem 0x181a30
    {
        if (optArg(ctx, 4) == 2u)
        {
            optEnsureTables(rdram, runtime);
            for (uint32_t i = 0; i < 14u; ++i)
                *optP(rdram, g_optItems + i * 8u) = 0u;
            returnValue(ctx, static_cast<int32_t>(g_optItems));
            return;
        }
        g_optOrigItem(rdram, ctx, runtime);
    }
    void optInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // COptionFrame::InitOptFrameParam 0x1837b0
    {
        const uint32_t frame = optArg(ctx, 4);
        g_optOrigInit(rdram, ctx, runtime);
        if (optR32(rdram, frame + 0x34u) == 2u && optR32(rdram, frame + 0x38u))
        {
            static const uint32_t ids[14] = {0xa, 0xb, 0xc, 0xd, 0xe, 0xf, kOptMusic, 0x10, 0x11, 0x12, kOptComm, 0x13, 0x16, 0x17};
            const uint32_t tab = optR32(rdram, frame + 0x38u);
            for (uint32_t i = 0; i < 14u; ++i)
            {
                optW32(rdram, tab + i * 8u, ids[i]);
                optW32(rdram, tab + i * 8u + 4u, 0u);
            }
            g_optRedir.active = false;
        }
    }
    void optStrList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // GetWindStringList 0x185a60
    {
        uint32_t id = optArg(ctx, 4);
        if (id == kOptDonor && g_optRedir.active)
            id = g_optRedir.id;
        if (id == kOptMusic || id == kOptComm)
        {
            optEnsureTables(rdram, runtime);
            returnValue(ctx, static_cast<int32_t>(id == kOptMusic ? g_optMusicList : g_optCommList));
            return;
        }
        g_optOrigStrList(rdram, ctx, runtime);
    }
    void optIndexNo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // COptionFrame::GetOptIndexNo 0x185150
    {
        const uint32_t id = optArg(ctx, 5);
        if (id == kOptMusic || id == kOptComm)
        {
            returnValue(ctx, optValue(rdram, optArg(ctx, 4), id));
            return;
        }
        g_optOrigIndexNo(rdram, ctx, runtime);
    }
    void optDecisionNext(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // COptionFrame::DecisionNext 0x184c00
    {
        const uint32_t frame = optArg(ctx, 4);
        uint32_t row = 0;
        const uint32_t id = optR32(rdram, frame + 0x34u) == 2u ? optCurId(rdram, frame, &row) : 0u;
        if (id == kOptMusic || id == kOptComm)
        {
            g_optRedir = {true, frame, row, id, *optP(rdram, frame + 0x51u)};
            *optP(rdram, frame + 0x51u) = optValue(rdram, frame, id);
            optW32(rdram, optR32(rdram, frame + 0x38u) + row * 8u, kOptDonor);
        }
        g_optOrigDecisionNext(rdram, ctx, runtime);
    }
    void optWindowEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // COptionFrame::WindowEnd 0x184eb0
    {
        const uint32_t frame = optArg(ctx, 4);
        g_optOrigWindowEnd(rdram, ctx, runtime);
        if (g_optRedir.active && g_optRedir.frame == frame && optR32(rdram, frame + 0x88u) == 0u)
        {
            const uint8_t v = *optP(rdram, frame + 0x51u);
            *optP(rdram, frame + 0x51u) = g_optRedir.donorSaved;
            optW32(rdram, optR32(rdram, frame + 0x38u) + g_optRedir.row * 8u, g_optRedir.id);
            if (g_optRedir.id == kOptMusic)
                *optP(rdram, frame + 0x59u) = v & 7u;
            else
                *optP(rdram, frame + 0x63u) = static_cast<uint8_t>(3u - (v & 3u));
            std::fprintf(stderr, "[hctp-opt] %s = %u%c", g_optRedir.id == kOptMusic ? "music track" : "commentary volume", v, 10);
            g_optRedir.active = false;
        }
    }
    void optSetDef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // COptionFrame::SetDefCategory 0x1852e0
    {
        const uint32_t frame = optArg(ctx, 4), mode = optArg(ctx, 5);
        g_optOrigSetDef(rdram, ctx, runtime);
        if (mode == 2u)
            *optP(rdram, frame + 0x63u) = 0u; // commentary MAX (music track 0 = RANDOM comes from the game's defaults)
    }
    // TEMP diag (HCTP_DTXLOG=<n>): CRI DTX_ExecHndl 0x3ca4f8 (EE<->IOP exchange per handle; movie PCM L/R) - handle,
    // state, sent count, IOP counter, and the first words of the exchange buffer when the IOP has replied
    PS2Runtime::RecompiledFunction g_dtxOrigExec = nullptr;
    void dtxExecLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int left = std::getenv("HCTP_DTXLOG") ? std::atoi(std::getenv("HCTP_DTXLOG")) : 0;
        const uint32_t h = optArg(ctx, 4);
        if (left > 0)
        {
            const uint32_t state = *optP(rdram, h + 1u), sent = optR32(rdram, h + 8u), buf = optR32(rdram, h + 12u),
                           rbuf = optR32(rdram, h + 20u), iopCnt = optR32(rdram, rbuf + 0x3cu), dest = optR32(rdram, h + 24u);
            if (state == 1u && static_cast<int32_t>(iopCnt) > static_cast<int32_t>(sent))
            {
                bool any = false;
                for (uint32_t i = 0; i < 16u; ++i)
                    any = any || optR32(rdram, buf + i * 4u) != 0u;
                if (!any)
                {
                    g_dtxOrigExec(rdram, ctx, runtime);
                    return;
                }
                --left;
                std::fprintf(stderr, "[dtx] h %x dest %x sent %u iop %u buf %x:", h, dest, sent, iopCnt, buf);
                for (uint32_t i = 0; i < 16u; ++i)
                    std::fprintf(stderr, " %08x", optR32(rdram, buf + i * 4u));
                std::fprintf(stderr, "%c", 10);
            }
        }
        g_dtxOrigExec(rdram, ctx, runtime);
    }

    // ---- NATIVE MOVIES (HCTP_MOVIES=native): CADX_CTRL's SFD channels play through the host FFmpeg player
    // (hctp_native_movie.cpp) instead of Sofdec on the emulated EE/IPU/IOP. Channel info = s_SFDInfo 0x525140 +
    // ch * 0x1ec: +0x8/+0xc channel width / height, +0x30 Sofdec handle, +0x34 the current frame's info (w +0x3c,
    // h +0x40), +0x84 path3 GS address word, +0xb4 status, +0xb8 draw mode (1 = UsrLoadMvFrm), +0xbc/+0xc4 path3 /
    // user packet buffers (MakePath3Tag), +0xcc the RGBA frame buffer, +0xd0/+0xd4 the frame callback and its argument,
    // byte +0x1e8 "a frame was drawn". The frame goes up through the same calls LoadImage 0x159ff0 makes.
    // Movie AFS partitions: 5-8 = MOVIE/NMV000-003.afs, 9 = MOVIE/LOGOS.afs; credits = movie/credits.sfd by name.
    constexpr uint32_t kSfdInfo = 0x525140u, kSfdStride = 0x1ecu;
    bool g_movieNative[4] = {};
    bool g_movieNativeImportedOnly = false;
    uint64_t g_movieLastStaleDrawTick[4] = {};
    bool g_movieHasStaleDrawTick[4] = {};
    std::chrono::steady_clock::time_point g_movieLastStatusPoll[4] = {};
    bool g_movieEndReported[4] = {};
    PS2Runtime::RecompiledFunction g_mvOrigPlayAfs, g_mvOrigPlayName, g_mvOrigStop, g_mvOrigPause, g_mvOrigContinue,
        g_mvOrigGetStat, g_mvOrigLoadImage;

    bool nativeMovieDrawDue(PS2Runtime *runtime, uint32_t ch)
    {
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        ch &= 3u;
        if (g_movieHasStaleDrawTick[ch] && g_movieLastStaleDrawTick[ch] == tick)
            return false;
        g_movieLastStaleDrawTick[ch] = tick;
        g_movieHasStaleDrawTick[ch] = true;
        return true;
    }

    uint32_t guestCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, std::initializer_list<uint32_t> args)
    {
        // A back-edge checkpoint inside the guest callee sets t_eeCheckpointHit; leaking it out of this native stub made
        // dispatchGuestBranch treat the stub's normal return (pc == entry) as a paused loop and call it again forever
        // (THQ logo: LoadImage repeated millions of times, the title sequence never advanced). Keep the caller's value.
        const bool savedHit = t_eeCheckpointHit;
        struct RestoreHit { bool v; ~RestoreHit() { t_eeCheckpointHit = v; } } restoreHit{savedHit};
        const R5900Context saved = *ctx;
        int r = 4;
        for (uint32_t a : args)
            ctx->r[r++] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a)));
        ctx->pc = addr;
        runtime->lookupFunction(addr)(rdram, ctx, runtime);
        const uint32_t ret = optArg(ctx, 2);
        *ctx = saved;
        return ret;
    }
    // A guest call from native code that runs to the end: no EE checkpoint may stop the callee part way (the native
    // caller cannot resume it - the CREDITS plate was left half built / never hidden or unregistered, then a hang).
    uint32_t ppvCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, std::initializer_list<uint32_t> args)
    {
        const bool saved = t_eeNoCheckpoint;
        t_eeNoCheckpoint = true;
        const uint32_t ret = guestCall(rdram, ctx, runtime, addr, args);
        t_eeNoCheckpoint = saved;
        return ret;
    }
    // Pay-Per-View slice 1 (Oct 27): main menu EXHIBITION / CREATE / SEASON / PPV MODE / ONLINE / OPTIONS / CREDITS.
    // TopMenu (0x2a0 bytes; ctor 0x138950, SimpleSelectFrame: table +0x1c, count +0x20, select +0x24) keeps 5 plate
    // pointers at +0x284 and a "decided on Exhibition" byte at +0x298; plates 5-6 go in +0x298/+0x29c, so plate 5 is
    // 256-aligned (that byte writes 0 into its low byte). The stock ChangeIntroduction builds plates 0-4 from the fixed
    // table 0x40EFB0, so its entries 3-4 show PPV MODE / ONLINE while a TopMenu lives (dtor restores); plates 5-6
    // (OPTIONS, CREDITS with its lock flag) are owned here. Their blocks are zeroed before the ctor: TopMenuPlate leaves
    // many fields unset (stock plates sit on fresh zeroed page memory), and recycled bytes there made SetAnimKey(1)
    // decompress from a garbage source (CBpe::Expand ran off the end of RAM).
    // The stock TopMenu methods can stop at an EE checkpoint part way (the scheduler resumes them later), e.g. inside a
    // stock plate's SetAnimKey -> CBpe::Expand, which works on shared state: guest calls of ours made then broke the
    // paused Expand (it ran off the end of RAM when resumed). So ours run only once the hooked original has returned
    // (pc == ra), or on the next fresh hook entry.
    // PPV MODE reuses CreateMenu (4 items, strings 87-90 shown as the PPV strings while g_ppvMenu) under
    // CreateTopMenuSequence, whose LoopEndAct 0x186fa0 is routed here. Play sample / Create / Play your PPV are stubs
    // (submenu reopens); BACK or cancel -> main menu. ONLINE is a stub that reopens the main menu.
    // RCstrID table: 830 entries at 0x40DF70 (GetRCString 0x12fd60); ids 830+ are ours. The menu font has no '-'.
    PS2Runtime::RecompiledFunction g_origGetRCString = nullptr, g_origTopMenuCtor = nullptr, g_origTopMenuIntro = nullptr,
                                   g_origTopMenuDecision = nullptr, g_origTopMenuAnimEnd = nullptr, g_origTopMenuDtor = nullptr,
                                   g_origTopSeqLoopEnd = nullptr, g_origCreateSeqLoopEnd = nullptr;
    constexpr uint32_t kPpvStrBase = 830u; // 830 PPV MODE, 831-834 submenu items, 835 ONLINE
    const char *const kPpvStrings[] = {"PPV MODE", "PLAY SAMPLE PPV", "CREATE A PPV", "PLAY YOUR PPV", "BACK", "ONLINE"};
    constexpr uint32_t kPpvStrCount = 6u;
    constexpr uint32_t kTopItems = 7u, kHostPlates = 2u;
    constexpr uint32_t kPlateSize = 0xd0u; // ChangeIntroduction's operator new size
    uint32_t g_ppvStrAddr[kPpvStrCount] = {};
    uint32_t g_ppvTopTable = 0u, g_ppvPlates[kHostPlates] = {}, g_ppvPlateRaw[kHostPlates] = {}, g_ppvPlateOwner = 0u;
    uint32_t g_ppvCreditsFlag = 0u;
    bool g_ppvMenu = false, g_ppvHidePending = false;
    enum class PpvMode { None, Create, Play }; // PPV cards (slice 2, below)
    PpvMode g_ppvMode = PpvMode::None;
    bool g_ppvForceExhibition = false; // making a card: the next main menu decides EXHIBITION at once
    // ---- PPV CREATE 2 (Nov 4, HCTP_PPV_CREATE2=0 = slice-2 flow). SvR06 Create port (notes\PPV_HOW_IT_WORKS_FROM_CODE
    // _20261104.md s7-8.2): CREATE A PPV opens the CARD menu (CreateMenu re-skin, level 2: MATCH 1-3 + SAVE PPV).
    // A match = stock Exhibition match type -> stock roster select; WrestlerSelectEnd 0x1321a0 decided -> we run
    // ExshibitionMenuSequence::SetCommon 0x130d10 (the real setup-block writer; OutPutRuleParam 0x181e70 is an empty
    // stub), store StPPVMatch + the 0x400 setup snapshot and end the sequence (0x1B) -> no arena, no matchup screen.
    // Any exit of that Exhibition (match stored or Back on its top menu) builds a TopMenuSequence; its LoopEndAct goes
    // straight back to the card menu (g_ppvReturnToCard). Back on the card menu -> PPV submenu.
    // Card file ppv/card_00.bin = "PPV2" + StPPVSaveData (434 B, SvR06 layout) + 2 pad + matchNum x 0x400 snapshots.
    enum class PpvLevel { Sub, Details, Card, Load, Show };
    PpvLevel g_ppvLevel = PpvLevel::Sub;
    int g_ppvEditMatch = -1;
    bool g_ppvReturnToCard = false;
    constexpr uint32_t kPpvSaveSize = 434u, kPpvMatchSize = 44u, kPpvCardMatches = 8u;
    // item 15: 8 matches like SvR, 3 rows per page; CIRCLE (Sub1) = next page, SQUARE (Sub2) = clear on the card / SHOW menus (HCTP_PPV_PAGES=0: 3 matches)
    int g_ppvPage = 0;
    bool ppvPages()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_PAGES"); return !(v && *v == '0'); }();
        return on;
    }
    int ppvPageCount() { return ppvPages() ? 3 : 1; }
    int ppvAbs(uint32_t row) { return g_ppvPage * 3 + static_cast<int>(row); }
    uint8_t g_ppvWork[kPpvSaveSize] = {};
    std::vector<uint8_t> g_ppvWorkSnap[8];
    std::string g_ppvRowNote; // card SAVE row message ("NO MATCHES YET")
    uint32_t g_ppvCardStr[4] = {};
    bool ppvDirectExhi() // build ExshibitionMenuSequence directly (item 16: ctor as guest code fixed the Nov 4 freeze); =0 = via main menu
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_DIRECT_EXHI"); return !(v && *v == '0'); }();
        return on;
    }
    // Leave Exhibition for the card: direct = self+0x14 0x15 (LoopEndAct calls GoGameFromMenu -> ppvGoGame builds the
    // card menu); old = 6 (TopMenuSequence -> ppvTopSeqLoopEnd -> card menu)
    bool g_ppvExitPending = false;
    void ppvExitToCard(uint8_t *rdram, uint32_t self)
    {
        g_ppvExitPending = ppvDirectExhi();
        optW32(rdram, self + 0x14u, g_ppvExitPending ? 0x15u : 6u);
    }
    bool g_ppvDetails = false, g_ppvDetailsBuilt = false, g_ppvDetailsDone = false;
    bool ppvDetailsOn()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_DETAILS"); return v && *v == '1'; }();
        return on;
    }
    bool ppvCreate2()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_CREATE2"); return !(v && *v == '0'); }();
        return on;
    }
    // ---- PPV DETAILS 2 (Nov 5 autorun item 7; HCTP_PPV_DETAILS2=0 off). SvR05 details rows on the CreateMenu re-skin
    // (4 fixed rows, ids 87-90): PPV NAME / ARENA / BRAND / MATCH CARD. CStringDraw::Show 0x163140 reads our text
    // buffer every frame, so X on a value row (CreateMenu::ChangeDecision 0x135b60 hook, original skipped -> menu stays
    // in input) cycles the value in place; UpdateRect 0x163230 re-measures it. Values live in g_ppvWork (StPPVSaveData:
    // name +0, arena +46, brand +47, commentary +48). Arena ids = SLUS name table 0x458738 (stage byte 0x4922C0).
    bool ppvDetails2()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_DETAILS2"); return !(v && *v == '0'); }();
        return on;
    }
    const char *const kPpvArenaNames[31] = {"SmackDown! A", "SmackDown! B", "SmackDown! C", "SmackDown! D", "RAW A", "RAW B",
        "RAW C", "RAW D", "Velocity", "HEAT", "SummerSlam", "Unforgiven", "No Mercy", "Survivor Series", "Armageddon",
        "No Way Out", "WrestleMania XIX", "Backlash", "Judgment Day", "Vengeance", "Rebellion", "Insurrextion", "Bad Blood",
        "Arena Back Entrance", "Parking Lot", "Training Room", "Times Square", "Outside Arena A", "Outside Arena B",
        "England Plaza B", "Royal Rumble"};
    // ring arenas only (23-29 = backstage areas)
    const uint8_t kPpvArenaCycle[] = {30, 15, 16, 17, 18, 19, 10, 11, 12, 13, 14, 22, 20, 21, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    struct PpvShow { const char *name; uint8_t arena, brand; }; // brand 0 RAW, 1 SMACKDOWN, 2 BOTH (2003 split)
    const PpvShow kPpvShows[] = {{"Royal Rumble", 30, 2}, {"No Way Out", 15, 2}, {"WrestleMania XIX", 16, 2},
        {"Backlash", 17, 0}, {"Judgment Day", 18, 1}, {"Bad Blood", 22, 0}, {"Vengeance", 19, 1}, {"SummerSlam", 10, 2},
        {"Unforgiven", 11, 0}, {"No Mercy", 12, 1}, {"Survivor Series", 13, 2}, {"Armageddon", 14, 2},
        {"Rebellion", 20, 1}, {"Insurrextion", 21, 0}, {"King of the Ring", 16, 2}, {"MY PPV", 0xFF, 2}};
    const char *const kPpvBrands[3] = {"RAW", "SMACKDOWN", "BOTH"};
    // menu plate font (TopMenuPlate CStringDraw) has capitals only and fits ~19 characters
    constexpr size_t kPpvPlateChars = 19u;
    std::string ppvPlateText(std::string t)
    {
        std::string o;
        for (char c : t)
            if (c != '!' && !(c == ' ' && !o.empty() && o.back() == ' '))
                o += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (o.size() > kPpvPlateChars)
            o.resize(kPpvPlateChars);
        return o;
    }
    // label + value; long values drop the label
    std::string ppvLabelled(const char *label, const std::string &v)
    {
        const std::string full = std::string(label) + " " + v;
        return ppvPlateText(full.size() <= kPpvPlateChars ? full : v);
    }
    void ppvDetailsText(int row, char *out, size_t n)
    {
        const uint8_t arena = g_ppvWork[46], brand = g_ppvWork[47] < 3u ? g_ppvWork[47] : 2u;
        char name[47] = {};
        std::memcpy(name, g_ppvWork, 46);
        std::string t;
        if (row == 0)
            t = ppvLabelled("PPV", name[0] ? name : "MY PPV");
        else if (row == 1)
            t = ppvLabelled("ARENA", arena < 31u ? kPpvArenaNames[arena] : "AS PICKED");
        else if (row == 2)
            t = ppvLabelled("BRAND", kPpvBrands[brand]);
        else
            t = "MATCH CARD";
        std::snprintf(out, n, "%s", t.c_str());
    }
    // commentary byte follows the arena like the hctp-pbp arena rule (Raw arenas -> JR & King = 0, else Cole & Tazz = 1)
    void ppvSetCommentary()
    {
        const uint8_t a = g_ppvWork[46];
        g_ppvWork[48] = (a >= 4u && a <= 7u) || a == 9u || a == 11u || a == 14u || a == 21u || a == 22u ? 0u : 1u;
    }
    void ppvDetailsCycle(int row)
    {
        if (row == 0)
        {
            char name[47] = {};
            std::memcpy(name, g_ppvWork, 46);
            size_t i = 0, n = sizeof(kPpvShows) / sizeof(kPpvShows[0]);
            while (i < n && std::strcmp(kPpvShows[i].name, name) != 0)
                ++i;
            const PpvShow &next = kPpvShows[i < n ? (i + 1) % n : 0];
            std::memset(g_ppvWork, 0, 46);
            std::memcpy(g_ppvWork, next.name, std::strlen(next.name));
            g_ppvWork[46] = next.arena;
            g_ppvWork[47] = next.brand;
        }
        else if (row == 1)
        {
            size_t i = 0, n = sizeof(kPpvArenaCycle);
            while (i < n && kPpvArenaCycle[i] != g_ppvWork[46])
                ++i;
            g_ppvWork[46] = i + 1 < n ? kPpvArenaCycle[i + 1] : (i + 1 == n ? 0xFFu : kPpvArenaCycle[0]);
        }
        else if (row == 2)
            g_ppvWork[47] = static_cast<uint8_t>((g_ppvWork[47] + 1u) % 3u);
        ppvSetCommentary();
    }
    // SvR06 createppvdata.o (Create.bin): CPPV_RosterDataInit 0x596980, MatchDataInit 0x5969A0, DataInit 0x596A40,
    // DefaultSet 0x596AC0. RandomMatch not ported (needs HCTP GetGameTypeData/MakeRosterList) -> matches start empty.
    void ppvRosterInit(uint8_t *r) { r[0] = 0x7Bu; r[1] = r[2] = r[3] = 0u; }
    void ppvMatchInit(uint8_t *m)
    {
        std::memset(m, 0, kPpvMatchSize);
        m[0] = 0x5Eu; // rule none
        m[5] = 0xFFu; // beltNo
        for (int i = 0; i < 9; ++i)
            ppvRosterInit(m + 8 + i * 4);
    }
    void ppvDataInit(uint8_t *d)
    {
        std::memset(d, 0, kPpvSaveSize);
        for (int i = 0; i < 8; ++i)
            ppvMatchInit(d + 52 + i * kPpvMatchSize);
        std::memset(d + 404, 0x7B, 30);
    }
    void ppvDefaultSet(uint8_t *d)
    {
        ppvDataInit(d);
        std::memcpy(d, "MY PPV", 7);
        d[46] = 0xFFu; // arena: not set (details screen = next slice)
        d[47] = 2u;    // brand both
        d[48] = 1u;    // commentary Cole/Tazz
        d[50] = static_cast<uint8_t>(kPpvCardMatches);
        d[51] = 30u;
        for (auto &v : g_ppvWorkSnap)
            v.clear();
    }
    bool ppvMatchSet(int i) { return i >= 0 && i < 8 && g_ppvWork[52 + i * kPpvMatchSize] != 0x5Eu && !g_ppvWorkSnap[i].empty(); }
    // ---- save slots (Nov 5 item 9): ppv/card_NN.bin, NN 00..07 (SvR CreatePPVSys: SearchEmptyData = first free slot,
    // GetSaveNum = count). File = "PPV2" + StPPVSaveData (434) + 2 pad + matchNum (d[50]) x 0x400 setup snapshots.
    // Loads are validated (magic, size, matchNum <= 8, name NUL-terminated, rule bytes) -> a bad file is skipped.
    constexpr int kPpvSlots = 8, kPpvSamples = 3;
    int g_ppvSlot = 0;
    bool g_ppvSample = false; // list / play the sample cards ppv/sample_NN.bin (PLAY SAMPLE PPV, item 11)
    std::string ppvCardPath(int slot)
    {
        char p[32];
        std::snprintf(p, sizeof(p), g_ppvSample ? "ppv/sample_%02d.bin" : "ppv/card_%02d.bin", slot);
        return p;
    }
    bool ppvSaveCard2()
    {
        std::error_code ec;
        std::filesystem::create_directories("ppv", ec);
        const std::string path = ppvCardPath(g_ppvSlot), tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            if (!f)
                return false;
            const char pad[2] = {};
            f.write("PPV2", 4);
            f.write(reinterpret_cast<const char *>(g_ppvWork), kPpvSaveSize);
            f.write(pad, 2);
            for (uint32_t i = 0; i < g_ppvWork[50] && i < 8u; ++i)
            {
                std::vector<uint8_t> snap = g_ppvWorkSnap[i];
                snap.resize(0x400u, 0u);
                f.write(reinterpret_cast<const char *>(snap.data()), 0x400);
            }
            if (!f)
                return false;
        }
        std::filesystem::rename(tmp, path, ec);
        return !ec;
    }
    bool ppvLoadCard2(int slot)
    {
        ppvDefaultSet(g_ppvWork);
        std::error_code ec;
        const std::string path = ppvCardPath(slot);
        const auto size = std::filesystem::file_size(path, ec);
        std::ifstream f(path, std::ios::binary);
        char magic[4] = {};
        uint8_t tmp[kPpvSaveSize + 2];
        if (ec || !f || !f.read(magic, 4) || std::memcmp(magic, "PPV2", 4) != 0 || !f.read(reinterpret_cast<char *>(tmp), sizeof(tmp)))
            return false;
        const uint32_t num = tmp[50];
        bool ok = num <= 8u && size >= 4u + sizeof(tmp) + num * 0x400u && std::memchr(tmp, 0, 46) != nullptr;
        for (uint32_t i = 0; ok && i < num; ++i)
            ok = tmp[52 + i * kPpvMatchSize] <= 0x5Eu; // eRuleType or 0x5E none
        if (!ok)
        {
            std::cerr << "[ppv] " << path << " rejected (bad header)" << std::endl;
            ppvDefaultSet(g_ppvWork);
            return false;
        }
        std::memcpy(g_ppvWork, tmp, kPpvSaveSize);
        if (ppvPages())
            g_ppvWork[50] = 8u; // 3-match files (PPV2, matchNum 3) load and save back with 8 slots
        g_ppvPage = 0;
        for (uint32_t i = 0; i < num; ++i)
        {
            std::vector<uint8_t> snap(0x400u);
            if (!f.read(reinterpret_cast<char *>(snap.data()), 0x400))
                break;
            if (g_ppvWork[52 + i * kPpvMatchSize] != 0x5Eu)
                g_ppvWorkSnap[i] = std::move(snap);
        }
        g_ppvSlot = slot;
        return true;
    }
    std::vector<int> g_ppvSlotList; // saved, valid slots (YOUR PPVs level)
    size_t g_ppvSlotIdx = 0;
    bool g_ppvDeleteArmed = false;
    void ppvScanSlots()
    {
        g_ppvSlotList.clear();
        for (int i = 0; i < (g_ppvSample ? kPpvSamples : kPpvSlots); ++i)
            if (ppvLoadCard2(i))
                g_ppvSlotList.push_back(i);
        if (g_ppvSlotIdx >= g_ppvSlotList.size())
            g_ppvSlotIdx = 0;
        if (!g_ppvSlotList.empty())
            ppvLoadCard2(g_ppvSlotList[g_ppvSlotIdx]);
    }
    int ppvFreeSlot()
    {
        std::error_code ec;
        for (int i = 0; i < kPpvSlots; ++i)
            if (!std::filesystem::exists(ppvCardPath(i), ec))
                return i;
        return -1;
    }
    // the CreateMenu font draws only capitals and a few digits (4-8 come out as stubs, item 15 shots) -> Roman numerals
    std::string ppvNum(int n)
    {
        static const char *r[] = {"0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
        return n >= 0 && n <= 10 ? r[n] : "X+";
    }
    std::string ppvLoadText(int row)
    {
        if (g_ppvSlotList.empty())
            return row == 0 ? "NO SAVED PPV" : "";
        char name[47] = {};
        std::memcpy(name, g_ppvWork, 46);
        if (row == 0)
            return ppvPlateText(ppvNum(static_cast<int>(g_ppvSlotIdx) + 1) + " " + name);
        if (row == 1)
            return "PLAY";
        if (g_ppvSample)
            return "";
        if (row == 2)
            return "EDIT";
        return g_ppvDeleteArmed ? "X AGAIN TO DELETE" : "DELETE";
    }
    // the hooked original ran to its return (not stopped at a checkpoint)
    bool ppvOrigDone(R5900Context *ctx, uint32_t ra) { return ctx->pc == ra; }
    void ppvReturn(R5900Context *ctx, uint32_t v0)
    {
        SET_GPR_U32(ctx, 2, v0);
        ctx->pc = optArg(ctx, 31);
    }
    uint32_t ppvVirtual(uint8_t *rdram, uint32_t obj, uint32_t vtblOff, uint32_t off)
    {
        return optR32(rdram, optR32(rdram, obj + vtblOff) + off);
    }
    // card row (Nov 5 item 8): "1 LESNAR VS ROCK" (surnames: the plate fits ~19 capitals), 3+ wrestlers
    // "1 6-MAN KANE ...", empty "1 EMPTY", result after play (item 10) "1 WIN LESNAR"
    std::string g_ppvResult[8];
    std::string ppvCtlText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int row); // item 14 (below)
    bool ppvCtlCycle(int row);
    int ppvShowLastRow();
    std::string ppvCardRowText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int i)
    {
        if (i >= 3)
            return g_ppvRowNote.empty() ? "SAVE PPV" : g_ppvRowNote;
        i = ppvAbs(static_cast<uint32_t>(i));
        if (i >= 8)
            return "-";
        std::string t = ppvNum(i + 1) + " ";
        if (!ppvMatchSet(i))
            return t + "EMPTY";
        if (!g_ppvResult[i].empty())
            return ppvPlateText(t + g_ppvResult[i]);
        if (g_ppvLevel == PpvLevel::Show)
        {
            const std::string c = ppvCtlText(rdram, ctx, runtime, i);
            if (!c.empty())
                return ppvPlateText(t + c);
        }
        const uint8_t *m = g_ppvWork + 52 + i * kPpvMatchSize;
        std::vector<std::string> names;
        for (int j = 0; j < 6; ++j)
        {
            if (m[8 + j * 4] == 0x7Bu)
                continue;
            // GetWrestlerName 0x12cde0: leaf table lookup (ids < 0x56: 0x4B6450 + id*0x170 + 0xAC1A)
            const uint32_t nm = ppvCall(rdram, ctx, runtime, 0x12cde0u, {m[8 + j * 4], 0u});
            std::string n = nm ? reinterpret_cast<const char *>(optP(rdram, nm)) : "?";
            const size_t sp = n.find_last_of(' ');
            names.push_back(sp == std::string::npos || sp + 1 >= n.size() ? n : n.substr(sp + 1));
        }
        if (names.size() == 2u)
            t += names[0] + " VS " + names[1];
        else
        {
            t += ppvNum(static_cast<int>(names.size())) + "-WAY";
            for (const auto &n : names)
                t += " " + n;
        }
        return ppvPlateText(t);
    }
    // rewrite a CreateMenu row's text in place (CStringDraw::Show reads the buffer each frame) + re-measure it
    void ppvSetRowText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t menu, uint32_t row, const std::string &t)
    {
        const uint32_t buf = row < 4u ? g_ppvCardStr[row] : 0u;
        if (!buf || !menu)
            return;
        const std::string v = t.substr(0, 47);
        std::memcpy(optP(rdram, buf), v.c_str(), v.size() + 1u);
        const uint32_t plate = optR32(rdram, menu + 0x34u + row * 4u);
        const uint32_t sd = plate ? optR32(rdram, plate + 0xbcu) : 0u;
        if (sd)
            ppvCall(rdram, ctx, runtime, 0x163230u, {sd}); // CStringDraw::UpdateRect
    }
    std::string ppvShowNextText();
    void ppvGetRCString(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t id = optArg(ctx, 4);
        if (g_ppvMenu && g_ppvLevel == PpvLevel::Show && id >= 87u && id <= 90u)
        {
            uint32_t &buf = g_ppvCardStr[id - 87u];
            if (!buf)
                buf = runtime->guestMalloc(48u);
            const std::string t = id == 90u ? ppvShowNextText() : ppvCardRowText(rdram, ctx, runtime, static_cast<int>(id - 87u));
            std::memcpy(optP(rdram, buf), t.c_str(), t.size() + 1u);
            return ppvReturn(ctx, buf);
        }
        if (g_ppvMenu && g_ppvLevel == PpvLevel::Load && id >= 87u && id <= 90u)
        {
            uint32_t &buf = g_ppvCardStr[id - 87u];
            if (!buf)
                buf = runtime->guestMalloc(48u);
            const std::string t = ppvLoadText(static_cast<int>(id - 87u));
            std::memcpy(optP(rdram, buf), t.c_str(), t.size() + 1u);
            return ppvReturn(ctx, buf);
        }
        if (g_ppvMenu && g_ppvLevel == PpvLevel::Details && id >= 87u && id <= 90u)
        {
            uint32_t &buf = g_ppvCardStr[id - 87u];
            if (!buf)
                buf = runtime->guestMalloc(48u);
            char text[48];
            ppvDetailsText(static_cast<int>(id - 87u), text, sizeof(text));
            std::memcpy(optP(rdram, buf), text, std::strlen(text) + 1u);
            return ppvReturn(ctx, buf);
        }
        if (g_ppvMenu && g_ppvLevel == PpvLevel::Card && id >= 87u && id <= 90u)
        {
            uint32_t &buf = g_ppvCardStr[id - 87u];
            if (!buf)
                buf = runtime->guestMalloc(48u);
            const std::string t = ppvCardRowText(rdram, ctx, runtime, static_cast<int>(id - 87u));
            std::memcpy(optP(rdram, buf), t.c_str(), t.size() + 1u);
            return ppvReturn(ctx, buf);
        }
        if (g_ppvMenu && id >= 87u && id <= 90u)
            id = kPpvStrBase + 1u + (id - 87u);
        if (id < kPpvStrBase || id >= kPpvStrBase + kPpvStrCount)
            return g_origGetRCString(rdram, ctx, runtime);
        uint32_t &addr = g_ppvStrAddr[id - kPpvStrBase];
        if (!addr)
        {
            const char *s = kPpvStrings[id - kPpvStrBase];
            addr = runtime->guestMalloc(static_cast<uint32_t>(std::strlen(s)) + 1u);
            std::memcpy(optP(rdram, addr), s, std::strlen(s) + 1u);
        }
        ppvReturn(ctx, addr);
    }
    PS2Runtime::RecompiledFunction g_origCreateMenuDecision = nullptr;
    // CreateMenu::ChangeDecision 0x135b60 (called by SimpleSelectFrame::FromUI_OK after it stored the row in +0x24)
    void ppvCreateMenuDecision(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4);
        const uint32_t row = optR32(rdram, self + 0x24u);
        if (g_ppvMenu && g_ppvLevel == PpvLevel::Card && row == 3u)
        {
            // SvR CheckEntry / RevisionData subset: a card needs at least one match (rules + wrestlers come from the
            // stock Exhibition menus, which already only offer legal choices)
            bool any = false;
            for (int i = 0; i < 8; ++i)
                any = any || ppvMatchSet(i);
            if (!any)
            {
                g_ppvRowNote = "NO MATCHES YET";
                ppvSetRowText(rdram, ctx, runtime, self, 3u, g_ppvRowNote);
                g_ppvRowNote.clear();
                std::cerr << "[ppv] card: SAVE refused, no matches" << std::endl;
                return ppvReturn(ctx, 0u);
            }
        }
        if (g_ppvMenu && g_ppvLevel == PpvLevel::Show && row < 3u)
        {
            // item 14: X on an upcoming match cycles who player 1 controls: card names -> AS <each wrestler> -> WATCH
            if (ppvCtlCycle(ppvAbs(row)))
                ppvSetRowText(rdram, ctx, runtime, self, row, ppvCardRowText(rdram, ctx, runtime, static_cast<int>(row)));
            return ppvReturn(ctx, 0u);
        }
        if (g_ppvMenu && g_ppvLevel == PpvLevel::Load)
        {
            if (row == 0u && g_ppvSlotList.size() > 1u)
            {
                g_ppvSlotIdx = (g_ppvSlotIdx + 1u) % g_ppvSlotList.size();
                ppvLoadCard2(g_ppvSlotList[g_ppvSlotIdx]);
            }
            else if (g_ppvSample && row >= 2u)
                ;
            else if (row == 3u && !g_ppvSlotList.empty() && g_ppvDeleteArmed)
            {
                std::error_code ec;
                std::filesystem::remove(ppvCardPath(g_ppvSlotList[g_ppvSlotIdx]), ec);
                std::cerr << "[ppv] deleted " << ppvCardPath(g_ppvSlotList[g_ppvSlotIdx]) << std::endl;
                g_ppvDeleteArmed = false;
                ppvScanSlots();
            }
            else if (row == 3u && !g_ppvSlotList.empty())
                g_ppvDeleteArmed = true;
            else if ((row == 1u || (row == 2u && !g_ppvSample)) && !g_ppvSlotList.empty())
            {
                g_ppvDeleteArmed = false;
                return g_origCreateMenuDecision(rdram, ctx, runtime); // PLAY / EDIT end the menu
            }
            if (row != 3u)
                g_ppvDeleteArmed = false;
            for (uint32_t r = 0; r < 4u; ++r)
                ppvSetRowText(rdram, ctx, runtime, self, r, ppvLoadText(static_cast<int>(r)));
            return ppvReturn(ctx, 0u);
        }
        if (!g_ppvMenu || g_ppvLevel != PpvLevel::Details || row >= 3u)
            return g_origCreateMenuDecision(rdram, ctx, runtime);
        ppvDetailsCycle(static_cast<int>(row));
        char text[48];
        ppvDetailsText(static_cast<int>(row), text, sizeof(text));
        ppvSetRowText(rdram, ctx, runtime, self, row, text);
        std::cerr << "[ppv] details: " << text << std::endl;
        ppvReturn(ctx, 0u);
    }
    // MenuInputAction::FromUI_Sub1 0x12e7c0 / FromUI_Sub2 0x12e7d0 (stock no-ops, a0 = frame + 0x14): on the card,
    // SQUARE / CIRCLE clears the selected match (SvR "delete match")
    PS2Runtime::RecompiledFunction g_origMenuSub1 = nullptr, g_origMenuSub2 = nullptr;
    bool ppvCardSub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *which)
    {
        const uint32_t frame = optArg(ctx, 4) - 0x14u;
        if (!g_ppvMenu || (g_ppvLevel != PpvLevel::Card && g_ppvLevel != PpvLevel::Show) ||
            optR32(rdram, frame) != 0x47add0u) // CreateMenu vtable
            return false;
        const uint32_t row = optR32(rdram, frame + 0x24u);
        std::cerr << "[ppv] card: " << which << " on row " << row << std::endl;
        if (ppvPages() && (which[3] == '1' || g_ppvLevel == PpvLevel::Show)) // next page of matches
        {
            int pages = ppvPageCount();
            if (g_ppvLevel == PpvLevel::Show) // only pages that hold card matches
            {
                const int last = ppvShowLastRow();
                pages = std::min(pages, last / 3 + 1);
            }
            g_ppvPage = (g_ppvPage + 1) % std::max(pages, 1);
            for (uint32_t r = 0; r < 3u; ++r)
                ppvSetRowText(rdram, ctx, runtime, frame, r, ppvCardRowText(rdram, ctx, runtime, static_cast<int>(r)));
            std::cerr << "[ppv] card: page " << g_ppvPage + 1 << std::endl;
            return true;
        }
        const int m = ppvAbs(row);
        if (g_ppvLevel == PpvLevel::Card && row < 3u && ppvMatchSet(m))
        {
            ppvMatchInit(g_ppvWork + 52 + m * kPpvMatchSize);
            g_ppvWorkSnap[m].clear();
            g_ppvResult[m].clear();
            ppvSetRowText(rdram, ctx, runtime, frame, row, ppvCardRowText(rdram, ctx, runtime, static_cast<int>(row)));
            std::cerr << "[ppv] card: match " << m + 1 << " cleared" << std::endl;
        }
        return true;
    }
    void ppvMenuSub1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!ppvCardSub(rdram, ctx, runtime, "SUB1"))
            return g_origMenuSub1(rdram, ctx, runtime);
        ppvReturn(ctx, 0u);
    }
    void ppvMenuSub2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!ppvCardSub(rdram, ctx, runtime, "SUB2"))
            return g_origMenuSub2(rdram, ctx, runtime);
        ppvReturn(ctx, 0u);
    }
    void ppvTopMenuCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4);
        g_origTopMenuCtor(rdram, ctx, runtime);
        if (!g_ppvTopTable)
            g_ppvTopTable = runtime->guestMalloc(kTopItems * 8u);
        const uint32_t stock = optR32(rdram, self + 0x1cu);
        g_ppvCreditsFlag = optR32(rdram, stock + 32u);
        // 3 PPV MODE, 4 ONLINE (stock plates 3-4), 5 OPTIONS, 6 CREDITS (host plates)
        const uint32_t extra[4][2] = {{0u, kPpvStrBase}, {0u, kPpvStrBase + 5u}, {0u, 5u}, {g_ppvCreditsFlag, 6u}};
        std::memcpy(optP(rdram, g_ppvTopTable), optP(rdram, stock), 3u * 8u);
        for (uint32_t i = 0; i < 4u; ++i)
        {
            optW32(rdram, g_ppvTopTable + (3u + i) * 8u, extra[i][0]);
            optW32(rdram, g_ppvTopTable + (3u + i) * 8u + 4u, extra[i][1]);
        }
        for (uint32_t i = 0; i < 2u; ++i)
        {
            optW32(rdram, 0x40EFB0u + (3u + i) * 8u, 0u);
            optW32(rdram, 0x40EFB0u + (3u + i) * 8u + 4u, extra[i][1]);
        }
        optW32(rdram, self + 0x1cu, g_ppvTopTable);
        optW32(rdram, self + 0x20u, kTopItems);
        optW32(rdram, self + 0x298u, 0u);
        if (g_ppvMode == PpvMode::Create && !g_ppvForceExhibition) // left Exhibition while making a card: card done
        {
            g_ppvMode = PpvMode::None;
            if (ppvCreate2() && g_ppvEditMatch >= 0) // Create 2: match stored or Back -> the card menu
            {
                g_ppvReturnToCard = true;
                g_ppvEditMatch = -1;
            }
        }
        g_ppvMenu = false;
    }
    // Builds plate 5 and lays out all 6 once the stock 5 exist. ChangeIntroduction can be paused at a loop checkpoint
    // and resumed later, so this also runs from IsAnimEnd (IntroductionAction polls it every frame).
    void ppvTopMenuLayout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self)
    {
        if (g_ppvPlateOwner == self)
            return;
        for (uint32_t i = 0; i < 5u; ++i)
            if (!optR32(rdram, self + 0x284u + i * 4u))
                return;
        const uint32_t first = optR32(rdram, self + 0x284u);
        const uint32_t ids[kHostPlates] = {5u, 6u}; // OPTIONS, CREDITS
        for (uint32_t k = 0; k < kHostPlates; ++k)
        {
            // the game's operator new on the current ymem page, like the stock plates (a host-heap plate made the next
            // screen's load crash); plate 5 rounded up to 256 (see above), the raw block kept for delete
            const uint32_t raw = ppvCall(rdram, ctx, runtime, 0x1001e0u, {k ? kPlateSize : kPlateSize + 0x100u});
            if (!raw)
                return;
            const uint32_t plate = k ? raw : (raw + 0xffu) & ~0xffu;
            std::memset(optP(rdram, plate), 0, kPlateSize);
            g_ppvPlateRaw[k] = raw;
            // TopMenuPlate(CFontDraw *, RCstrID, munuPacNoAnim 0x2712, munuPacNoTim 2) as ChangeIntroduction does
            ppvCall(rdram, ctx, runtime, 0x138450u, {plate, optR32(rdram, self + 0x7cu), ids[k], 0x2712u, 2u});
            rdram[plate + 0xc8u] = static_cast<uint8_t>(k ? g_ppvCreditsFlag : 0u);
            optW32(rdram, plate + 0x60u, optR32(rdram, first + 0x60u));
            optW32(rdram, plate + 0x68u, optR32(rdram, first + 0x68u));
            optW32(rdram, plate + 0x6cu, optR32(rdram, first + 0x6cu));
            g_ppvPlates[k] = plate;
            optW32(rdram, self + 0x298u + k * 4u, plate);
        }
        g_ppvPlateOwner = self;
        // stock y = 58 + 78*i for 5 plates; 7 plates 56 apart, ending where the 5th did
        for (uint32_t i = 0; i < kTopItems; ++i)
        {
            const float y = 34.0f + 56.0f * static_cast<float>(i);
            std::memcpy(optP(rdram, optR32(rdram, self + 0x284u + i * 4u) + 0x64u), &y, 4);
        }
        for (uint32_t plate : g_ppvPlates)
            ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, plate, 0u, 0x34u), {plate, 0u});
    }
    // TopMenuCursor::ChangeTarget 0x138900 (this, CBaseDraw *, RCstrID): TopMenu passes the stock table 0x40EFB0
    // entry [select], so selects 5-6 read past it (ids 0-1 = PRESS START / NOW LOADING in the green highlight)
    PS2Runtime::RecompiledFunction g_origCursorTarget = nullptr;
    void ppvCursorTarget(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_ppvPlateOwner && g_ppvTopTable)
        {
            const uint32_t sel = optR32(rdram, g_ppvPlateOwner + 0x24u);
            if (sel < kTopItems)
                SET_GPR_U32(ctx, 6, optR32(rdram, g_ppvTopTable + sel * 8u + 4u));
        }
        g_origCursorTarget(rdram, ctx, runtime);
    }
    void ppvTopMenuIntro(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4), ra = optArg(ctx, 31);
        g_origTopMenuIntro(rdram, ctx, runtime);
        if (ppvOrigDone(ctx, ra))
            ppvTopMenuLayout(rdram, ctx, runtime, self);
    }
    void ppvHidePlates(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_ppvHidePending || !g_ppvPlateOwner)
            return;
        g_ppvHidePending = false;
        for (uint32_t plate : g_ppvPlates)
            ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, plate, 0u, 0x34u), {plate, 1u});
    }
    void ppvTopMenuDecision(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4);
        const uint32_t ra = optArg(ctx, 31);
        g_origTopMenuDecision(rdram, ctx, runtime);
        if (g_ppvPlateOwner != self)
            return;
        g_ppvHidePending = true;
        if (ppvOrigDone(ctx, ra))
            ppvHidePlates(rdram, ctx, runtime);
    }
    void ppvTopMenuAnimEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4);
        ppvTopMenuLayout(rdram, ctx, runtime, self);
        ppvHidePlates(rdram, ctx, runtime);
        const uint32_t ra = optArg(ctx, 31);
        g_origTopMenuAnimEnd(rdram, ctx, runtime);
        // the stock check loops over plates 0-4 only; wait for ours too (+0x89 = anim end) so OPTIONS / CREDITS finish
        // their decision fade before the next screen
        if (g_ppvPlateOwner == self && ppvOrigDone(ctx, ra) && (optArg(ctx, 2) & 0xffu))
            for (uint32_t plate : g_ppvPlates)
                if (!rdram[plate + 0x89u])
                    SET_GPR_U32(ctx, 2, 0u);
    }
    void ppvTopMenuDtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4);
        if (self && g_ppvPlateOwner == self)
        {
            g_ppvPlateOwner = 0u;
            g_ppvHidePending = false;
            optW32(rdram, self + 0x298u, 0u);
            optW32(rdram, self + 0x29cu, 0u);
            optW32(rdram, 0x40EFB0u + 24u, 0u);
            optW32(rdram, 0x40EFB0u + 28u, 5u);
            optW32(rdram, 0x40EFB0u + 32u, g_ppvCreditsFlag);
            optW32(rdram, 0x40EFB0u + 36u, 6u);
            for (uint32_t k = 0; k < kHostPlates; ++k)
            {
                uint32_t &plate = g_ppvPlates[k];
                if (!plate)
                    continue;
                ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, plate, 0u, 8u), {plate, 0u});
                ppvCall(rdram, ctx, runtime, 0x100200u, {g_ppvPlateRaw[k]}); // operator delete
                plate = 0u;
            }
        }
        g_origTopMenuDtor(rdram, ctx, runtime);
    }
    // Ends a sequence and its menu the way the stock LoopEndActs do, then starts `ctor` (size 0x10 sequences).
    // item 16: ExshibitionMenuSequence ctor 0x1305a0 calls Next(0), which builds the first frame (+0xC); run through
    // ppvCall (no EE checkpoints) that part was lost -> +0xC NULL -> LoopEndAct 0x1313e0 jalr through a zero vtable
    // (logs\ppv16.err). Like stock TopMenuSequence::LoopEndAct 0x1382bc: page 1, new 0x164, ctor AS GUEST CODE, then the
    // old page back - the ctor returns into OutPutRuleParam 0x181e70 (empty stub, hooked) which restores the page.
    bool g_ppvTramp = false;
    uint32_t g_ppvTrampRa = 0u, g_ppvTrampSp = 0u, g_ppvTrampPage = 0u;
    bool ppvTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_ppvTramp || optArg(ctx, 29) != g_ppvTrampSp)
            return false;
        g_ppvTramp = false;
        ppvCall(rdram, ctx, runtime, 0x140be0u, {g_ppvTrampPage}); // ymemChangePage back
        std::cerr << "[ppv] Exhibition ctor ran as guest code, page restored" << std::endl;
        SET_GPR_U32(ctx, 31, g_ppvTrampRa);
        ctx->pc = g_ppvTrampRa;
        return true;
    }
    void ppvNextSequenceGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, uint32_t menu,
                              uint32_t ctor, uint32_t size)
    {
        ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, menu, 0u, 8u), {menu, 1u});
        optW32(rdram, self + 0xcu, 0u);
        ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, self, 8u, 8u), {self, 1u});
        g_ppvTrampPage = optR32(rdram, 0x500f00u);
        ppvCall(rdram, ctx, runtime, 0x140be0u, {1u}); // ymemChangePage
        const uint32_t next = ppvCall(rdram, ctx, runtime, 0x1001e0u, {size});
        if (!next)
        {
            ppvCall(rdram, ctx, runtime, 0x140be0u, {g_ppvTrampPage});
            return ppvReturn(ctx, 0u);
        }
        std::memset(optP(rdram, next), 0, size);
        g_ppvTramp = true;
        g_ppvTrampRa = optArg(ctx, 31);
        g_ppvTrampSp = optArg(ctx, 29);
        SET_GPR_U32(ctx, 4, next);
        SET_GPR_U32(ctx, 31, 0x181e70u);
        ctx->pc = ctor; // guest tail call
    }
    void ppvNextSequence(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, uint32_t menu, uint32_t ctor,
                         uint32_t size = 0x10u, std::initializer_list<uint32_t> ctorArgs = {})
    {
        ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, menu, 0u, 8u), {menu, 1u});
        optW32(rdram, self + 0xcu, 0u);
        ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, self, 8u, 8u), {self, 1u});
        const uint32_t page = optR32(rdram, 0x500f00u);
        ppvCall(rdram, ctx, runtime, 0x140be0u, {1u}); // ymemChangePage
        const uint32_t next = ppvCall(rdram, ctx, runtime, 0x1001e0u, {size});
        if (next)
        {
            std::memset(optP(rdram, next), 0, size);
            if (ctorArgs.size() == 2u)
                ppvCall(rdram, ctx, runtime, ctor, {next, *ctorArgs.begin(), *(ctorArgs.begin() + 1)});
            else
                ppvCall(rdram, ctx, runtime, ctor, {next});
        }
        ppvCall(rdram, ctx, runtime, 0x140be0u, {page});
        ppvReturn(ctx, 0u);
    }
    // TopMenuSequence::LoopEndAct 0x1381d0: item 3 PPV MODE -> CreateTopMenuSequence (result 1) in PPV mode, item 4
    // ONLINE (stub) -> main menu again, items 5-6 -> the stock OPTIONS / CREDITS results 3-4
    void ppvTopSeqLoopEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4);
        const uint32_t menu = optR32(rdram, self + 0xcu);
        if (g_ppvReturnToCard && menu)
        {
            if (!(ppvCall(rdram, ctx, runtime, 0x378bc0u, {}) & 0xffu) || // caOutGame::IsAllEnd
                !(ppvCall(rdram, ctx, runtime, 0x3aa380u, {}) & 0xffu))   // CrowdSignRead::CanUse
                return ppvReturn(ctx, 0u);
            g_ppvReturnToCard = false;
            g_ppvMenu = true;
            if (g_ppvLevel != PpvLevel::Show)
                g_ppvLevel = PpvLevel::Card;
            std::cerr << "[ppv] back to the " << (g_ppvLevel == PpvLevel::Show ? "SHOW" : "card") << " menu" << std::endl;
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u); // CreateTopMenuSequence (card menu)
        }
        if (g_ppvForceExhibition && menu)
        {
            g_ppvForceExhibition = false;
            optW32(rdram, menu + 8u, 5u);     // MenuFrame state: ended
            rdram[menu + 0xcu] = 1u;          // decided
            optW32(rdram, menu + 0x24u, 0u);  // EXHIBITION
            return g_origTopSeqLoopEnd(rdram, ctx, runtime);
        }
        if (menu && optR32(rdram, menu + 8u) == 5u && rdram[menu + 0xcu])
        {
            const uint32_t sel = optR32(rdram, menu + 0x24u);
            if (sel == 3u)
            {
                g_ppvMenu = true;
                optW32(rdram, menu + 0x24u, 1u);
            }
            else if (sel == 4u)
            {
                if (!(ppvCall(rdram, ctx, runtime, 0x378bc0u, {}) & 0xffu) || // caOutGame::IsAllEnd
                    !(ppvCall(rdram, ctx, runtime, 0x3aa380u, {}) & 0xffu))   // CrowdSignRead::CanUse
                    return ppvReturn(ctx, 0u);
                return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x138030u); // TopMenuSequence
            }
            else if (sel >= 5u)
                optW32(rdram, menu + 0x24u, sel - 2u); // OPTIONS 3, CREDITS 4
        }
        g_origTopSeqLoopEnd(rdram, ctx, runtime);
    }
    // PPV slice 2: cards. A card is up to 5 snapshots of the global match setup block (rule 0x4920C0, cast slots
    // 0x4921C0 + n*0x20, stage/game state 0x4922C0; dense code refs 0x492040..0x492440). CREATE A PPV runs the stock
    // Exhibition flow per match; GoGameFromMenu 0x1a8f80 (called by ExshibitionMenuSequence::LoopEndAct after it deleted
    // itself, on page 1) snapshots instead of starting and opens Exhibition again (PPV submenu after 5). PLAY plays
    // match 1 through GoGameFromMenu with the snapshot put back after OutPutRuleParam 0x181e70 (options -> rule);
    // ReturnMenuFromGame 0x1a9020 starts the next one, the last returns as usual. Cards: ppv_card.bin /
    // ppv_sample.bin in the game folder ("PPV1", u32 count, count * 0x400 bytes). See notes\PPV_MODE_DESIGN.md.
    constexpr uint32_t kPpvSetup = 0x492040u, kPpvSetupSize = 0x400u, kPpvMaxMatches = 5u;
    std::vector<std::vector<uint8_t>> g_ppvCard;
    size_t g_ppvPlayIdx = 0;
    uint8_t g_ppvPlayArena = 0xFFu;
    uint8_t g_ppvPlayBrand = 2u; // item 18: card BRAND row for the show being played (2 = both)
    // Nov 5 item 10: card row of each played match, result once per match, card shown between matches
    std::vector<int> g_ppvCardRow;
    bool g_ppvResultTaken = false;
    // ---- item 14 (Nov 5 round 2): per-match Play-as / WATCH (HCTP_PPV_PLAYAS=0 off). SetCommon 0x130d10 writes cast
    // slot j (0x4921C0 + j*0x20): +0 = pad / char index (human = its position 0..; CPU = first free index), +8 = 1 for
    // CPU else 0; referee etc. have +4 != 0. Choice per card match: -1 = as created, 0..n-1 = P1 plays wrestler k
    // (k-th slot with a code and +4 == 0), n = WATCH (all CPU). Applied to the snapshot copy in ppvOutPutRule.
    std::vector<int> g_ppvCtl;
    bool ppvPlayAs()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_PLAYAS"); return !(v && *v == '0'); }();
        return on;
    }
    std::vector<int> ppvCtlWrestlers(const std::vector<uint8_t> &m)
    {
        std::vector<int> w;
        for (int j = 0; j < 6; ++j)
        {
            const uint8_t *c = m.data() + 0x180 + j * 0x20;
            uint16_t code; std::memcpy(&code, c + 0xA, 2);
            uint32_t ctl; std::memcpy(&ctl, c + 4, 4);
            if (code != 0x639Cu && ctl == 0u)
                w.push_back(j);
        }
        return w;
    }
    int ppvCtlCardIdx(int row)
    {
        for (size_t k = 0; k < g_ppvCardRow.size() && k < g_ppvCard.size(); ++k)
            if (g_ppvCardRow[k] == row)
                return static_cast<int>(k);
        return -1;
    }
    int ppvShowLastRow()
    {
        int last = 0;
        for (int r : g_ppvCardRow)
            last = std::max(last, r);
        return last;
    }
    std::string ppvCtlText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int row)
    {
        const int k = ppvPlayAs() ? ppvCtlCardIdx(row) : -1;
        if (k < 0 || static_cast<size_t>(k) >= g_ppvCtl.size() || g_ppvCtl[k] < 0)
            return {};
        const auto w = ppvCtlWrestlers(g_ppvCard[k]);
        if (g_ppvCtl[k] >= static_cast<int>(w.size()))
            return "WATCH";
        uint16_t code; std::memcpy(&code, g_ppvCard[k].data() + 0x180 + w[g_ppvCtl[k]] * 0x20 + 0xA, 2);
        const uint32_t nm = ppvCall(rdram, ctx, runtime, 0x12cde0u, {code / 100u, 0u}); // GetWrestlerName
        std::string n = nm ? reinterpret_cast<const char *>(optP(rdram, nm)) : "?";
        const size_t sp = n.find_last_of(' ');
        return "AS " + (sp == std::string::npos || sp + 1 >= n.size() ? n : n.substr(sp + 1));
    }
    bool ppvCtlCycle(int row)
    {
        const int k = ppvPlayAs() ? ppvCtlCardIdx(row) : -1;
        if (k < 0 || static_cast<size_t>(k) < g_ppvPlayIdx) // played already
            return false;
        g_ppvCtl.resize(g_ppvCard.size(), -1);
        const int n = static_cast<int>(ppvCtlWrestlers(g_ppvCard[k]).size());
        g_ppvCtl[k] = g_ppvCtl[k] >= n ? -1 : g_ppvCtl[k] + 1;
        std::cerr << "[ppv] play-as: match " << row + 1 << " choice " << g_ppvCtl[k] << " of " << n << " (n = WATCH)" << std::endl;
        return true;
    }
    void ppvCtlApply(uint8_t *rdram, size_t k)
    {
        if (!ppvPlayAs() || k >= g_ppvCtl.size() || g_ppvCtl[k] < 0)
            return;
        const auto w = ppvCtlWrestlers(g_ppvCard[k]);
        const int human = g_ppvCtl[k] < static_cast<int>(w.size()) ? w[g_ppvCtl[k]] : -1;
        uint8_t next = human >= 0 ? 1u : 0u;
        for (int j = 0; j < 6; ++j)
        {
            uint8_t *c = rdram + 0x4921C0u + j * 0x20u;
            uint16_t code; std::memcpy(&code, c + 0xA, 2);
            if (code == 0x639Cu)
                continue;
            if (j == human)
                c[0] = 0u, c[8] = 0u;
            else
                c[0] = next++, c[8] = 1u;
        }
        std::cerr << "[ppv] play-as applied: human slot " << human << " (-1 = WATCH)" << std::endl;
    }
    int ppvCommentaryBrand()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_BRAND"); return !(v && *v == '0'); }();
        return on && g_ppvMode == PpvMode::Play && g_ppvPlayBrand < 2u ? int(g_ppvPlayBrand) : -1;
    }
    bool ppvCardBetween()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_CARD_BETWEEN"); return !(v && *v == '0'); }();
        return on;
    }
    std::string ppvShowNextText()
    {
        if (g_ppvPlayIdx >= g_ppvCard.size())
            return "END SHOW";
        if (g_ppvPlayIdx + 1u == g_ppvCard.size() && g_ppvCard.size() > 1u)
            return "MAIN EVENT";
        return "NEXT MATCH " + ppvNum(static_cast<int>(g_ppvPlayIdx) + 1);
    }
    PS2Runtime::RecompiledFunction g_origGoGame = nullptr, g_origOutPutRule = nullptr, g_origReturnFromGame = nullptr;
    // Guest tail call: heavy game functions (GoGameFromMenu) run as guest code with EE checkpoints, not in ppvCall
    // (no checkpoints there), returning straight to the hook's caller.
    void ppvTail(R5900Context *ctx, uint32_t fn)
    {
        ctx->pc = fn;
    }
    bool ppvSaveCard(const char *path)
    {
        std::ofstream f(path, std::ios::binary);
        if (!f)
            return false;
        const uint32_t count = static_cast<uint32_t>(g_ppvCard.size());
        f.write("PPV1", 4);
        f.write(reinterpret_cast<const char *>(&count), 4);
        for (const auto &m : g_ppvCard)
            f.write(reinterpret_cast<const char *>(m.data()), kPpvSetupSize);
        return static_cast<bool>(f);
    }
    bool ppvLoadCard(const char *path)
    {
        g_ppvCard.clear();
        std::ifstream f(path, std::ios::binary);
        char magic[4] = {};
        uint32_t count = 0;
        if (!f.read(magic, 4) || std::memcmp(magic, "PPV1", 4) != 0 || !f.read(reinterpret_cast<char *>(&count), 4))
            return false;
        for (uint32_t i = 0; i < count && i < kPpvMaxMatches; ++i)
        {
            std::vector<uint8_t> m(kPpvSetupSize);
            if (!f.read(reinterpret_cast<char *>(m.data()), kPpvSetupSize))
                break;
            g_ppvCard.push_back(std::move(m));
        }
        return !g_ppvCard.empty();
    }
    // ---- sample cards (Nov 5 item 11): host-generated from stock roster ids. No game data: a real singles setup
    // snapshot captured by the game (first singles match of any saved card, else the Oct 27 ppv_sample.bin) is the
    // template; only the two cast codes change (cast slot n = snapshot +0x180 + n*0x20, u16 code at +0xA =
    // roster id * 100 + attire; slot 2 = referee, 25500 = empty).
    struct PpvSampleCard { const char *name; uint8_t arena, brand; uint8_t ids[3][2]; };
    const PpvSampleCard kPpvSampleCards[kPpvSamples] = {
        {"WrestleMania XIX", 16, 2, {{0x17, 0x25}, {0x05, 0x2C}, {0x00, 0x01}}},  // Rey v Chavo, Angle v Lesnar, Rock v Austin
        {"SummerSlam", 10, 2, {{0x38, 0x04}, {0x2D, 0x06}, {0x02, 0x2F}}},       // Eddie v RVD, HBK v Jericho, HHH v Goldberg
        {"Survivor Series", 13, 2, {{0x26, 0x1A}, {0x07, 0x03}, {0x08, 0x09}}}}; // Cena v Show, Kane v Taker, Booker v Benoit
    bool ppvSingleSnap(const std::vector<uint8_t> &v)
    {
        auto code = [&](int n) { return static_cast<uint32_t>(v[0x18A + n * 0x20] | v[0x18B + n * 0x20] << 8); };
        return v.size() >= 0x400u && v[0x80] == 0u && code(1) < 25500u && code(3) == 25500u;
    }
    bool ppvFindTemplate(std::vector<uint8_t> &out)
    {
        const bool was = g_ppvSample;
        g_ppvSample = false;
        bool found = false;
        for (int i = 0; i < kPpvSlots && !found; ++i)
            if (ppvLoadCard2(i))
                for (int m = 0; m < 8 && !found; ++m)
                    if (ppvMatchSet(m) && ppvSingleSnap(g_ppvWorkSnap[m]))
                    {
                        out = g_ppvWorkSnap[m];
                        found = true;
                    }
        g_ppvSample = was;
        if (!found && ppvLoadCard("ppv_sample.bin")) // slice-2 card (raw 0x400 snapshots)
            for (const auto &m : g_ppvCard)
                if (!found && ppvSingleSnap(m))
                {
                    out = m;
                    found = true;
                }
        g_ppvCard.clear();
        return found;
    }
    void ppvMakeSamples()
    {
        std::error_code ec;
        g_ppvSample = true;
        bool missing = false;
        for (int i = 0; i < kPpvSamples; ++i)
            missing = missing || !std::filesystem::exists(ppvCardPath(i), ec);
        if (!missing)
            return;
        std::vector<uint8_t> tpl;
        if (!ppvFindTemplate(tpl))
        {
            std::cerr << "[ppv] samples: no singles template yet (save one PPV with a singles match first)" << std::endl;
            return;
        }
        g_ppvSample = true;
        for (int i = 0; i < kPpvSamples; ++i)
        {
            const PpvSampleCard &c = kPpvSampleCards[i];
            ppvDefaultSet(g_ppvWork);
            std::memset(g_ppvWork, 0, 46);
            std::memcpy(g_ppvWork, c.name, std::strlen(c.name));
            g_ppvWork[46] = c.arena;
            g_ppvWork[47] = c.brand;
            ppvSetCommentary();
            for (int m = 0; m < 3; ++m)
            {
                uint8_t *rec = g_ppvWork + 52 + m * kPpvMatchSize;
                rec[0] = 0u; // singles (the template's rule)
                g_ppvWorkSnap[m] = tpl;
                for (int w = 0; w < 2; ++w)
                {
                    rec[8 + w * 4] = c.ids[m][w];
                    const uint32_t code = c.ids[m][w] * 100u;
                    g_ppvWorkSnap[m][0x18A + w * 0x20] = static_cast<uint8_t>(code);
                    g_ppvWorkSnap[m][0x18B + w * 0x20] = static_cast<uint8_t>(code >> 8);
                }
            }
            g_ppvSlot = i;
            std::cerr << "[ppv] sample " << ppvCardPath(i) << (ppvSaveCard2() ? " made" : " FAILED") << std::endl;
        }
    }
    // Nov 5 PPV OPENING (Jordan: main-event feud cutscene + show pyro + announcers BEFORE the card). SvR05 plays its PPV
    // events (PPVEvent 0x5230b0 mode 0) through the normal event control, not the Season VM. HCTP's PPV openings are
    // Season scripts' EventInit(40NN) + EventInit(41NN) = /TKME/SEVP events (EventType 4 = Season machinery). We start
    // them like drDoramock::SetDoramockEvent 0x39c0c0 as EventType 3 (non-Season end path -> ReturnMenuFromGame) and only
    // switch the SEVP / EVTK loaders (ctors 0x165680 / 0x165ba0, EventType a1) to type 4 while the opening runs.
    // Trigger: match 1's GoGameFromMenu builds CSd5EventMng(2,0,0) (entrance) -> we turn it into (3,3,&param).
    bool g_ppvOpening = false, g_ppvOpeningActive = false;
    bool ppvOpeningOn()
    {
        static const bool on = [] { const char *v = std::getenv("HCTP_PPV_OPENING"); return v && *v == '1'; }();
        return on;
    }
    int ppvShowIndex()
    {
        if (const char *v = std::getenv("HCTP_PPV_OPENING_EVT"))
            return std::atoi(v);
        char name[47] = {};
        std::memcpy(name, g_ppvWork, 46);
        // Season opening ids 40NN: 4002 played the No Way Out arena (Jordan, Nov 5) -> NN = month order with
        // Armageddon = 0, Royal Rumble = 1, No Way Out = 2 ... (kPpvShows index + 1) % 12. Rebellion/Insurrextion/KOTR: unknown
        for (int i = 0; i < 12; ++i)
            if (std::strcmp(kPpvShows[i].name, name) == 0)
                return (i + 1) % 12;
        return 1;
    }
    PS2Runtime::RecompiledFunction g_origEvtMngCtor = nullptr, g_origEvtSysLoaderCtor = nullptr, g_origEvtMotLoaderCtor = nullptr;
    void ppvEvtSysLoaderCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (std::getenv("HCTP_EVTLOG"))
            std::cerr << "[evtlog] ppvEvtSysLoaderCtor type " << optArg(ctx, 5) << " arg " << optArg(ctx, 6) << std::endl;
        if (g_ppvOpeningActive && optArg(ctx, 5) == 3u)
        {
            // Nov 7 v2: Season PPV intro ground truth (Jordan's Backlash run, HCTP_EVTLOG) = EventType 1 events
            // 1999 (feud montage) + 1902 (arena/pyro opening) -> /EVTS/SEVP (Request 0x165790 type 1 = 0x45d570)
            SET_GPR_U32(ctx, 5, 1u);
            std::cerr << "[ppv] opening: event archive -> /EVTS/SEVP" << std::endl;
        }
        g_origEvtSysLoaderCtor(rdram, ctx, runtime);
    }
    void ppvEvtMotLoaderCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (std::getenv("HCTP_EVTLOG"))
            std::cerr << "[evtlog] ppvEvtMotLoaderCtor type " << optArg(ctx, 5) << " arg " << optArg(ctx, 6) << std::endl;
        if (g_ppvOpeningActive && optArg(ctx, 5) == 3u)
        {
            SET_GPR_U32(ctx, 5, 1u); // type 1, id >= 1000 -> /EVNT/%04d (Request 0x165cc0)
            std::cerr << "[ppv] opening: motions /EVNT/" << optArg(ctx, 6) << std::endl;
        }
        g_origEvtMotLoaderCtor(rdram, ctx, runtime);
    }
    // Scene driver (Season scripts drive opening scenes: nvJump 0x259220 = SetCurrentScene 0x167250 + SetCurrentFrame
    // 0x167270 + Restart 0x166f30; nvEventEnd 0x259150 = Stop 0x166eb0 + End 0x166fa0 on the CEventSystem). Without the
    // Season VM the first scene idles, so the host steps the scenes (SvR: X CONTINUE per beat; here a timer for now).
    uint32_t g_ppvEvtSys = 0u;
    int g_ppvEvtNo = 0, g_ppvSceneStep = 0, g_ppvSceneTimer = 0;
    PS2Runtime::RecompiledFunction g_origEvtSysCtor = nullptr, g_origEvtSysAction = nullptr;
    void ppvEvtSysCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_ppvOpeningActive)
        {
            g_ppvEvtSys = optArg(ctx, 4);
            g_ppvSceneStep = 0;
            g_ppvSceneTimer = 0;
            std::cerr << "[ppv] opening: event system #" << g_ppvEvtNo << " at 0x" << std::hex << g_ppvEvtSys << std::dec << std::endl;
            ++g_ppvEvtNo;
        }
        g_origEvtSysCtor(rdram, ctx, runtime);
    }
    void ppvEvtSysAction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sys = optArg(ctx, 4);
        if (std::getenv("HCTP_EVTLOG") && sys)
        {
            static uint32_t lastSys = 0u; static uint16_t lastScene = 0xFFFFu;
            uint16_t sc; std::memcpy(&sc, rdram + sys + 0x18u, 2);
            if (sys != lastSys || sc != lastScene)
            {
                uint16_t cnt; std::memcpy(&cnt, rdram + sys + 0x1Cu, 2);
                std::cerr << "[evtlog] event system 0x" << std::hex << sys << std::dec << " scene " << sc << " of " << cnt << std::endl;
                lastSys = sys; lastScene = sc;
            }
        }
        if (g_ppvOpeningActive && sys && sys == g_ppvEvtSys && rdram[sys + 0x54u])
        {
            static const int kFrames = [] { const char *v = std::getenv("HCTP_PPV_SCENE_FRAMES"); return v ? std::atoi(v) : 180; }();
            // feud montage 1999 (first event): scene order = Jordan's Season Backlash intro (HCTP_EVTLOG, Nov 5):
            // 130 0 120 113 115 105 126 1 112 121 100 (feud-type pick; one order for now); pyro 1902: plays as built
            static const int kFeud[] = {130, 0, 120, 113, 115, 105, 126, 1, 112, 121, 100};
            const bool feud = g_ppvEvtNo == 1;
            const int steps = feud ? static_cast<int>(sizeof(kFeud) / sizeof(kFeud[0])) : 0;
            if (feud && g_ppvSceneStep == 0 && g_ppvSceneTimer == 0)
                g_ppvSceneTimer = kFrames - 1; // first jump (130) right away, like the script
            const int limit = feud ? kFrames : kFrames * 6;
            if (++g_ppvSceneTimer >= limit)
            {
                g_ppvSceneTimer = 0;
                uint16_t count; std::memcpy(&count, rdram + sys + 0x1Cu, 2);
                if (g_ppvSceneStep < steps && kFeud[g_ppvSceneStep] < count)
                {
                    const int sc = kFeud[g_ppvSceneStep++];
                    ppvCall(rdram, ctx, runtime, 0x167250u, {sys, static_cast<uint32_t>(sc)});
                    ppvCall(rdram, ctx, runtime, 0x167270u, {sys, 0u});
                    ppvCall(rdram, ctx, runtime, 0x166f30u, {sys});
                    std::cerr << "[ppv] opening: scene " << sc << " of " << count << std::endl;
                }
                else
                {
                    std::cerr << "[ppv] opening: event end (scenes " << count << ")" << std::endl;
                    g_ppvEvtSys = 0u;
                    ppvCall(rdram, ctx, runtime, 0x166eb0u, {sys}); // Stop
                    ppvCall(rdram, ctx, runtime, 0x166fa0u, {sys}); // End
                    return ppvReturn(ctx, 0u);
                }
            }
        }
        g_origEvtSysAction(rdram, ctx, runtime);
    }
    void ppvEvtMngCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool evtlog = std::getenv("HCTP_EVTLOG") != nullptr;
        if (evtlog)
        {
            const uint32_t prm = optArg(ctx, 7);
            std::cerr << "[evtlog] CSd5EventMng type " << optArg(ctx, 5) << " mgr " << optArg(ctx, 6) << " param 0x" << std::hex << prm << std::dec;
            if (prm)
            {
                std::cerr << " count " << optR32(rdram, prm) << " ev " << optR32(rdram, prm + 4) << " " << optR32(rdram, prm + 8)
                          << " casts " << optR32(rdram, prm + 0x24) << " " << optR32(rdram, prm + 0x28) << " e34 " << optR32(rdram, prm + 0x34);
            }
            std::cerr << " setup casts";
            for (int i = 0; i < 8; ++i) { uint16_t v; std::memcpy(&v, rdram + 0x4921CAu + i * 0x20, 2); std::cerr << " " << v; }
            std::cerr << " stage " << int(rdram[0x4922C0u]) << std::endl;
        }
        if (!g_ppvOpening || optArg(ctx, 5) != 2u || optArg(ctx, 6) != 0u || g_ppvCard.empty())
            return g_origEvtMngCtor(rdram, ctx, runtime);
        g_ppvOpening = false;
        // main event = the card's last match: its first two wrestlers, different teams when possible
        const std::vector<uint8_t> &m = g_ppvCard.back();
        uint32_t c[2] = {0x639Cu, 0x639Cu};
        int team0 = -1, got = 0;
        for (int pass = 0; pass < 2 && got < 2; ++pass)
            for (int j = 0; j < 6 && got < 2; ++j)
            {
                const uint8_t *s = m.data() + 0x180 + j * 0x20;
                uint16_t code; std::memcpy(&code, s + 0xA, 2);
                uint32_t role; std::memcpy(&role, s + 4, 4);
                if (code == 0x639Cu || role != 0u)
                    continue;
                const uint32_t cc = code / 10u * 10u; // SetDoramockEvent: costume code /10*10
                if (got == 0)
                    c[got++] = cc, team0 = s[9];
                else if (cc != c[0] && (pass == 1 || s[9] != team0))
                    c[got++] = cc;
            }
        // v2 (Nov 7): Season PPV intro = 1999 feud montage + 1902 arena/pyro opening (EVNT motion banks exist only for
        // 1900/1901/1902/1904 -> 1902 = the PPV opening for every PPV, arena from the stage; UNVERIFIED for non-Backlash
        // PPVs). HCTP_PPV_OPENING_EVT=<n> overrides the 2nd event number.
        const uint32_t ev0 = 1999u;
        const uint32_t ev1 = std::getenv("HCTP_PPV_OPENING_EVT") ? static_cast<uint32_t>(std::atoi(std::getenv("HCTP_PPV_OPENING_EVT"))) : 1902u;
        const uint32_t page = optR32(rdram, 0x500f00u);
        ppvCall(rdram, ctx, runtime, 0x140be0u, {1u}); // ymemChangePage (common page: param must outlive this call)
        const uint32_t p = ppvCall(rdram, ctx, runtime, 0x1001e0u, {0x130u});
        ppvCall(rdram, ctx, runtime, 0x140be0u, {page});
        if (!p)
            return g_origEvtMngCtor(rdram, ctx, runtime);
        std::memset(optP(rdram, p), 0, 0x130u);
        optW32(rdram, p, 2u); // tagEVENTMNGPARAM: count; event numbers at +4 + k*4 (CSd5EventData::SetDoramockEventNo
        optW32(rdram, p + 4u, ev0); // 0x2ad170 copies param+4.. into its event list);
        optW32(rdram, p + 8u, ev1); // entries of 0x20 below: casts +0x24..+0x30, +0x34
        for (uint32_t k = 0; k < 2u; ++k)
        {
            const uint32_t e = p + k * 0x20u;
            optW32(rdram, e + 0x24u, c[0]);
            optW32(rdram, e + 0x28u, c[1]);
            optW32(rdram, e + 0x2Cu, 0x639Cu);
            optW32(rdram, e + 0x30u, 0x639Cu);
            optW32(rdram, e + 0x34u, k ? ev1 : ev0);
            optW32(rdram, e + 0x38u, 0x639Cu);
            optW32(rdram, e + 0x3Cu, 0x639Cu);
            optW32(rdram, e + 0x40u, 0x639Cu);
        }
        // casts into the setup cast table like SetDoramockEvent (OutPutRuleParam restores match 1's snapshot later)
        const uint32_t slots[] = {0x4921CAu, 0x4921EAu, 0x49220Au, 0x49222Au, 0x49224Au, 0x49226Au, 0x49228Au, 0x4922AAu};
        for (int i = 0; i < 8; ++i)
        {
            const uint16_t v = static_cast<uint16_t>(i < 2 ? c[i] : 0x639Cu);
            std::memcpy(rdram + slots[i], &v, 2);
        }
        SET_GPR_U32(ctx, 5, 3u); // EventType 3 (doramock: ends in ReturnMenuFromGame)
        SET_GPR_U32(ctx, 6, 3u); // SD5EVENTMNG_TYPE 3 (6 = MovesFromDramock)
        SET_GPR_U32(ctx, 7, p);
        g_ppvOpeningActive = true;
        g_ppvEvtNo = 0;
        g_ppvEvtSys = 0u;
        std::cerr << "[ppv] opening: events " << ev0 << " + " << ev1 << " casts " << c[0] << " " << c[1]
                  << " (stage " << int(rdram[0x4922C0u]) << ")" << std::endl;
        g_origEvtMngCtor(rdram, ctx, runtime);
    }
    void ppvGoGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_ppvExitPending)
        {
            g_ppvExitPending = false;
            g_ppvMode = PpvMode::None;
            g_ppvEditMatch = -1;
            g_ppvMenu = true;
            g_ppvLevel = PpvLevel::Card;
            std::cerr << "[ppv] back to the card menu (direct)" << std::endl;
            const uint32_t page = optR32(rdram, 0x500f00u);
            ppvCall(rdram, ctx, runtime, 0x140be0u, {1u}); // ymemChangePage
            const uint32_t next = ppvCall(rdram, ctx, runtime, 0x1001e0u, {0x10u});
            if (next)
            {
                std::memset(optP(rdram, next), 0, 0x10u);
                ppvCall(rdram, ctx, runtime, 0x186e50u, {next}); // CreateTopMenuSequence = card menu
            }
            ppvCall(rdram, ctx, runtime, 0x140be0u, {page});
            return ppvReturn(ctx, 0u);
        }
        if (g_ppvMode != PpvMode::Create)
            return g_origGoGame(rdram, ctx, runtime);
        ppvCall(rdram, ctx, runtime, 0x181e70u, {}); // OutPutRuleParam, as GoGameFromMenu does first
        g_ppvCard.emplace_back(optP(rdram, kPpvSetup), optP(rdram, kPpvSetup) + kPpvSetupSize);
        ppvSaveCard("ppv_card.bin");
        std::cerr << "[ppv] card match " << g_ppvCard.size() << " saved (rule 0x" << std::hex
                  << optR32(rdram, 0x4920C0u) << " stage " << static_cast<int>(rdram[0x4922C0u]) << std::dec << ")"
                  << std::endl;
        // next match: the main-menu sequence with EXHIBITION forced (an ExshibitionMenuSequence built here or from the
        // PPV submenu misses the main menu's context: a zero-vtable sub-object crashed CObjControl::AllAction);
        // card full: the PPV submenu
        const bool more = g_ppvCard.size() < kPpvMaxMatches;
        if (more)
            g_ppvForceExhibition = true;
        else
        {
            g_ppvMode = PpvMode::None;
            g_ppvMenu = true;
        }
        const uint32_t page = optR32(rdram, 0x500f00u);
        ppvCall(rdram, ctx, runtime, 0x140be0u, {1u}); // ymemChangePage
        const uint32_t next = ppvCall(rdram, ctx, runtime, 0x1001e0u, {0x10u});
        if (next)
            ppvCall(rdram, ctx, runtime, more ? 0x138030u : 0x186e50u, {next}); // TopMenuSequence / CreateTopMenuSequence
        ppvCall(rdram, ctx, runtime, 0x140be0u, {page});
        ppvReturn(ctx, 0u);
    }
    // while playing a card the snapshot (taken after OutPutRuleParam) replaces it; acting after a hooked original
    // is unsafe (it can pause at an EE checkpoint and resume later)
    void ppvOutPutRule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ppvTrampoline(rdram, ctx, runtime))
            return;
        if (g_ppvMode != PpvMode::Play || g_ppvPlayIdx >= g_ppvCard.size())
            return g_origOutPutRule(rdram, ctx, runtime);
        std::memcpy(optP(rdram, kPpvSetup), g_ppvCard[g_ppvPlayIdx].data(), kPpvSetupSize);
        ppvCtlApply(rdram, g_ppvPlayIdx);
        if (g_ppvPlayArena < 31u) // PPV arena (details screen) for every match; 0xFF = keep the snapshot's stage
            rdram[0x4922C0u] = g_ppvPlayArena;
        ppvReturn(ctx, 0u);
    }
    void ppvReturnFromGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_ppvOpeningActive) // the opening event ended (type 3 -> ReturnMenuFromGame): now the card, match 1 next
        {
            g_ppvOpeningActive = false;
            g_ppvOpening = false;
            std::cerr << "[ppv] opening over -> card" << std::endl;
            g_ppvReturnToCard = true;
            g_ppvLevel = PpvLevel::Show;
            g_ppvPage = g_ppvCardRow.empty() ? 0 : g_ppvCardRow[0] / 3;
            return g_origReturnFromGame(rdram, ctx, runtime);
        }
        g_ppvOpening = false; // entrances off: no event manager was built for the opening
        if (g_ppvMode == PpvMode::Play && ppvCardBetween() && !g_ppvCardRow.empty())
        {
            // back through the stock menus; the first TopMenuSequence LoopEnd opens the SHOW card (results + next)
            if (!g_ppvResultTaken && g_ppvPlayIdx < g_ppvCardRow.size() && g_ppvCardRow[g_ppvPlayIdx] < 8)
            {
                // item 19: time-limit draw (scripted tag, 10:00 -> REMATCH / EXIT) never calls GetDoramockResult
                g_ppvResult[g_ppvCardRow[g_ppvPlayIdx]] = "DRAW";
                std::cerr << "[ppv] match " << g_ppvCardRow[g_ppvPlayIdx] + 1 << ": DRAW (no result call)" << std::endl;
            }
            ++g_ppvPlayIdx;
            std::cerr << "[ppv] match over -> SHOW card (" << g_ppvPlayIdx << "/" << g_ppvCard.size() << " played)" << std::endl;
            g_ppvReturnToCard = true;
            g_ppvLevel = PpvLevel::Show;
            if (g_ppvPlayIdx < g_ppvCardRow.size())
                g_ppvPage = g_ppvCardRow[g_ppvPlayIdx] / 3;
            return g_origReturnFromGame(rdram, ctx, runtime);
        }
        if (g_ppvMode == PpvMode::Play && ++g_ppvPlayIdx < g_ppvCard.size())
        {
            std::cerr << "[ppv] next match " << g_ppvPlayIdx + 1 << "/" << g_ppvCard.size() << std::endl;
            return ppvTail(ctx, 0x1a8f80u); // GoGameFromMenu (ppvGoGame -> stock, snapshot restored)
        }
        if (g_ppvMode == PpvMode::Play)
            std::cerr << "[ppv] card finished" << std::endl;
        g_ppvMode = PpvMode::None;
        g_origReturnFromGame(rdram, ctx, runtime);
    }
    // PPV submenu: start playing the loaded card (teardown as ppvNextSequence, then the first match)
    // PPV submenu: start playing the loaded card. Launching in the same frame crashed in a CChar dtor (the menu
    // wrestler still alive / half torn down when the event manager's ChangeLevel clears all objects, with or
    // without the snapshot): end MenuWrestlerCtrl and the menu now, keep the sequence for kPpvLaunchFrames
    // (ppvCreateSeqLoopEnd counts down), then delete it and run GoGameFromMenu as guest code.
    constexpr int kPpvLaunchFrames = 10;
    int g_ppvLaunchDelay = 0;
    // RecordIf::SetMatchData 0x2b66b0(this, MatchResultIn*): the game's record of a finished match. Read from its code:
    // +0xC rule (0xD = Royal Rumble), +0x14 u32 roster id x6, +0x2C u8 per-wrestler result x6, +0x10 / +0xC2 / +0xC3.
    // Winner = wrestlers whose result byte is 1 (NOT verified yet - the raw bytes are logged for checking).
    PS2Runtime::RecompiledFunction g_origRecordSetMatch = nullptr;
    void ppvRecordSetMatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t in = optArg(ctx, 5);
        if (!g_ppvOpeningActive && g_ppvMode == PpvMode::Play && !g_ppvResultTaken && in && g_ppvPlayIdx < g_ppvCardRow.size())
        {
            g_ppvResultTaken = true;
            std::string win;
            std::fprintf(stderr, "[ppv] result: rule 0x%X +0 %u +1 %u +4 %u +8 %u +10 %u +C2 %u +C3 %u |", optR32(rdram, in + 0xCu),
                         rdram[in], rdram[in + 1u], optR32(rdram, in + 4u), optR32(rdram, in + 8u), optR32(rdram, in + 0x10u),
                         rdram[in + 0xC2u], rdram[in + 0xC3u]);
            for (uint32_t j = 0; j < 6u; ++j)
            {
                const uint32_t id = optR32(rdram, in + 0x14u + j * 4u);
                const uint8_t r = rdram[in + 0x2Cu + j];
                std::fprintf(stderr, " %X:%u", id, r);
                if (r == 1u && id < 0x7Bu && win.empty())
                {
                    const uint32_t nm = ppvCall(rdram, ctx, runtime, 0x12cde0u, {id, 0u}); // GetWrestlerName
                    std::string n = nm ? reinterpret_cast<const char *>(optP(rdram, nm)) : "?";
                    const size_t sp = n.find_last_of(' ');
                    win = sp == std::string::npos || sp + 1 >= n.size() ? n : n.substr(sp + 1);
                }
            }
            std::fprintf(stderr, "%c", 10);
            const int row = g_ppvCardRow[g_ppvPlayIdx];
            if (row >= 0 && row < 8)
                g_ppvResult[row] = win.empty() ? "NO WINNER" : "WIN " + win;
            std::cerr << "[ppv] match " << row + 1 << ": " << g_ppvResult[row] << std::endl;
        }
        g_origRecordSetMatch(rdram, ctx, runtime);
    }
    // GetDoramockResult 0x39dc00 (called by GameMainSequence::LoopEndAct 0x2e1198 after the match; SetMatchData above is
    // Season-only via CMatchMakeSystem::SetResultData). Its code: CChar table 0x526ea0[slot]; for slot 0..7 a char with
    // +0x383 != 0 puts its cast code (0x4921CA + slot*0x20) into drGameResult's list (= the winners), bytes 0x492370 /
    // 0x492371 index that table too (decision / finisher side). We read the same data before the original runs.
    PS2Runtime::RecompiledFunction g_origDoramockResult = nullptr;
    void ppvDoramockResult(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_ppvOpeningActive && g_ppvMode == PpvMode::Play && !g_ppvResultTaken && g_ppvPlayIdx < g_ppvCardRow.size())
        {
            g_ppvResultTaken = true;
            std::string win;
            int winners = 0;
            std::fprintf(stderr, "[ppv] result: 370=%d 371=%d |", int(int8_t(rdram[0x492370u])), int(int8_t(rdram[0x492371u])));
            for (uint32_t t = 0; t < 8u; ++t)
            {
                const uint32_t ch = optR32(rdram, 0x526ea0u + t * 4u);
                uint16_t code = 0;
                std::memcpy(&code, rdram + 0x4921CAu + t * 0x20u, 2);
                if (!ch || ch >= 0x2000000u)
                    continue;
                const uint8_t f = rdram[ch + 0x383u];
                std::fprintf(stderr, " slot%u code %u flag %u", t, code, f);
                if (f && code < 25500u)
                {
                    const uint32_t id = hctpRosterCodeId(rdram, code);
                    if (winners++ == 0)
                    {
                        const uint32_t nm = ppvCall(rdram, ctx, runtime, 0x12cde0u, {id, 0u}); // GetWrestlerName
                        std::string n = nm ? reinterpret_cast<const char *>(optP(rdram, nm)) : "?";
                        const size_t sp = n.find_last_of(' ');
                        win = sp == std::string::npos || sp + 1 >= n.size() ? n : n.substr(sp + 1);
                    }
                }
            }
            std::fprintf(stderr, "%c", 10);
            // item 19: 0x492371 = the slot the referee counted (CRef::Count10 0x1f9968 = ref+0x38 target, Miteru pin
            // 0x1f8930 / Count20) = the LOSER (scripted ppv19: Rock pinned by Austin, 371 = Rock's slot 0). Winner =
            // the other team (team byte +9); 3+ teams -> "<NAME> LOST". The +0x383 list above is only a fallback.
            auto nameOf = [&](int slot) {
                uint16_t code = 0;
                std::memcpy(&code, rdram + 0x4921CAu + slot * 0x20u, 2);
                if (code >= 25500u)
                    return std::string();
                const uint32_t id = hctpRosterCodeId(rdram, code);
                const uint32_t nm = ppvCall(rdram, ctx, runtime, 0x12cde0u, {id, 0u});
                std::string n = nm ? reinterpret_cast<const char *>(optP(rdram, nm)) : "?";
                const size_t sp = n.find_last_of(' ');
                return sp == std::string::npos || sp + 1 >= n.size() ? n : n.substr(sp + 1);
            };
            const int ls = int(int8_t(rdram[0x492371u]));
            std::string text;
            if (ls >= 0 && ls < 8 && !nameOf(ls).empty() && rdram[0x4921C4u + ls * 0x20u] == 0u) // a wrestler (+4 role 0)
            {
                const uint8_t lt = rdram[0x4921C9u + ls * 0x20u];
                int teams = 0, otherFirst = -1, others = 0;
                bool seen[8] = {};
                for (int j = 0; j < 8; ++j)
                {
                    if (nameOf(j).empty() || rdram[0x4921C4u + j * 0x20u] != 0u)
                        continue;
                    const uint8_t t = rdram[0x4921C9u + j * 0x20u] & 7u;
                    if (!seen[t])
                        seen[t] = true, ++teams;
                    if (t != lt && otherFirst < 0)
                        otherFirst = j;
                    if (t != lt && otherFirst >= 0 && rdram[0x4921C9u + otherFirst * 0x20u] == t)
                        ++others;
                }
                if (teams == 2 && otherFirst >= 0)
                    text = others > 1 ? "WIN " + nameOf(otherFirst) + " +" + ppvNum(others - 1) : "WIN " + nameOf(otherFirst);
                else
                    text = nameOf(ls) + " LOST";
                std::fprintf(stderr, "[ppv] result from 0x492371: loser slot %d team %d, %d teams%c", ls, int(lt), teams, 10);
            }
            else if (!win.empty())
                text = winners > 1 ? "WIN " + win + " +" + ppvNum(winners - 1) : "WIN " + win;
            const int row = g_ppvCardRow[g_ppvPlayIdx];
            if (row >= 0 && row < 8)
                g_ppvResult[row] = text.empty() ? "NO RESULT" : text;
            std::cerr << "[ppv] match " << row + 1 << ": " << g_ppvResult[row] << std::endl;
        }
        g_origDoramockResult(rdram, ctx, runtime);
    }
    void ppvStartPlay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, uint32_t menu)
    {
        if (g_ppvMode != PpvMode::Play) // a new show (else: next match from the SHOW card)
        {
            g_ppvPlayIdx = 0;
            for (auto &r : g_ppvResult)
                r.clear();
        }
        g_ppvMode = PpvMode::Play;
        g_ppvResultTaken = false;
        std::cerr << "[ppv] show arena " << int(g_ppvPlayArena) << std::endl;
        std::cerr << "[ppv] playing card match " << g_ppvPlayIdx + 1 << "/" << g_ppvCard.size() << std::endl;
        ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, menu, 0u, 8u), {menu, 1u});
        optW32(rdram, self + 0xcu, 0u);
        ppvCall(rdram, ctx, runtime, 0x2440a0u, {}); // MenuWrestlerCtrl::End
        g_ppvLaunchDelay = kPpvLaunchFrames;
        ppvReturn(ctx, 0u);
    }
    // CreateTopMenuSequence::LoopEndAct 0x186fa0 while it shows the PPV submenu (same waits as the original)
    void ppvCreateSeqLoopEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_ppvMenu)
            return g_origCreateSeqLoopEnd(rdram, ctx, runtime);
        const uint32_t self = optArg(ctx, 4);
        const uint32_t menu = optR32(rdram, self + 0xcu);
        if (!menu && g_ppvLaunchDelay > 0 && --g_ppvLaunchDelay == 0) // ppvStartPlay: the menu wrestler is gone now
        {
            g_ppvMenu = false;
            ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, self, 8u, 8u), {self, 1u});
            return ppvTail(ctx, 0x1a8f80u); // GoGameFromMenu
        }
        if (!menu || optR32(rdram, menu + 8u) != 5u)
            return ppvReturn(ctx, 0u);
        const uint32_t sel = optR32(rdram, menu + 0x24u);
        const bool decided = rdram[menu + 0xcu] != 0u;
        if (decided && !(ppvCall(rdram, ctx, runtime, 0x378bc0u, {}) & 0xffu)) // caOutGame::IsAllEnd
            return ppvReturn(ctx, 0u);
        if (!(ppvCall(rdram, ctx, runtime, 0x3aa380u, {}) & 0xffu)) // CrowdSignRead::CanUse
            return ppvReturn(ctx, 0u);
        if (g_ppvLevel == PpvLevel::Show)
        {
            if (decided && sel == 3u && g_ppvPlayIdx < g_ppvCard.size())
            {
                g_ppvLevel = PpvLevel::Sub;
                return ppvStartPlay(rdram, ctx, runtime, self, menu); // next match (index kept)
            }
            std::cerr << "[ppv] show " << (g_ppvPlayIdx < g_ppvCard.size() ? "abandoned" : "finished") << std::endl;
            g_ppvMode = PpvMode::None;
            g_ppvCardRow.clear();
            g_ppvLevel = PpvLevel::Sub;
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
        }
        if (g_ppvLevel == PpvLevel::Load)
        {
            g_ppvDeleteArmed = false;
            if (decided && sel == 1u && !g_ppvSlotList.empty()) // PLAY
            {
                g_ppvCard.clear();
                g_ppvCardRow.clear();
                g_ppvPlayArena = g_ppvWork[46];
                g_ppvPlayBrand = g_ppvWork[47];
                for (int i = 0; i < 8; ++i)
                    if (ppvMatchSet(i))
                    {
                        g_ppvCard.push_back(g_ppvWorkSnap[i]);
                        g_ppvCardRow.push_back(i);
                    }
                g_ppvMode = PpvMode::None; // new show
                g_ppvLevel = PpvLevel::Sub;
                if (!g_ppvCard.empty())
                {
                    std::cerr << "[ppv] YOUR PPVs: play " << ppvCardPath(g_ppvSlot) << std::endl;
                    g_ppvCtl.assign(g_ppvCard.size(), -1);
                    if (ppvPlayAs() && ppvCardBetween())
                    {
                        // item 14: SHOW card before match 1 (pick Play-as / WATCH per match, then NEXT MATCH 1)
                        g_ppvPlayIdx = 0;
                        for (auto &r : g_ppvResult)
                            r.clear();
                        g_ppvMode = PpvMode::Play;
                        g_ppvLevel = PpvLevel::Show;
                        g_ppvPage = g_ppvCardRow.empty() ? 0 : g_ppvCardRow[0] / 3;
                        if (ppvOpeningOn()) // Jordan: opening (feud + pyro + announcers) BEFORE the card
                        {
                            g_ppvOpening = true;
                            std::cerr << "[ppv] opening event before the card" << std::endl;
                            return ppvStartPlay(rdram, ctx, runtime, self, menu);
                        }
                        std::cerr << "[ppv] show card before match 1" << std::endl;
                        return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
                    }
                    return ppvStartPlay(rdram, ctx, runtime, self, menu);
                }
                std::cerr << "[ppv] YOUR PPVs: card has no matches" << std::endl;
                g_ppvLevel = PpvLevel::Load;
            }
            else if (decided && sel == 2u && !g_ppvSlotList.empty() && !g_ppvSample) // EDIT
            {
                std::cerr << "[ppv] YOUR PPVs: edit " << ppvCardPath(g_ppvSlot) << std::endl;
                g_ppvLevel = ppvDetails2() ? PpvLevel::Details : PpvLevel::Card;
            }
            else
                g_ppvLevel = PpvLevel::Sub;
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
        }
        if (g_ppvLevel == PpvLevel::Details)
        {
            // only MATCH CARD (row 3) ends the menu with a decision; Back -> PPV submenu
            g_ppvLevel = decided && sel == 3u ? PpvLevel::Card : PpvLevel::Sub;
            std::cerr << "[ppv] details -> " << (g_ppvLevel == PpvLevel::Card ? "card" : "PPV submenu") << std::endl;
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
        }
        if (ppvCreate2() && g_ppvLevel == PpvLevel::Card)
        {
            if (decided && sel < 3u && ppvAbs(sel) < 8) // edit match (page row sel): stock match type + roster select
            {
                g_ppvEditMatch = ppvAbs(sel);
                std::cerr << "[ppv] card: editing match " << g_ppvEditMatch + 1 << std::endl;
                g_ppvMode = PpvMode::Create;
                g_ppvMenu = false;
                g_ppvLevel = PpvLevel::Sub;
                if (ppvDirectExhi())
                {
                    // what TopMenuSequence's ctor 0x138030 sets up before the main menu can start Exhibition:
                    // save byte (save+0x10000-0x63e1) = 0 and CStableControl::SetStableType(0); MenuMovie and
                    // MenuWrestlerCtrl are already running under the PPV menus
                    const uint32_t save = ppvCall(rdram, ctx, runtime, 0x12cb10u, {}); // GetSaveData
                    if (save)
                        rdram[save + 0x10000u - 0x63e1u] = 0u;
                    ppvCall(rdram, ctx, runtime, 0x172ef0u, {0u});
                    std::cerr << "[ppv] card: Exhibition built directly" << std::endl;
                    return ppvNextSequenceGuest(rdram, ctx, runtime, self, menu, 0x1305a0u, 0x164u); // ExshibitionMenuSequence
                }
                g_ppvForceExhibition = true;
                return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x138030u); // TopMenuSequence -> EXHIBITION
            }
            if (decided && sel == 3u)
                std::cerr << "[ppv] card saved: " << (ppvSaveCard2() ? ppvCardPath(g_ppvSlot) : "FAILED") << std::endl;
            // SAVE -> PPV submenu; Back -> details (HCTP_PPV_DETAILS2) else PPV submenu
            g_ppvLevel = !(decided && sel == 3u) && ppvDetails2() ? PpvLevel::Details : PpvLevel::Sub;
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
        }
        if (!decided || sel >= 3u)
        {
            g_ppvMenu = false;
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x138030u); // TopMenuSequence
        }
        if (sel == 1u && ppvCreate2()) // CREATE A PPV: the card menu (edits the saved card, or a new default one)
        {
            // new card in the first free slot (SvR SearchEmptyData); all 8 used -> YOUR PPVs (edit / delete one)
            g_ppvSample = false;
            const int slot = ppvFreeSlot();
            if (slot < 0)
            {
                std::cerr << "[ppv] create: all " << kPpvSlots << " slots used -> YOUR PPVs" << std::endl;
                g_ppvSlotIdx = 0;
                ppvScanSlots();
                g_ppvLevel = PpvLevel::Load;
                return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
            }
            ppvDefaultSet(g_ppvWork);
            for (auto &r : g_ppvResult)
                r.clear();
            g_ppvSlot = slot;
            std::cerr << "[ppv] create: new card in " << ppvCardPath(slot) << std::endl;
            g_ppvLevel = ppvDetails2() ? PpvLevel::Details : PpvLevel::Card;
            if (ppvDetailsOn())
            {
                // PPV DETAILS = Stable Details re-skin: StableSequence(STABLE_TYPE 0, ATB 0) as CreateTopMenuSequence's
                // LoopEndAct 0x187148 builds it (new 0xC); ppvStableFrameAction lands on CStableDetails
                g_ppvDetails = true;
                g_ppvDetailsBuilt = g_ppvDetailsDone = false;
                g_ppvMenu = false;
                std::cerr << "[ppv] details: StableSequence" << std::endl;
                return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x177000u, 0xCu, {0u, 0u});
            }
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
        }
        if (sel == 2u && ppvCreate2())
        {
            // YOUR PPVs: saved cards (X on the name cycles them), PLAY / EDIT / DELETE
            g_ppvSample = false;
            g_ppvSlotIdx = 0;
            g_ppvDeleteArmed = false;
            ppvScanSlots();
            for (auto &r : g_ppvResult)
                r.clear();
            std::cerr << "[ppv] YOUR PPVs: " << g_ppvSlotList.size() << " saved" << std::endl;
            g_ppvLevel = PpvLevel::Load;
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
        }
        if (sel == 0u && ppvCreate2()) // PLAY SAMPLE PPV: the generated sample cards, same list screen (PLAY only)
        {
            ppvMakeSamples();
            g_ppvSample = true;
            g_ppvSlotIdx = 0;
            g_ppvDeleteArmed = false;
            ppvScanSlots();
            std::cerr << "[ppv] samples: " << g_ppvSlotList.size() << std::endl;
            if (!g_ppvSlotList.empty())
            {
                g_ppvLevel = PpvLevel::Load;
                return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u);
            }
            g_ppvSample = false; // no template yet: the old slice-2 sample below
        }
        if (sel == 1u) // CREATE A PPV: new card, one Exhibition run per match
        {
            std::cerr << "[ppv] creating a card" << std::endl;
            g_ppvCard.clear();
            g_ppvMode = PpvMode::Create;
            g_ppvMenu = false;
            g_ppvForceExhibition = true; // the main menu picks EXHIBITION by itself (see ppvTopSeqLoopEnd)
            return ppvNextSequence(rdram, ctx, runtime, self, menu, 0x138030u); // TopMenuSequence
        }
        if (ppvLoadCard(sel == 0u ? "ppv_sample.bin" : "ppv_card.bin"))
            return ppvStartPlay(rdram, ctx, runtime, self, menu);
        std::cerr << "[ppv] no " << (sel == 0u ? "sample" : "saved") << " card" << std::endl;
        ppvNextSequence(rdram, ctx, runtime, self, menu, 0x186e50u); // CreateTopMenuSequence again (PPV submenu)
    }
    // ExshibitionMenuSequence::WrestlerSelectEnd 0x1321a0 (fresh entry, our own decided path; cancel and Royal Rumble
    // 0xD / Hell 0x47 stay stock). Stock decided path: rule+0x1E == 3 or rule 0x3C -> 0x18 matchup, else 0x17 arena.
    PS2Runtime::RecompiledFunction g_origWrestlerSelEnd = nullptr;
    void ppvWrestlerSelEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4);
        const uint32_t rule = optR32(rdram, self + 0x140u);
        if (!ppvCreate2() || g_ppvMode != PpvMode::Create || g_ppvEditMatch < 0 || rule == 0xDu || rule == 0x47u)
            return g_origWrestlerSelEnd(rdram, ctx, runtime);
        const uint32_t frame = optR32(rdram, self + 0xcu);
        if (!frame || !(ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, frame, 0u, 0x34u), {frame}) & 0xffu))
        {
            ppvCall(rdram, ctx, runtime, 0x346110u, {}); // MatchWrestlerCtrl::End, as the stock cancel
            ppvExitToCard(rdram, self);
            return ppvReturn(ctx, 0x1Bu);
        }
        const uint32_t ruleRec = 0x45AF60u + rule * 0x20u; // rule table: +0x1F default stage
        const uint32_t stage = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(rdram[ruleRec + 0x1fu])));
        optW32(rdram, self + 0x148u, stage);
        optW32(rdram, self + 0x14cu, stage);
        ppvCall(rdram, ctx, runtime, 0x181df0u, {});     // COptionBackUp::OptionBackUp, as the stock decided path
        ppvCall(rdram, ctx, runtime, 0x130d10u, {self}); // SetCommon: selections -> setup block 0x4920C0..
        const int i = g_ppvEditMatch;
        uint8_t *m = g_ppvWork + 52 + i * kPpvMatchSize;
        ppvMatchInit(m);
        m[0] = static_cast<uint8_t>(rule);
        for (uint32_t j = 0; j < 6u; ++j) // selection records self+0x18+j*0x30 (SetCommon: +4 used, +8 team, +0x1C id, +0x20 attire)
        {
            const uint32_t rec = self + 0x18u + j * 0x30u;
            if (!optR32(rdram, rec + 4u))
                continue;
            uint8_t *r = m + 8 + j * 4;
            r[0] = static_cast<uint8_t>(optR32(rdram, rec + 0x1cu));
            r[1] = rdram[0x4921C0u + j * 0x20u];
            r[2] = static_cast<uint8_t>(optR32(rdram, rec + 0x20u));
            r[3] = static_cast<uint8_t>(optR32(rdram, rec + 8u));
        }
        g_ppvWorkSnap[i].assign(optP(rdram, kPpvSetup), optP(rdram, kPpvSetup) + kPpvSetupSize);
        std::cerr << "[ppv] card match " << i + 1 << " stored: rule 0x" << std::hex << rule << " ids";
        for (int j = 0; j < 6; ++j)
            std::cerr << " " << static_cast<int>(m[8 + j * 4]);
        std::cerr << std::dec << " (no arena / matchup in Create)" << std::endl;
        ppvExitToCard(rdram, self);
        ppvReturn(ctx, 0x1Bu);
    }
    // ExshibitionMenuSequence::Next 0x131530 (fresh entry): Create skips PlayerEntry 0x14 / PositionSelect 0x15 - one
    // player (self+0x13C = 1, GetCommon+0x206 as PlayerEntryEnd), InitPositionInfo 0x1307b0 fills the slot records
    // (+4 used, +8 position, control from the rule table, default wrestlers), then WrestlerSelect 0x16.
    PS2Runtime::RecompiledFunction g_origExhiNext = nullptr, g_origExhiTopEnd = nullptr;
    void ppvExhiNext(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4), type = optArg(ctx, 5);
        if (ppvCreate2() && g_ppvMode == PpvMode::Create && g_ppvEditMatch >= 0 && (type == 0x14u || type == 0x15u) &&
            optR32(rdram, self + 0x140u) != 0xDu)
        {
            optW32(rdram, self + 0x13cu, 1u);
            const uint32_t common = ppvCall(rdram, ctx, runtime, 0x12cb20u, {}); // GetCommon
            if (common)
                rdram[common + 0x206u] = 1u;
            ppvCall(rdram, ctx, runtime, 0x1307b0u, {self});
            SET_GPR_U32(ctx, 5, 0x16u);
            std::cerr << "[ppv] card: controller screen skipped -> wrestler select" << std::endl;
        }
        g_origExhiNext(rdram, ctx, runtime);
    }
    // TopEnd 0x131670: Back on Exhibition's top menu while making a card -> card menu (not the main menu)
    void ppvExhiTopEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = optArg(ctx, 4), ra = optArg(ctx, 31);
        g_origExhiTopEnd(rdram, ctx, runtime);
        if (ppvOrigDone(ctx, ra) && ppvCreate2() && g_ppvMode == PpvMode::Create && g_ppvEditMatch >= 0 &&
            optArg(ctx, 2) == 0x1Bu && optR32(rdram, self + 0x14u) == 6u)
            ppvExitToCard(rdram, self);
    }
    // ---- PPV DETAILS slice 1 (Nov 4; HCTP_PPV_DETAILS=0 off). Stable Details re-skin (Jordan): name row = PPV name,
    // members = commentary team, entrance -> arena (relabel/picks = later slices). Read (side-session note
    // ppv_details_stable_handoff_20261104.md): CStableFrame (singleton *0x526338) works on a new[0x402] COPY of the
    // global stable table (+0x20, 27 x 0x26); only CStableMain::FromUI_OK/Cancel (0x178650/0x1789d0) write it back,
    // so CStableMain is deleted at once and never decides. Action 0x177610 builds CStableMain (+0x10) when the reader
    // is done; we then build CStableDetails like its case 0x177700 (new 0xD0, ctor 0x1743e0(this, frame+0x28, 6, 0),
    // +0x3c = record index, +0x14) on the first empty record (+0x16 == 0 -> ctor runs InitStableData on the copy).
    // Details ended (SimpleSelectFrame state +8 == 5, CStableMain gone): record -> g_ppvWork, frame+0x39 = 1 ->
    // StableSequence::LoopEndAct 0x177100 builds CreateTopMenuSequence (stock) -> our card menu (g_ppvMenu).
    PS2Runtime::RecompiledFunction g_origStableFrameAction = nullptr;
    void ppvStableFrameAction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t frame = optArg(ctx, 4), ra = optArg(ctx, 31);
        if (g_ppvDetails && g_ppvDetailsBuilt && !g_ppvDetailsDone && frame && !optR32(rdram, frame + 0x10u))
        {
            const uint32_t det = optR32(rdram, frame + 0x14u);
            // Action's own "details finished" test (0x1777dc): CStableMain gone, details alive, frame+0x1c == 0
            if (det && optR32(rdram, frame + 0x1cu) == 0u)
            {
                g_ppvDetailsDone = true;
                const uint32_t idx = optR32(rdram, frame + 0x3cu), tbl = optR32(rdram, frame + 0x20u);
                if (tbl && idx < 27u)
                {
                    const uint8_t *rec = optP(rdram, tbl + idx * 0x26u);
                    char hex[0x26 * 3 + 1] = {};
                    for (int i = 0; i < 0x26; ++i)
                        std::snprintf(hex + i * 3, 4, "%02x ", rec[i]);
                    std::cerr << "[ppv] details done, record " << idx << ": " << hex << std::endl;
                    if (rec[0])
                    {
                        std::memset(g_ppvWork, 0, 0x16);
                        std::memcpy(g_ppvWork, rec, 0x15); // name (EndAction copies 0x16 bytes from the name entry)
                    }
                }
                rdram[frame + 0x39u] = 1u; // StableSequence::LoopEndAct -> CreateTopMenuSequence
                g_ppvDetails = false;
                g_ppvMenu = true;
                g_ppvLevel = PpvLevel::Card;
                return ppvReturn(ctx, 0u);
            }
        }
        g_origStableFrameAction(rdram, ctx, runtime);
        if (g_ppvDetails && ppvOrigDone(ctx, ra) && rdram[frame + 0x39u]) // left the stable screens: PPV submenu
        {
            std::cerr << "[ppv] details: left the stable screens" << std::endl;
            g_ppvDetails = false;
            g_ppvMenu = true;
            g_ppvLevel = PpvLevel::Sub;
            return;
        }
        if (!g_ppvDetails || g_ppvDetailsBuilt || !ppvOrigDone(ctx, ra))
            return;
        const uint32_t main = optR32(rdram, frame + 0x10u);
        if (!main)
            return;
        ppvCall(rdram, ctx, runtime, ppvVirtual(rdram, main, 0u, 8u), {main, 1u}); // delete CStableMain
        optW32(rdram, frame + 0x10u, 0u);
        ppvCall(rdram, ctx, runtime, 0x177870u, {frame}); // MakeMenuItem
        uint32_t idx = 26u;
        const uint32_t tbl = optR32(rdram, frame + 0x20u);
        for (uint32_t i = 0; tbl && i < 27u; ++i)
            if (rdram[tbl + i * 0x26u + 0x16u] == 0u)
            {
                idx = i;
                break;
            }
        optW32(rdram, frame + 0x3cu, idx);
        const uint32_t det = ppvCall(rdram, ctx, runtime, 0x1001e0u, {0xD0u});
        if (det)
        {
            std::memset(optP(rdram, det), 0, 0xD0);
            ppvCall(rdram, ctx, runtime, 0x1743e0u, {det, optR32(rdram, frame + 0x28u), 6u, 0u});
        }
        optW32(rdram, frame + 0x14u, det);
        optW32(rdram, frame + 0x1cu, 1u); // details active (0 = Action deletes it and rebuilds CStableMain)
        g_ppvDetailsBuilt = true;
        std::cerr << "[ppv] details: CStableDetails on record " << idx << (det ? "" : " (new FAILED)") << std::endl;
    }
    void installPpvMenu(PS2Runtime &runtime)
    {
        struct Hook { uint32_t addr; PS2Runtime::RecompiledFunction *orig; PS2Runtime::RecompiledFunction fn; };
        const Hook hooks[] = {{0x12fd60u, &g_origGetRCString, ppvGetRCString},
                              {0x138950u, &g_origTopMenuCtor, ppvTopMenuCtor},
                              {0x138c70u, &g_origTopMenuIntro, ppvTopMenuIntro},
                              {0x138f40u, &g_origTopMenuDecision, ppvTopMenuDecision},
                              {0x139400u, &g_origTopMenuAnimEnd, ppvTopMenuAnimEnd},
                              {0x138a30u, &g_origTopMenuDtor, ppvTopMenuDtor},
                              {0x1381d0u, &g_origTopSeqLoopEnd, ppvTopSeqLoopEnd},
                              {0x186fa0u, &g_origCreateSeqLoopEnd, ppvCreateSeqLoopEnd},
                              {0x138900u, &g_origCursorTarget, ppvCursorTarget},
                              {0x1a8f80u, &g_origGoGame, ppvGoGame},
                              {0x181e70u, &g_origOutPutRule, ppvOutPutRule},
                              {0x1a9020u, &g_origReturnFromGame, ppvReturnFromGame},
                              {0x1321a0u, &g_origWrestlerSelEnd, ppvWrestlerSelEnd},
                              {0x131530u, &g_origExhiNext, ppvExhiNext},
                              {0x131670u, &g_origExhiTopEnd, ppvExhiTopEnd},
                              {0x177610u, &g_origStableFrameAction, ppvStableFrameAction},
                              {0x135b60u, &g_origCreateMenuDecision, ppvCreateMenuDecision},
                              {0x12e7c0u, &g_origMenuSub1, ppvMenuSub1},
                              {0x12e7d0u, &g_origMenuSub2, ppvMenuSub2},
                              {0x2b66b0u, &g_origRecordSetMatch, ppvRecordSetMatch},
                              {0x39dc00u, &g_origDoramockResult, ppvDoramockResult},
                              {0x2a7a20u, &g_origEvtMngCtor, ppvEvtMngCtor},
                              {0x165680u, &g_origEvtSysLoaderCtor, ppvEvtSysLoaderCtor},
                              {0x165ba0u, &g_origEvtMotLoaderCtor, ppvEvtMotLoaderCtor},
                              {0x166a60u, &g_origEvtSysCtor, ppvEvtSysCtor},
                              {0x166e00u, &g_origEvtSysAction, ppvEvtSysAction}};
        for (const Hook &h : hooks)
            if (!(*h.orig = runtime.lookupFunction(h.addr)))
                return;
        for (const Hook &h : hooks)
            runtime.replaceFunction(h.addr, h.fn);

        std::cerr << "[ppv] main menu PPV MODE entry installed" << std::endl;

    }
    uint32_t sfdInfo(uint32_t ch) { return kSfdInfo + ch * kSfdStride; }
    void makePath3Tag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ch)
    {
        const uint32_t info = sfdInfo(ch);
        if (optR32(rdram, info + 0xd0u) == 0u) // as PlayByAFS: the path3 / user packets for this movie size
            guestCall(rdram, ctx, runtime, 0x158b60u, {info, optR32(rdram, info + 8u), optR32(rdram, info + 0xcu)});
    }
    void movieStarted(uint8_t *rdram, uint32_t ch)
    {
        g_movieNative[ch] = true;
        g_movieHasStaleDrawTick[ch] = false;
        g_movieLastStatusPoll[ch] = {};
        g_movieEndReported[ch] = false;
        optW32(rdram, sfdInfo(ch) + 0xb4u, 2u);
        *optP(rdram, sfdInfo(ch) + 0x1e8u) = 0u;
    }
    bool nativePlayAfs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ch, uint32_t part, uint32_t fid, uint32_t loop)
    {
        static const char *files[] = {"MOVIE/NMV000.AFS", "MOVIE/NMV001.AFS", "MOVIE/NMV002.AFS", "MOVIE/NMV003.AFS", "MOVIE/LOGOS.AFS"};
        if (ch >= 4u || part < 5u || part > 9u)
            return false;
        const std::string *imported = hostMediaPath('V', part, fid);
        if (g_movieNativeImportedOnly && !imported)
            return false; // stock movies retain their working Sofdec/interlace path
        hctpMovieStop(static_cast<int>(ch));
        makePath3Tag(rdram, ctx, runtime, ch);
        if (!hctpMovieOpen(static_cast<int>(ch), imported ? imported->c_str() : files[part - 5u],
                           imported ? -1 : static_cast<int>(fid), loop != 0u))
            return false;
        movieStarted(rdram, ch);
        std::fprintf(stderr, "[nmovie] PlayByAFS ch %u part %u fid %u loop %u%c", ch, part, fid, loop, 10);
        return true;
    }
    void nativeStop(uint8_t *rdram, uint32_t ch)
    {
        hctpMovieStop(static_cast<int>(ch));
        g_movieNative[ch] = false;
        *optP(rdram, sfdInfo(ch) + 0x1e8u) = 0u;
        optW32(rdram, sfdInfo(ch) + 0xb4u, 0u);
    }
    void mvPlayAfs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // CADX_CTRL::PlayByAFS 0x159980
    {
        const uint32_t ch = optArg(ctx, 4);
        if (nativePlayAfs(rdram, ctx, runtime, ch, optArg(ctx, 5), optArg(ctx, 6), optArg(ctx, 7)))
        {
            returnValue(ctx, 1);
            return;
        }
        if (g_movieNative[ch & 3u]) nativeStop(rdram,ch & 3u);
        g_movieNative[ch & 3u] = false;
        g_mvOrigPlayAfs(rdram, ctx, runtime);
    }
    // CMoviePlayer (this: +0 = the SFD channel) inlines its own copies of these: PlayByAfs 0x1b0c20, Stop 0x1b0c30,
    // Pause 0x1b0f60, Continue 0x1b0f70
    PS2Runtime::RecompiledFunction g_cmpOrigPlayAfs, g_cmpOrigStop, g_cmpOrigPause, g_cmpOrigContinue;
    void cmpPlayAfs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ch = optR32(rdram, optArg(ctx, 4));
        if (nativePlayAfs(rdram, ctx, runtime, ch, optArg(ctx, 5), optArg(ctx, 6), optArg(ctx, 7)))
        {
            returnValue(ctx, 1);
            return;
        }
        if (g_movieNative[ch & 3u]) nativeStop(rdram,ch & 3u);
        g_movieNative[ch & 3u] = false;
        g_cmpOrigPlayAfs(rdram, ctx, runtime);
    }
    void cmpStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ch = optR32(rdram, optArg(ctx, 4)) & 3u;
        if (g_movieNative[ch])
            nativeStop(rdram, ch);
        else
            g_cmpOrigStop(rdram, ctx, runtime);
    }
    void cmpPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ch = optR32(rdram, optArg(ctx, 4)) & 3u;
        if (g_movieNative[ch])
            hctpMoviePause(static_cast<int>(ch), true);
        else
            g_cmpOrigPause(rdram, ctx, runtime);
    }
    void cmpContinue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ch = optR32(rdram, optArg(ctx, 4)) & 3u;
        if (g_movieNative[ch])
            hctpMoviePause(static_cast<int>(ch), false);
        else
            g_cmpOrigContinue(rdram, ctx, runtime);
    }
    void mvPlayName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // CADX_CTRL::PlayByName 0x1598b0
    {
        const uint32_t ch = optArg(ctx, 4), name = optArg(ctx, 5), loop = optArg(ctx, 6);
        if (g_movieNativeImportedOnly)
        {
            if (g_movieNative[ch & 3u]) nativeStop(rdram,ch & 3u);
            g_mvOrigPlayName(rdram,ctx,runtime);
            return;
        }
        std::string path(reinterpret_cast<const char *>(optP(rdram, name)));
        if (const size_t colon = path.find(':'); colon != std::string::npos)
            path = path.substr(colon + 1);
        if (const size_t semi = path.find(';'); semi != std::string::npos)
            path.resize(semi);
        while (!path.empty() && (path[0] == '\\' || path[0] == '/'))
            path.erase(0, 1);
        if (ch < 4u)
        {
            hctpMovieStop(static_cast<int>(ch));
            makePath3Tag(rdram, ctx, runtime, ch);
            if (hctpMovieOpen(static_cast<int>(ch), path.c_str(), -1, loop != 0u))
            {
                movieStarted(rdram, ch);
                std::fprintf(stderr, "[nmovie] PlayByName ch %u %s loop %u%c", ch, path.c_str(), loop, 10);
                returnValue(ctx, 1);
                return;
            }
        }
        g_movieNative[ch & 3u] = false;
        g_mvOrigPlayName(rdram, ctx, runtime);
    }
    void mvStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // CADX_CTRL::Stop 0x159430
    {
        const uint32_t ch = optArg(ctx, 4) & 3u;
        if (g_movieNative[ch])
            nativeStop(rdram, ch);
        else
            g_mvOrigStop(rdram, ctx, runtime);
    }
    void mvPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // CADX_CTRL::Pause 0x159390
    {
        const uint32_t ch = optArg(ctx, 4) & 3u;
        if (g_movieNative[ch])
            hctpMoviePause(static_cast<int>(ch), true);
        else
            g_mvOrigPause(rdram, ctx, runtime);
    }
    void mvContinue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // CADX_CTRL::Continue 0x1593e0
    {
        const uint32_t ch = optArg(ctx, 4) & 3u;
        if (g_movieNative[ch])
            hctpMoviePause(static_cast<int>(ch), false);
        else
            g_mvOrigContinue(rdram, ctx, runtime);
    }
    void mvGetStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // CADX_CTRL::GetStat 0x159fc0
    {
        const uint32_t ch = optArg(ctx, 4) & 3u;
        if (g_movieNative[ch])
        {
            const int stat = hctpMovieStatus(static_cast<int>(ch));
            optW32(rdram, sfdInfo(ch) + 0xb4u, static_cast<uint32_t>(stat));
            returnValue(ctx, stat);
            return;
        }
        g_mvOrigGetStat(rdram, ctx, runtime);
    }
    void mvLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // CADX_CTRL::LoadImage 0x159ff0
    {
        const uint32_t ch = optArg(ctx, 4) & 3u, color = optArg(ctx, 5);
        if (!g_movieNative[ch])
        {
            g_mvOrigLoadImage(rdram, ctx, runtime);
            return;
        }
        const uint32_t info = sfdInfo(ch);
        // The title's tight LoadImage loop stops polling GetStat after playback starts.
        // Publish the host player's end state here so the guest can leave the final logo.
        const auto now = std::chrono::steady_clock::now();
        auto &lastPoll = g_movieLastStatusPoll[ch];
        if (lastPoll.time_since_epoch().count() == 0 || now - lastPoll >= std::chrono::milliseconds(16))
        {
            lastPoll = now;
            const int stat = hctpMovieStatus(static_cast<int>(ch));
            optW32(rdram, info + 0xb4u, static_cast<uint32_t>(stat));
            if (stat == 3 && !g_movieEndReported[ch])
            {
                g_movieEndReported[ch] = true;
                std::fprintf(stderr, "[nmovie] ch %u end published from LoadImage%c", ch, 10);
            }
        }
        const uint32_t w = optR32(rdram, info + 8u), h = optR32(rdram, info + 0xcu), buf = optR32(rdram, info + 0xccu);
        const uint32_t mode = optR32(rdram, info + 0xb8u), gsWord = optR32(rdram, info + 0x84u);
        const uint32_t pkt = optR32(rdram, info + 0xbcu), usr = optR32(rdram, info + 0xc4u);
        const bool fresh = w && h && buf && hctpMovieFrame(static_cast<int>(ch), optP(rdram, buf), static_cast<int>(w), static_cast<int>(h));
        if (!fresh)
        {
            if (mode == 1u && *optP(rdram, info + 0x1e8u) && nativeMovieDrawDue(runtime, ch))
                guestCall(rdram, ctx, runtime, 0x3666c0u, {usr}); // UsrDrawMv: keep showing the last frame
            return;
        }
        if (static int logged[4] = {}; logged[ch]++ < 2)
            std::fprintf(stderr, "[nmovie] LoadImage ch %u w %u h %u mode %u gs %x pkt %x usr %x cb %x buf %x info:%08x %08x %08x %08x%c", ch, w, h,
                         mode, gsWord, pkt, usr, optR32(rdram, info + 0xd0u), buf, optR32(rdram, info + 0x10u),
                         optR32(rdram, info + 0x14u), optR32(rdram, info + 0x80u), optR32(rdram, info + 0x88u), 10);
        optW32(rdram, info + 0x3cu, w);
        optW32(rdram, info + 0x40u, h);
        if (const uint32_t cb = optR32(rdram, info + 0xd0u))
            guestCall(rdram, ctx, runtime, cb, {optR32(rdram, info + 0xd4u), buf, w, h});
        else if (mode == 1u)
        {
            guestCall(rdram, ctx, runtime, 0x3665f0u, {pkt, usr, buf, w, h, color, gsWord}); // UsrLoadMvFrm
            guestCall(rdram, ctx, runtime, 0x3666c0u, {usr});                                 // UsrDrawMv
            if (runtime->chain().async())
                runtime->chain().fence(ChainFence::GifStat);
            *optP(rdram, info + 0x1e8u) = 1u;
        }
        else if (gsWord && pkt)
        {
            *optP(rdram, info + 0x1e8u) = 1u;
            guestCall(rdram, ctx, runtime, 0x159d70u, {buf, w, h, pkt, gsWord}); // LoadImagePath3
            const uint32_t dmaCh = guestCall(rdram, ctx, runtime, 0x102df0u, {2u}); // sceDmaGetChan(GIF)
            guestCall(rdram, ctx, runtime, 0x101770u, {0u, 0u});                  // sceGsSyncPath
            guestCall(rdram, ctx, runtime, 0x103120u, {dmaCh, pkt});              // sceDmaSend
            if (runtime->chain().async())
                runtime->chain().fence(ChainFence::GifStat);
        }
    }
    void installNativeMovies(PS2Runtime &runtime, bool importedOnly = false)
    {
        g_movieNativeImportedOnly = importedOnly;
        bool ok = true;
        auto hook = [&](uint32_t addr, PS2Runtime::RecompiledFunction &orig, PS2Runtime::RecompiledFunction fn) {
            orig = runtime.lookupFunction(addr);
            ok = ok && orig && runtime.replaceFunction(addr, fn);
        };
        hook(0x159980u, g_mvOrigPlayAfs, mvPlayAfs);
        hook(0x1598b0u, g_mvOrigPlayName, mvPlayName);
        hook(0x159430u, g_mvOrigStop, mvStop);
        hook(0x159390u, g_mvOrigPause, mvPause);
        hook(0x1593e0u, g_mvOrigContinue, mvContinue);
        hook(0x159fc0u, g_mvOrigGetStat, mvGetStat);
        hook(0x159ff0u, g_mvOrigLoadImage, mvLoadImage);
        hook(0x1b0c20u, g_cmpOrigPlayAfs, cmpPlayAfs);
        hook(0x1b0c30u, g_cmpOrigStop, cmpStop);
        hook(0x1b0f60u, g_cmpOrigPause, cmpPause);
        hook(0x1b0f70u, g_cmpOrigContinue, cmpContinue);
        std::cerr << "[nmovie] native movies (FFmpeg) " << (ok ? "on" : "FAILED")
                  << (importedOnly ? " (imported TitanTrons only; stock movies use Sofdec)" : " (all movies)") << std::endl;
    }


    void installOptionRestore(PS2Runtime &runtime)
    {
        if (const char *v = std::getenv("HCTP_OPT_RESTORE"); v && v[0] == '0')
            return;
        bool ok = true;
        auto hook = [&](uint32_t addr, PS2Runtime::RecompiledFunction &orig, PS2Runtime::RecompiledFunction fn) {
            orig = runtime.lookupFunction(addr);
            ok = ok && orig && runtime.replaceFunction(addr, fn);
        };
        hook(0x181a90u, g_optOrigItemNum, optItemNum);
        hook(0x181a30u, g_optOrigItem, optItem);
        hook(0x1837b0u, g_optOrigInit, optInit);
        hook(0x185a60u, g_optOrigStrList, optStrList);
        hook(0x185150u, g_optOrigIndexNo, optIndexNo);
        hook(0x184c00u, g_optOrigDecisionNext, optDecisionNext);
        hook(0x184eb0u, g_optOrigWindowEnd, optWindowEnd);
        hook(0x1852e0u, g_optOrigSetDef, optSetDef);
        std::cerr << "[hctp-opt] in-game music track + commentary volume options " << (ok ? "on" : "FAILED") << std::endl;
    }

    // ===================== ROSTER EXPANSION (HCTP_ROSTER_EXPAND=1, default off; Nov 5) =====================
    // Real new roster ids (no CAW carriers). Plan: F:\hctp-project\notes\ROSTER_EXPANSION_PLAN_20261105.md.
    // Ids 0x56..0xA1 are CAW ids (GetCharaGameProp 0x12cec0: id >= 0x56 -> CAW block 0x4B6460+(id-0x56)*0x460), so new
    // slots take ids 0xA2.. (u8-safe). Every one of the 49 functions with "slti id, 0x56" would send them to the CAW path:
    //  - record accessors (GetCharaGameProp, name, ring name, IsSmackDownEntry, IsRosterExist, IsSeasonEntry) answer
    //    from a host-built 0x170 record (template's org record + MODS\ROSTER\<folder>\slot.txt / record.bin) in guest heap;
    //  - per-id stock tables (mike/belt/corset pos, posture, portrait, costume, B&P/first blood) get the TEMPLATE id;
    //  - GetMatchWrestlerNo returns model no (86..99, the CAW-id range no stock model uses) *100 + attire*10: the
    //    model pac is copied behind MODS\PAC\EMD2\<code>. Paths that turn a cast code back into an id (code/100) land on
    //    a CAW id -> GetCharaGameProp maps (pool model no, known ra) to the slot ([rosterx] logs unknown ra sites).
    // Step 1 = static model numbers (<= 14 slots); step 3 = per-match model-number pool (unlimited slots).
    struct RosterxSlot
    {
        uint16_t id = 0;
        uint8_t tmpl = 0x2D, modelNo = 0, attires = 0; // modelNo 0 = unbound (Nov 10 step 3 pool)
        uint64_t lastUse = 0; // pool LRU stamp
        int music = -1, video = -1; // slot.txt music=/video= (MEDIA ids): written into the record; template MEDIA_DEFAULTS row skipped
        uint8_t portrait = 0; // slot.txt portrait=<stock id>: select-screen face (0 = template's); 31 = blank NPC placeholder
        int16_t entrance = 0; // 0 = template's entrance (slot.txt entrance=3071 = O'Haire walk-down)
        std::string name, ring, folder;
        std::vector<std::string> pacs; // attire models (model1.pac, model2.pac ... or the first *.pac)
        std::vector<uint8_t> record;   // optional record.bin (0x170 stock layout)
        uint32_t rec = 0;              // guest record
    };
    std::vector<RosterxSlot> g_rosterx;
    constexpr uint16_t kRosterxFirstId = 0xA2;
    constexpr uint8_t kRosterxModelFirst = 86, kRosterxModelLast = 99;
    // ra sites that call GetCharaGameProp(cast code / 100): GetEntranceEventNoFromSystem 0x2ad230
    // A CAW in slot 0 also gives (10000-10000)/100 + 0x56 = 0x56 at these sites: map only when a live cast code is the
    // slot's model no (8600..8699), never when that code is a CAW one.
    bool rosterxCastHasModel(const uint8_t *rdram, uint32_t modelNo)
    {
        for (uint32_t i = 0; i < 8; ++i)
        {
            uint16_t code;
            std::memcpy(&code, rdram + 0x4921CAu + i * 0x20u, 2);
            if (code < 10000u && code / 100u == modelNo)
                return true;
        }
        return false;
    }
    RosterxSlot *rosterxByModel(uint32_t no);
    // Code paths turn cast code 8600 into id 86 = CAW id 0x56 (~36 functions do code/100, see plan). Treat that id as the
    // slot only while a live cast code uses the slot's model no, no CAW code of that index is cast, and that CAW block is
    // empty (a real CAW is never shadowed). Nov 5: the ra-site list was wrong (ResetRosterUseType / DrawAbility /
    // GetTotalAbility loop over real CAW ids).
    RosterxSlot *rosterxFromCodeId(const uint8_t *rdram, uint32_t id)
    {
        if (id < kRosterxModelFirst || id > kRosterxModelLast)
            return nullptr;
        RosterxSlot *m = rosterxByModel(id);
        if (!m || !rosterxCastHasModel(rdram, id) || rdram[0x4B6450u + (id - 0x56u) * 0x460u + 0x126u] != 0)
            return nullptr;
        for (uint32_t i = 0; i < 8; ++i)
        {
            uint16_t code;
            std::memcpy(&code, rdram + 0x4921CAu + i * 0x20u, 2);
            if (code != 0x639Cu && code >= 10000u && (code - 10000u) / 100u == id - 0x56u)
                return nullptr;
        }
        return m;
    }
    // Template's cast code for a slot's code (same attire/part digits): for per-model tables keyed on cast codes.
    uint32_t rosterxTemplateCode(const uint8_t *rdram, const RosterxSlot &s, uint32_t code)
    {
        int16_t model;
        std::memcpy(&model, rdram + 0x4C0F54u + s.tmpl * 0x170u + 0x114u, 2);
        return static_cast<uint32_t>(model) * 100u + code % 100u;
    }
    uint32_t rosterxCount() { return static_cast<uint32_t>(g_rosterx.size()); }
    bool rosterxVictoryKey(uint32_t modelNo, uint32_t &slotId, int16_t &sel)
    {
        for (const auto &s : g_rosterx)
            if (s.modelNo == modelNo)
            {
                slotId = s.id;
                sel = 0;
                if (s.record.size() == 0x170u)
                    std::memcpy(&sel, s.record.data() + 0xEA, 2);
                return true;
            }
        return false;
    }
    RosterxSlot *rosterxById(uint32_t id)
    {
        if (id < kRosterxFirstId)
            return nullptr;
        for (auto &s : g_rosterx)
            if (s.id == id)
                return &s;
        return nullptr;
    }
    RosterxSlot *rosterxByModel(uint32_t no)
    {
        if (no == 0u)
            return nullptr;
        for (auto &s : g_rosterx)
            if (s.modelNo == no)
                return &s;
        return nullptr;
    }
    int rosterxEntranceForModel(uint32_t model)
    {
        const RosterxSlot *s = (model >= kRosterxModelFirst && model <= kRosterxModelLast) ? rosterxByModel(model) : nullptr;
        return s ? s->entrance : 0;
    }
    uint32_t rosterxRecord(uint8_t *rdram, PS2Runtime *runtime, RosterxSlot &s)
    {
        if (s.rec != 0u)
            return s.rec;
        s.rec = runtime->guestMalloc(0x170u);
        if (s.rec == 0u)
            return 0u;
        uint8_t *const r = rdram + s.rec;
        std::memcpy(r, rdram + 0x4C0F54u + s.tmpl * 0x170u, 0x170u);
        if (s.record.size() == 0x170u)
            std::memcpy(r, s.record.data(), 0x170u);
        if (s.music >= 0)
            r[0x15F] = static_cast<uint8_t>(s.music);
        if (s.video >= 0)
            r[0x15E] = static_cast<uint8_t>(s.video);
        if (s.modelNo != 0u) // unbound pool slot keeps the template's model no until rosterxBind
        {
            const int16_t model = s.modelNo;
            std::memcpy(r + 0x114, &model, 2);
        }
        if (s.entrance > 0)
            std::memcpy(r + 0xEC, &s.entrance, 2);
        std::memset(r + 0x116, 0, 0x16);
        std::strncpy(reinterpret_cast<char *>(r + 0x116), s.name.c_str(), 0x15);
        std::memset(r + 0x12C, 0, 0x10);
        std::strncpy(reinterpret_cast<char *>(r + 0x12C), (s.ring.empty() ? s.name : s.ring).c_str(), 0xF);
        if (r[0x163] == 6) // template hidden -> visible male
            r[0x163] = 0;
        std::fprintf(stderr, "[rosterx] %s: id 0x%X record %08x (template 0x%X, model %u, entrance %d)%c", s.name.c_str(),
                     s.id, s.rec, s.tmpl, s.modelNo, s.entrance, 10);
        return s.rec;
    }
    // Copies models behind "/EMD2/<model*100 + suffix>" for cfileSearchLoose. Suffix = attire*10 + part (part 0 ring,
    // 3 entrance flag, 4 entrance dress); entrance-only attires sit at higher attire digits (stock Piper 6930 = his
    // entrance model, asked for at the entrance; a missing key froze it Nov 5). A slot file "<2 digits>.pac" (e.g.
    // 30.pac) is that exact suffix; every other suffix the game may ask for gets attire 1's model.
    void rosterxInstallModels(const RosterxSlot &s, bool rebind = false)
    {
        const std::filesystem::path dir = std::filesystem::path(s.pacs[0]).parent_path();
        // Nov 10 step 3: a pool number may have belonged to another slot -> drop its entrance codes; unshipped
        // suffix keys are removed below so the old slot's dress/backstage files are never served.
        for (uint32_t k = 0; k < 100u; ++k)
            g_cutEntranceCodes.erase(static_cast<uint16_t>(s.modelNo * 100u + k));
        std::vector<uint32_t> suffixes;
        for (uint32_t a = 0; a < 10; ++a)
            for (uint32_t part : {0u, 1u, 2u, 3u, 4u})
                suffixes.push_back(a * 10u + part);
        for (uint32_t suffix : suffixes)
            {
                std::error_code ec0;
                char own[8];
                std::snprintf(own, sizeof(own), "%02u.pac", suffix);
                std::string src;
                if (std::filesystem::exists(dir / own, ec0))
                    src = (dir / own).string();
                else if (suffix % 10u == 1u || suffix % 10u == 2u)
                {
                    if (rebind)
                    {
                        const std::string k = std::to_string(s.modelNo * 100u + suffix);
                        g_loosePac.erase("/EMD2/" + k);
                        std::error_code rmEc;
                        std::filesystem::remove(std::filesystem::path("MODS") / "PAC" / "EMD2" / k, rmEc);
                    }
                    continue;
                } // backstage part / entrance dress (Nov 6: XXA2 raw SvR gear, e.g. Bret 7702): only when shipped: only when the slot ships one (the game falls back to the ring model)
                else if (suffix / 10u < s.pacs.size())
                    src = s.pacs[suffix / 10u];
                else
                    src = s.pacs[0];
                const std::string key = std::to_string(s.modelNo * 100u + suffix);
                const std::filesystem::path dst = std::filesystem::path("MODS") / "PAC" / "EMD2" / key;
                std::error_code ec;
                std::filesystem::create_directories(dst.parent_path(), ec);
                if (rebind || !std::filesystem::exists(dst, ec) ||
                    std::filesystem::file_size(dst, ec) != std::filesystem::file_size(src, ec))
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
                const uint32_t size = static_cast<uint32_t>(std::filesystem::file_size(dst, ec));
                if (ec || size == 0u)
                {
                    std::fprintf(stderr, "[rosterx] %s: model copy to %s failed%c", s.name.c_str(), dst.string().c_str(), 10);
                    continue;
                }
                auto &e = g_loosePac["/EMD2/" + key];
                e.first = size;
                if (e.second == 0)
                    e.second = -1;
                // Nov 7: a shipped "<a>4.pac" = full entrance model (SvR06 style, e.g. Hogan 80s retail 7204 with
                // the bandana) -> GetEntranceCastNo serves XXa4 at the entrance. Skipped when the slot also ships
                // "<a>2.pac": SvR04 XX04 bodies need their XX02 jacket (Bret 7704/7702), and slot dress casts are
                // not armed yet (IsDress 0x41B5C0 is per stock id) -> keep the ring body there.
                if (suffix % 10u == 4u && std::filesystem::exists(dir / own, ec0))
                {
                    char dress[8];
                    std::snprintf(dress, sizeof(dress), "%02u.pac", suffix - 2u);
                    // Nov 9 JACKET: XX04 entrance body + XX02 dress together (SvR rule: 04 is worn UNDER 02). The
                    // IsDress hook (rosterxSub 0x2af300) returns 1 for the slot when <a>2.pac ships, and suffix 2 is
                    // served above -> arm the entrance body too. HCTP_SLOT_DRESS04=0 -> old (ring body at entrance).
                    const bool oldRule = std::getenv("HCTP_SLOT_DRESS04") && std::strcmp(std::getenv("HCTP_SLOT_DRESS04"), "0") == 0;
                    if (oldRule && std::filesystem::exists(dir / dress, ec0))
                        std::fprintf(stderr, "[rosterx] %s: %s needs dress %s, entrance attire off%c", s.name.c_str(), own,
                                     dress, 10);
                    else
                    {
                        g_cutEntranceCodes.insert(static_cast<uint16_t>(s.modelNo * 100u + suffix));
                        std::fprintf(stderr, "[rosterx] %s: entrance attire %u (%s)%c", s.name.c_str(),
                                     unsigned(s.modelNo * 100u + suffix), own, 10);
                    }
                }
            }
    }
    // ---- SvR entrance reader, engine side (slot.txt entrance=svr04:<SvR event no>) ----
    // SvR04 SEVP = HCTP SEVP layout (rows 12 B {no, numChar, 0, pChar}, chars 28 B, scenes 40 B, effect rows 8 B
    // {no, n, p} -> 16 B entries {type, scene, frame, 0x64, 0, nParam, pParam} -> params {kind, pValue}).
    // CEventSystem::SetEvent 0x166fe0 linear-searches header(this+0xC)+0x8/+0xC rows and deep-copies the row, so the
    // SvR row is appended to the LIVE header (relocated into guest memory) under a virtual event number (3850+k, stage
    // plane variant +100). EVET banks are length-prefixed CEKey streams with their own op-size table -> HCTP walks them
    // unchanged; only camera keys differ (SvR 19 B f32 pos, HCTP 13 B s16 pos), decoded in AnalyzeAttribute.
    // Rules (any SvR04 entrance, nothing per entrance): char objs = runtime casts (model 1 -> 0, InitEventSystem sets
    // the entrant via SetCast 0x167280); SvR props 2000-2199 (except the engine's belt 2019) -> SOBJ id +800 served from
    // SvR bg.pac; effects keep SvR timing, BGM (type 1) + titantron (type 8) values from the slot's HCTP donor event
    // (template's own entrance); SvR light ops (0x11, SvR light motion ids) -> HCTP lighting stays (CMotionLight::Set
    // from SetAttr skipped while an SvR event runs). TODO: camera ops 0x80/0x82 SetHOff/SetHBetween (skipped by size
    // table today), glasses handoff ops 0xB7/0xBA/0xBC, SvR light id -> HCTP light motion table, SvR06.
    // Nov 10 STEP 1 entrance port: donorEv != 0 = fixed HCTP donor event for BGM/tron (else roster tmpl +0xEC).
    struct SvrEvent { uint16_t svrNo; uint8_t tmpl; uint8_t game = 4; int16_t donorEv = 0; uint8_t kind = 1; }; // game 4 = SvR04, 6 = SvR06; kind 1 single, 2 tag
    // Nov 10 STEP 3: cast objects of an appended SvR row (virtual ev -> guest chars, {obj, cast order, scene count}).
    // Visibility is re-applied at EVERY SetEvent (the row is appended once per run; before, a tag match after a singles
    // one kept the partners hidden). SetCast index k -> k-th cast object (SvR06 tag partner = obj 4, HCTP casts obj 1).
    struct SvrCastObj { uint16_t obj, order, ns; };
    std::map<uint16_t, std::pair<uint32_t, std::vector<SvrCastObj>>> g_svrRowCast;
    std::vector<uint16_t> g_svrCastMap; // active SvR event: HCTP SetCast index -> object index
    // fixed number ranges (ENTRANCE_PORT_MASTER): 7000 SvR04 singles, 7200 SvR04 tags, 7400 SvR06 singles, 7600 SvR06 tags
    uint8_t svrKindOf(uint16_t ev) { return ((ev >= 7200u && ev < 7400u) || (ev >= 7600u && ev < 7800u)) ? uint8_t(2) : uint8_t(1); }
    std::map<uint16_t, SvrEvent> g_svrEvents;      // virtual event no (and +100 variant) -> SvR event
    // Nov 10 Mr. America: virtual event -> HCTP light motion that replaces the SvR lights (entrances.txt light=<n>)
    std::map<uint16_t, uint16_t> g_svrLightOverride;
    uint16_t g_svrCurEv = 0;                       // event of the last CEventSystem::SetEvent
    // MODS\SVR04\entrances.txt (tools\stage_entrance_port.py from notes\ENTRANCE_PORT_MASTER_20261110.csv):
    // "<svr04 no> <hctp event> <donor hctp event> <name>" -> fixed virtual numbers (7000+, plane +100).
    struct SvrTableRow { uint16_t ev; int16_t donor; std::string name; };
    std::map<uint16_t, SvrTableRow> g_svrTable;    // SvR04 event no -> fixed row
    std::map<uint16_t, uint16_t> g_svrRegistered;  // SvR04 event no -> registered virtual event
    int g_svr06NextEv = 3870;                      // SvR06 reader (step 2 gives fixed numbers); 3850 = old Bret alias
    std::vector<uint8_t> g_svrSevp;                // SvR ENTR/SEVP bytes (base = +8)
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> g_svrEvetDir; // SvR EVET no -> (sector, size/0x100)
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> g_svrSobjDir; // SvR SOBJ hex id -> (sector, size/0x100)
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> g_svr06SobjDir; // SvR06 bg.pac SOBJ hex id -> (sector, size/0x100)
    bool g_svrEventActive = false;
    // Nov 10 SvR06 build 10: reader event playing (game 6), CMotionLight ('CMOL' link, seen at CMotionLight::Set),
    // host light bank built from the SvR06 bank's global obj 0xB tracks y1..y8 (see svr06Register / svr06LightsStart).
    bool g_svr06Active = false, g_svr06LightsSet = false;
    uint32_t g_motionLightThis = 0, g_svr06LightBank = 0;
    uint8_t g_svr06LightMask = 0;
    std::vector<uint8_t> g_svr06LightHost;
    PS2Runtime::RecompiledFunction g_origEvsSetEvent = nullptr, g_origEvsEnd = nullptr, g_origEvsRelease = nullptr,
                                   g_origCamAnalyze = nullptr, g_origMotionLightSet = nullptr;
    uint32_t g_svrCamScratch = 0;
    uint32_t g_svrEmotSec = 0, g_svrEmotSz = 0;    // SvR04 evtEnt.pac ENTR/EMOT (PAC of 4 BPE light banks)
    bool svrLoadPacs()
    {
        static int state = 0; // 0 untried, 1 ok, -1 failed
        if (state != 0)
            return state > 0;
        state = -1;
        auto readDir = [](const char *file, auto &&onGroup) -> bool {
            std::ifstream f(file, std::ios::binary);
            if (!f)
                return false;
            std::vector<uint8_t> h(0x4000);
            f.read(reinterpret_cast<char *>(h.data()), 0x4000);
            if (!f || std::memcmp(h.data(), "DPAC", 4) != 0)
                return false;
            for (uint32_t off = 0x800; off + 8 <= 0x4000 && std::memcmp(&h[off], "\0\0\0\0", 4) != 0;)
            {
                char tag[5] = {};
                std::memcpy(tag, &h[off], 4);
                uint16_t n, start;
                std::memcpy(&n, &h[off + 4], 2);
                std::memcpy(&start, &h[off + 6], 2);
                if (n & 0x8000u)
                {
                    n &= 0x7FFFu;
                    uint32_t sec = 8u + start;
                    for (uint32_t i = 0; i < n && off + 12 + 4 * i <= 0x4000; ++i)
                    {
                        uint16_t id, sz;
                        std::memcpy(&id, &h[off + 8 + 4 * i], 2);
                        std::memcpy(&sz, &h[off + 10 + 4 * i], 2);
                        onGroup(tag, std::string(), id, sec, sz);
                        sec += (sz * 0x100u + 0x7FFu) >> 11;
                    }
                    off += 8 + 4 * n;
                }
                else
                {
                    uint32_t cnt = 0;
                    for (; cnt < n && off + 16 + 8 * cnt <= 0x4000 && std::memcmp(&h[off + 8 + 8 * cnt], "\0\0\0\0", 4) != 0; ++cnt)
                    {
                        const char *nm = reinterpret_cast<const char *>(&h[off + 8 + 8 * cnt]);
                        uint16_t sec, sz;
                        std::memcpy(&sec, &h[off + 12 + 8 * cnt], 2);
                        std::memcpy(&sz, &h[off + 14 + 8 * cnt], 2);
                        std::string name(nm, strnlen(nm, 4));
                        while (!name.empty() && name.back() == ' ') name.pop_back();
                        onGroup(tag, name, 0, 8u + sec, sz);
                    }
                    off += 8 + 8 * cnt;
                }
            }
            return true;
        };
        uint32_t sevpSec = 0, sevpSz = 0;
        if (!readDir("MODS/SVR04/evtEnt.pac", [&](const char *tag, const std::string &name, uint16_t id, uint32_t sec, uint32_t sz) {
                if (std::strcmp(tag, "EVET") == 0 && name.empty())
                {
                    // SvR04 EVET = id-form group: hex-digit ids (0x3077 = bank 3077)
                    char hx[8];
                    std::snprintf(hx, sizeof(hx), "%X", id);
                    if (std::strspn(hx, "0123456789") == std::strlen(hx))
                        g_svrEvetDir[static_cast<uint16_t>(std::stoul(hx))] = {sec, sz};
                }
                else if (std::strcmp(tag, "EVET") == 0 && !name.empty() && std::isdigit(static_cast<unsigned char>(name[0])))
                    g_svrEvetDir[static_cast<uint16_t>(std::stoul(name))] = {sec, sz};
                else if (std::strncmp(tag, "ENTR", 4) == 0 && name == "SEVP")
                    sevpSec = sec, sevpSz = sz;
                else if (std::strncmp(tag, "ENTR", 4) == 0 && name == "EMOT")
                    g_svrEmotSec = sec, g_svrEmotSz = sz;
            }) || sevpSz == 0)
        {
            std::fprintf(stderr, "[svrent] MODS\\SVR04\\evtEnt.pac missing or unreadable (copy the original SvR04 file there)%c", 10);
            return false;
        }
        std::ifstream f("MODS/SVR04/evtEnt.pac", std::ios::binary);
        g_svrSevp.resize(sevpSz * 0x100u);
        f.seekg(static_cast<std::streamoff>(sevpSec) * 0x800);
        f.read(reinterpret_cast<char *>(g_svrSevp.data()), static_cast<std::streamsize>(g_svrSevp.size()));
        if (!f || std::memcmp(g_svrSevp.data(), "POF0", 4) != 0 || g_svrSevp[8] != 'V' || g_svrSevp[9] != 'E')
        {
            std::fprintf(stderr, "[svrent] SvR04 ENTR/SEVP not a SEVP%c", 10);
            return false;
        }
        readDir("MODS/SVR04/bg.pac", [&](const char *tag, const std::string &, uint16_t id, uint32_t sec, uint32_t sz) {
            if (std::strcmp(tag, "SOBJ") == 0)
                g_svrSobjDir[id] = {sec, sz};
        });
        // Nov 10 STEP 5: SvR06 bg.pac (props 2024..2083, vehicles 2500-2503) = file 4, same id-form SOBJ group
        // SvR06 DPAC named groups carry DOUBLED counts (evt.pac too) -> own walk: named = n/2 x 8 B, id-form = n&0x7FFF x 4 B
        auto readDir06 = [](const char *file) -> bool {
            std::ifstream f6(file, std::ios::binary);
            std::vector<uint8_t> h6(0x4000);
            if (!f6 || !f6.read(reinterpret_cast<char *>(h6.data()), 0x4000) || std::memcmp(h6.data(), "DPAC", 4) != 0)
                return false;
            for (uint32_t off = 0x800; off + 8 <= 0x4000 && std::memcmp(&h6[off], "\0\0\0\0", 4) != 0;)
            {
                uint16_t n, start;
                std::memcpy(&n, &h6[off + 4], 2);
                std::memcpy(&start, &h6[off + 6], 2);
                if (n & 0x8000u)
                {
                    const uint32_t cnt = n & 0x7FFFu;
                    uint32_t sec = 8u + start;
                    for (uint32_t i = 0; i < cnt && off + 12 + 4 * i <= 0x4000; ++i)
                    {
                        uint16_t id, sz;
                        std::memcpy(&id, &h6[off + 8 + 4 * i], 2);
                        std::memcpy(&sz, &h6[off + 10 + 4 * i], 2);
                        if (std::memcmp(&h6[off], "SOBJ", 4) == 0)
                            g_svr06SobjDir[id] = {sec, sz};
                        sec += (sz * 0x100u + 0x7FFu) >> 11;
                    }
                    off += 8 + 4 * cnt;
                }
                else
                    off += 8 + 8 * (n / 2u);
            }
            return true;
        };
        if (!readDir06("MODS/SVR06/bg.pac"))
            std::fprintf(stderr, "[svr06ent] MODS\\SVR06\\bg.pac missing: SvR06 entrance props hidden (copy SvR06 PAC\\BG.PAC there)%c", 10);
        if (g_svrPacFiles.size() < 5u)
            g_svrPacFiles.resize(5u); // [2] = SvR06 evt.pac, [3] = CEVN.PAC cache (svr06Register), [4] = SvR06 bg.pac
        g_svrPacFiles[4] = {"MODS/SVR06/bg.pac", "MODS\\SVR06\\BG.PAC"};
        g_svrPacFiles[0] = {"MODS/SVR04/evtEnt.pac", "MODS\\SVR04\\EVTENT.PAC"};
        g_svrPacFiles[1] = {"MODS/SVR04/bg.pac", "MODS\\SVR04\\BG.PAC"};
        std::fprintf(stderr, "[svrent] SvR04 evtEnt.pac: %zu EVET banks, SEVP %zu B; bg.pac %zu SOBJ%c", g_svrEvetDir.size(),
                     g_svrSevp.size(), g_svrSobjDir.size(), 10);
        state = 1;
        return true;
    }
    uint32_t svrU32(uint32_t o) { uint32_t v = 0; if (o + 4 <= g_svrSevp.size()) std::memcpy(&v, &g_svrSevp[o], 4); return v; }
    uint16_t svrU16(uint32_t o) { uint16_t v = 0; if (o + 2 <= g_svrSevp.size()) std::memcpy(&v, &g_svrSevp[o], 2); return v; }
    // SvR SEVP row of event no (file offset of the 12 B row) or 0
    uint32_t svrFindRow(uint16_t no)
    {
        const uint32_t n = svrU32(0x10), t = svrU32(0x14) + 8u;
        for (uint32_t i = 0; i < n; ++i)
            if (svrU16(t + 12u * i) == no)
                return t + 12u * i;
        return 0;
    }
    uint16_t svrPropId(uint16_t model) { return (model >= 2000u && model < 2200u && model != 2019u) ? uint16_t(model + 800u) : model; }
    // Nov 10 STEP 5: SvR06 props -> free SOBJ ids (HCTP stock 2001-2116, SvR04 2800-2999): 2000-2199 -> +400 (2400-2599),
    // vehicles 2500-2599 -> +100 (2600-2699); belts 2019/2021 stay the engine belt (2021 -> 2019 in svr06EnsureRow).
    uint16_t svr06PropId(uint16_t model)
    {
        if (model >= 2000u && model < 2200u && model != 2019u && model != 2021u)
            return static_cast<uint16_t>(model + 400u);
        if (model >= 2500u && model < 2600u)
            return static_cast<uint16_t>(model + 100u);
        return model;
    }
    int svrEntranceRegister(unsigned svrNo, uint8_t tmpl)
    {
        if (std::getenv("HCTP_SVRENT") && std::strcmp(std::getenv("HCTP_SVRENT"), "0") == 0)
            return 0;
        if (!svrLoadPacs())
            return 0;
        if (auto done = g_svrRegistered.find(static_cast<uint16_t>(svrNo)); done != g_svrRegistered.end())
            return done->second; // table row already registered at boot (slot.txt entrance=svr04:<no> reuses it)
        const uint32_t row = svrFindRow(static_cast<uint16_t>(svrNo));
        auto bank = g_svrEvetDir.find(static_cast<uint16_t>(svrNo));
        if (row == 0 || bank == g_svrEvetDir.end())
        {
            std::fprintf(stderr, "[svrent] SvR04 event %u not in MODS\\SVR04\\evtEnt.pac%c", svrNo, 10);
            return 0;
        }
        // fixed number from the table, else first free 3850+k (pre-table behaviour, below 3870 = SvR06 counter start)
        auto trow = g_svrTable.find(static_cast<uint16_t>(svrNo));
        uint16_t v = 0;
        if (trow != g_svrTable.end())
            v = trow->second.ev;
        else
            for (uint16_t c = 3851u; c < 3870u && v == 0; ++c)
                if (!g_svrEvents.count(c))
                    v = c;
        if (v == 0)
            return 0;
        const int16_t donorEv = trow != g_svrTable.end() ? trow->second.donor : int16_t(0);
        for (uint16_t ev : {v, uint16_t(v + 100u)})
        {
            g_svrEvents[ev] = {static_cast<uint16_t>(svrNo), tmpl, 4, donorEv, svrKindOf(v)};
            g_svrPac["/EVET/" + std::to_string(ev)] = {0, bank->second.first, bank->second.second};
        }
        g_svrRegistered[static_cast<uint16_t>(svrNo)] = v;
        if (svrNo == 3077u) // Nov 9-10 CAM saves hold Bret's old number 3850 (+100): keep it playing
            for (uint16_t ev : {uint16_t(3850u), uint16_t(3950u)})
            {
                g_svrEvents[ev] = {static_cast<uint16_t>(svrNo), tmpl, 4, donorEv};
                g_svrPac["/EVET/" + std::to_string(ev)] = {0, bank->second.first, bank->second.second};
            }
        // props of the event: SvR model ids -> free SOBJ ids served from SvR bg.pac
        const uint32_t nChar = svrU16(row + 2), pChar = svrU32(row + 8) + 8u;
        for (uint32_t c = 0; c < nChar; ++c)
        {
            const uint32_t q = pChar + 28u * c;
            const uint16_t model = svrU16(q + 2);
            if (q + 8 > g_svrSevp.size() || g_svrSevp[q + 6] != 1u || svrPropId(model) == model)
                continue;
            const uint16_t hexId = static_cast<uint16_t>(std::stoul(std::to_string(model), nullptr, 16));
            auto so = g_svrSobjDir.find(hexId);
            if (so == g_svrSobjDir.end())
                std::fprintf(stderr, "[svrent] event %u prop %u: not in SvR04 bg.pac%c", svrNo, model, 10);
            else
                g_svrPac["/SOBJ/" + std::to_string(svrPropId(model))] = {1, so->second.first, so->second.second};
        }
        std::fprintf(stderr, "[svrent] SvR04 event %u -> HCTP event %u/%u '%s' (%u objs, donor ev %d / template 0x%02X)%c", svrNo, v,
                     unsigned(v + 100u), trow != g_svrTable.end() ? trow->second.name.c_str() : "-", nChar, donorEv, tmpl, 10);
        return v;
    }
    // Nov 10 STEP 1: register every table row at boot (hooks install with no slot.txt user; CAM-saved numbers play in matches)
    void svrEntranceTableLoad()
    {
        std::ifstream f("MODS/SVR04/entrances.txt");
        if (!f)
        {
            std::fprintf(stderr, "[svrent] MODS\\SVR04\\entrances.txt missing: no fixed SvR04 entrance numbers%c", 10);
            return;
        }
        for (std::string line; std::getline(f, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            unsigned no, ev; int donor, used = 0;
            if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%u %u %d %n", &no, &ev, &donor, &used) < 3)
                continue;
            if (ev < 3000u || ev > 32000u)
                continue;
            g_svrTable[static_cast<uint16_t>(no)] = {static_cast<uint16_t>(ev), static_cast<int16_t>(donor),
                                                     used > 0 ? line.substr(static_cast<size_t>(used)) : std::string()};
        }
        int ok = 0;
        for (const auto &kv : g_svrTable)
            ok += svrEntranceRegister(kv.first, 0x2Du) > 0 ? 1 : 0;
        std::fprintf(stderr, "[svrent] table: %zu SvR04 rows, %d registered%c", g_svrTable.size(), ok, 10);
    }
    uint32_t gU32(uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; }
    uint16_t gU16(uint8_t *rdram, uint32_t a) { uint16_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 2); return v; }
    void gPut(uint8_t *rdram, uint32_t a, const void *src, size_t n) { std::memcpy(rdram + (a & 0x1FFFFFFu), src, n); }
    // append the SvR row (+ its effects row) to the live SEVP header if missing
    void svrEnsureRow(uint8_t *rdram, PS2Runtime *runtime, uint32_t header, uint16_t ev, const SvrEvent &se)
    {
        const uint32_t evCount = gU16(rdram, header + 8), evRows = gU32(rdram, header + 0xC);
        for (uint32_t i = 0; i < evCount; ++i)
            if (gU16(rdram, evRows + 12u * i) == ev)
                return;
        const uint32_t row = svrFindRow(se.svrNo);
        if (row == 0)
            return;
        // chars + scenes
        const uint16_t nChar = svrU16(row + 2);
        const uint32_t pChar = svrU32(row + 8) + 8u;
        const uint32_t chars = runtime->guestMalloc(28u * nChar + 16u);
        // singles (every team size 1): SvR partner objects (2nd+ runtime-cast char) get no scenes, else they show
        // as cast 0 (The Rock). Object indices stay (bank keys / parents use them).
        int teamSize[8] = {};
        bool singles = true;
        for (int i = 0; i < 8; ++i)
            if (g_entranceTeam[i] >= 0 && g_entranceTeam[i] < 8 && ++teamSize[g_entranceTeam[i]] > 1)
                singles = false;
        uint32_t castChars = 0;
        std::vector<SvrCastObj> casts;
        (void)singles; // STEP 3: visibility applied per SetEvent by svrApplyCastVisibility
        for (uint32_t c = 0; c < nChar; ++c)
        {
            uint8_t ch[28];
            std::memcpy(ch, &g_svrSevp[pChar + 28u * c], 28);
            uint16_t model; std::memcpy(&model, ch + 2, 2);
            if (ch[4] == 1u) // char object: SvR04 tags carry cast codes (3101 = 7301/7401), others model 1
            {
                uint16_t ns0; std::memcpy(&ns0, ch + 0x14, 2);
                casts.push_back({static_cast<uint16_t>(c), static_cast<uint16_t>(castChars++), ns0});
                model = 0; // runtime cast (HCTP generic char), entrant/partners filled by SetCast
            }
            else if (ch[6] == 1u)
                model = svrPropId(model);
            std::memcpy(ch + 2, &model, 2);
            uint16_t ns; std::memcpy(&ns, ch + 0x14, 2);
            uint32_t ps; std::memcpy(&ps, ch + 0x18, 4);
            const uint32_t scenes = runtime->guestMalloc(40u * ns + 16u);
            if (ps + 8u + 40u * ns <= g_svrSevp.size())
                gPut(rdram, scenes, &g_svrSevp[ps + 8u], 40u * ns);
            std::memcpy(ch + 0x18, &scenes, 4);
            gPut(rdram, chars + 28u * c, ch, 28);
        }
        g_svrRowCast[ev] = {chars, casts};
        const uint32_t rows = runtime->guestMalloc(12u * (evCount + 1u) + 16u);
        std::vector<uint8_t> tmp(12u * evCount);
        std::memcpy(tmp.data(), rdram + (evRows & 0x1FFFFFFu), tmp.size());
        gPut(rdram, rows, tmp.data(), tmp.size());
        uint8_t nr[12] = {};
        std::memcpy(nr, &ev, 2);
        std::memcpy(nr + 2, &nChar, 2);
        std::memcpy(nr + 8, &chars, 4);
        gPut(rdram, rows + 12u * evCount, nr, 12);
        const uint32_t evCount1 = evCount + 1u;
        gPut(rdram, header + 0xC, &rows, 4);
        gPut(rdram, header + 8, &evCount1, 4);
        // effects: SvR entries (timing), BGM/tron values from the donor HCTP event (template's own entrance)
        int16_t donor = se.donorEv;
        if (donor == 0)
            std::memcpy(&donor, rdram + 0x4C0F54u + se.tmpl * 0x170u + 0xECu, 2);
        const uint32_t efCount = gU16(rdram, header + 0x10), efRows = gU32(rdram, header + 0x14);
        uint32_t donorEnt = 0, donorN = 0;
        for (uint32_t i = 0; i < efCount; ++i)
            if (gU16(rdram, efRows + 8u * i) == static_cast<uint16_t>(donor))
                donorN = gU16(rdram, efRows + 8u * i + 2), donorEnt = gU32(rdram, efRows + 8u * i + 4);
        const uint32_t se0 = svrU32(0x1C) + 8u, sn = svrU32(0x18);
        uint32_t svrN = 0, svrEnt = 0;
        for (uint32_t i = 0; i < sn; ++i)
            if (svrU16(se0 + 8u * i) == se.svrNo)
                svrN = svrU16(se0 + 8u * i + 2), svrEnt = svrU32(se0 + 8u * i + 4) + 8u;
        const uint32_t ents = runtime->guestMalloc(16u * (svrN + 1u) + 16u);
        for (uint32_t k = 0; k < svrN; ++k)
        {
            uint8_t e[16];
            std::memcpy(e, &g_svrSevp[svrEnt + 16u * k], 16);
            uint16_t type, np; uint32_t pp;
            std::memcpy(&type, e, 2); std::memcpy(&np, e + 10, 2); std::memcpy(&pp, e + 12, 4);
            bool fromDonor = false;
            if (type == 1u || type == 8u)
                for (uint32_t j = 0; j < donorN && !fromDonor; ++j)
                    if (gU16(rdram, donorEnt + 16u * j) == type)
                    {
                        const uint16_t dnp = gU16(rdram, donorEnt + 16u * j + 10);
                        const uint32_t dpp = gU32(rdram, donorEnt + 16u * j + 12);
                        std::memcpy(e + 10, &dnp, 2);
                        std::memcpy(e + 12, &dpp, 4);
                        fromDonor = true;
                    }
            if (!fromDonor && np != 0u && pp != 0u)
            {
                const uint32_t params = runtime->guestMalloc(8u * np + 4u * np + 16u);
                for (uint32_t j = 0; j < np; ++j)
                {
                    const uint32_t kind = svrU32(pp + 8u + 8u * j), pv = svrU32(pp + 8u + 8u * j + 4u);
                    const uint32_t val = svrU32(pv + 8u), store = params + 8u * np + 4u * j;
                    gPut(rdram, store, &val, 4);
                    gPut(rdram, params + 8u * j, &kind, 4);
                    gPut(rdram, params + 8u * j + 4u, &store, 4);
                }
                std::memcpy(e + 12, &params, 4);
            }
            else if (!fromDonor)
                std::memset(e + 12, 0, 4);
            gPut(rdram, ents + 16u * k, e, 16);
        }
        const uint32_t frows = runtime->guestMalloc(8u * (efCount + 1u) + 16u);
        std::vector<uint8_t> ftmp(8u * efCount);
        std::memcpy(ftmp.data(), rdram + (efRows & 0x1FFFFFFu), ftmp.size());
        gPut(rdram, frows, ftmp.data(), ftmp.size());
        uint8_t fr[8];
        const uint16_t svrN16 = static_cast<uint16_t>(svrN);
        std::memcpy(fr, &ev, 2); std::memcpy(fr + 2, &svrN16, 2); std::memcpy(fr + 4, &ents, 4);
        gPut(rdram, frows + 8u * efCount, fr, 8);
        const uint32_t efCount1 = efCount + 1u;
        gPut(rdram, header + 0x14, &frows, 4);
        gPut(rdram, header + 0x10, &efCount1, 4);
        std::fprintf(stderr, "[svrent] SEVP header %08x: + event %u (SvR04 %u, %u objs, %u effects, donor %d)%c", header, ev,
                     se.svrNo, nChar, svrN, donor, 10);
    }
    // ---- Nov 9 SvR06 entrance reader (proof: Brock 4044). Original MODS\SVR06\evt.pac as-is: DPAC with doubled group
    // counts, rows {name[4], u16 sector, u16 size/0x100}, data 0x4000 + sector*0x800. EVNT/<no> = HCTP CEKey bank
    // (served raw like SvR04 EVET), EVTS/<no> = script: u16 objs @0x42, ptr @0x48 (+8) -> 28 B objects (HCTP layout),
    // each 21 x 212 B slots {u16 slot, u8 on, u8 0, u32 tid, u32 x, u32 y, f32 x y z yaw ?, ..., u16 parent @208}.
    // -> HCTP 40 B scenes = slot bytes 0..35 + parent (segments = slots 0/5/10/15/20, bank key x = slot no).
    // Entrant char = runtime cast; chars/props with no slot on and the global light object (SvR06 inline lights,
    // TODO) get no scenes; belt 2021 -> HCTP 2019. Effects (BGM/tron) = donor HCTP event's row. Camera 29 B keys
    // [c][f32 pos x3][f32 rot rad x3][f32 fov] -> camAnalyzeSvr. Source: Mod Maker svr06_entrance.py + 4044 dump.
    std::map<uint16_t, std::vector<uint8_t>> g_svr06Script;
    std::vector<uint8_t> g_svr06Cache;                                  // STEP 2: combined patched banks (CEVN.PAC)
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> g_svr06Raw;      // virtual ev -> raw evt.pac (sector, size/0x100)
    std::map<uint16_t, uint16_t> g_svr06Registered;                    // SvR06 event no -> virtual event
    std::map<uint16_t, std::pair<std::vector<uint8_t>, uint8_t>> g_svr06Lights; // SvR06 no -> host light bank + mask
    size_t g_svr06LightMax = 0;
    // Nov 10 STEP 5 nWo GRAYSCALE (Hollywood Hogan 2 = SvR06 4071): SvR06 camera attr op 0x3A = MotionCameraBase::SetAttr
    // -> MakeCamEff(a, b) (SvR06 0x5020d0): a 31..38 = CGrayscaleFast SetClut(a-30) + fade in over b frames, a 30 = fade
    // back to colour over b frames (4071: on scene 0 f30, off scene 4 (x 20) f280). Not in the SvR06 script effect table
    // (types 1/8 only; SvR06 effect 6 ScreenColor is unused by retail). HCTP: effect type 6 CEventColorFilter (start/stop
    // scene+frame = entry +2/+4 and +6/+8) param 1 = PalleteChange 0x195c60 grayscale (2R+4G+B)/7 on registered CLUTs.
    // No fade in HCTP (switch at the fade midpoint). End/Release force ChangeColorPallte(0) (skipped entrances).
    struct Svr06Gray { uint16_t onSc = 0xFFFF, onFr = 0, offSc = 100, offFr = 0; };
    std::map<uint16_t, Svr06Gray> g_svr06Gray; // SvR06 event no -> grayscale window
    bool g_svrGrayArmed = false;
    int svr06Register(unsigned no, uint8_t tmpl, uint16_t fixedEv = 0, int16_t donorEv = 0)
    {
        if (auto done = g_svr06Registered.find(static_cast<uint16_t>(no)); done != g_svr06Registered.end())
            return done->second;
        std::ifstream f("MODS/SVR06/evt.pac", std::ios::binary);
        std::vector<uint8_t> h(0x4000);
        if (!f || !f.read(reinterpret_cast<char *>(h.data()), 0x4000) || std::memcmp(h.data(), "DPAC", 4) != 0)
        {
            std::fprintf(stderr, "[svr06ent] MODS\\SVR06\\evt.pac missing (copy SvR06 PAC\\EVT.PAC there)%c", 10);
            return 0;
        }
        const std::string want = std::to_string(no);
        uint32_t bSec = 0, bSz = 0, sSec = 0, sSz = 0;
        for (uint32_t o = 0x800; o + 8 <= 0x4000 && std::memcmp(&h[o], "\0\0\0\0", 4) != 0;)
        {
            char tag[5] = {};
            std::memcpy(tag, &h[o], 4);
            uint16_t n2;
            std::memcpy(&n2, &h[o + 4], 2);
            o += 8;
            for (uint32_t i = 0; i < n2 / 2u && o + 8 <= 0x4000; ++i, o += 8)
            {
                std::string nm(reinterpret_cast<const char *>(&h[o]), strnlen(reinterpret_cast<const char *>(&h[o]), 4));
                while (!nm.empty() && nm.back() == ' ') nm.pop_back();
                uint16_t sec, sz;
                std::memcpy(&sec, &h[o + 4], 2);
                std::memcpy(&sz, &h[o + 6], 2);
                if (nm != want)
                    continue;
                if (std::strcmp(tag, "EVNT") == 0) bSec = 8u + sec, bSz = sz;
                if (std::strcmp(tag, "EVTS") == 0) sSec = 8u + sec, sSz = sz;
            }
        }
        if (bSz == 0u || sSz == 0u)
        {
            std::fprintf(stderr, "[svr06ent] SvR06 event %u not in evt.pac (bank %u script %u)%c", no, bSz, sSz, 10);
            return 0;
        }
        std::vector<uint8_t> sc(sSz * 0x100u);
        f.clear();
        f.seekg(static_cast<std::streamoff>(sSec) * 0x800);
        f.read(reinterpret_cast<char *>(sc.data()), static_cast<std::streamsize>(sc.size()));
        if (!f || std::memcmp(sc.data(), "POF0", 4) != 0 || sc[8] != 'V' || sc[9] != 'E')
        {
            std::fprintf(stderr, "[svr06ent] EVTS/%u not a script%c", no, 10);
            return 0;
        }
        g_svr06Script[static_cast<uint16_t>(no)] = std::move(sc);
        // STAGE HEIGHT (Jordan test 6: Brock walked through the stage): SvR06 stage = floor level, HCTP stage is 11.8
        // above the floor (y down; Mod Maker hctp_entrance.py ground/shift_at: FLOOR 11.8, ramp z 87..237). Inverse of
        // shift_at: root y -= (FLOOR - ground(z)) / 0.01 on the entrant's body tracks (obj 0, y 0, first pose sub).
        // The patched bank goes to MODS\SVR06\C<no>.EVN (host cache, rebuilt each boot) and is served from there.
        // HCTP_SVR06_GROUND=0 -> raw bank.
        std::vector<uint8_t> bk(bSz * 0x100u);
        f.clear();
        f.seekg(static_cast<std::streamoff>(bSec) * 0x800);
        f.read(reinterpret_cast<char *>(bk.data()), static_cast<std::streamsize>(bk.size()));
        // Nov 10 STEP 2: pass 0 = stage/ramp arenas (event v), pass 1 = plane arenas (v+100, floor level: no stage shift,
        // resample + camera split kept). Both banks go into ONE host cache MODS\SVR06\CEVN.PAC (one loose entry slot for
        // all SvR06 events), written once after the boot table (svr06CacheFlush).
        int fileIdx = 2;
        uint32_t passSec[2] = {bSec, bSec}, passSz[2] = {bSz, bSz};
        const std::vector<uint8_t> bkRaw = bk;
        const uint32_t bSzRaw = bSz;
        for (int pass = 0; pass < 2; ++pass)
        if (f && bk.size() > 0x104u && !(std::getenv("HCTP_SVR06_GROUND") && std::getenv("HCTP_SVR06_GROUND")[0] == '0'))
        {
            if (pass == 1)
                bk = bkRaw, bSz = bSzRaw;
            uint32_t n = 0;
            std::memcpy(&n, &bk[0x100], 4);
            const size_t data0 = 0x104u + 16u * size_t(n);
            auto shift = [pass](float zUnits) {
                if (pass == 1)
                    return 0.f; // plane arena: SvR06 floor-level stage = HCTP floor
                const float z = zUnits * 0.01f, F = 11.8f, top = 237.f, foot = 87.f;
                const float g = z >= top ? 0.f : (z <= foot ? F : F * (top - z) / (top - foot));
                return (F - g) / 0.01f;
            };
            // BUILD 8 (Nov 9, test 7 + Jordan video): (a) entrant records = per-bone subs 0x00-0x14 + a TRAILING 0x7f f32
            // root -> patch every root sub, not only the first; (b) camera tracks (id 0x400A, 29 B f32 keys) get the
            // entrant's stage shift of the same scene/frame so the shot follows him up (Jordan: camera too low); (c) jitter:
            // stock HCTP entrances key EVERY frame with count 0, SvR06 keys every 2nd frame with count 1 (DataAnalysis
            // 0x1b7220 blends (count+1)*2 ticks) -> resample entrant tracks to per-frame count-0 keys (mid = halfway).
            // HCTP_SVR06_RESAMPLE=0 / HCTP_SVR06_CAMSHIFT=0 turn (c) / (b) off.
            static const uint8_t kWide[21] = {7, 7, 7, 6, 23, 6, 7, 3, 6, 6, 7, 3, 6, 7, 2, 6, 2, 7, 2, 6, 2};
            auto boneSz = [](uint8_t t) -> size_t { return t == 2 || t == 4 ? 2u : (t == 3 || t == 6 ? 4u : 6u); };
            const bool resample = !(std::getenv("HCTP_SVR06_RESAMPLE") && std::getenv("HCTP_SVR06_RESAMPLE")[0] == '0');
            const bool camShift = !(std::getenv("HCTP_SVR06_CAMSHIFT") && std::getenv("HCTP_SVR06_CAMSHIFT")[0] == '0');
            struct Ent { uint32_t key, off; };
            std::vector<Ent> dir(n);
            for (uint32_t i = 0; i < n; ++i)
                std::memcpy(&dir[i].key, &bk[0x104u + 16u * i], 4), std::memcpy(&dir[i].off, &bk[0x104u + 16u * i + 4u], 4);
            std::vector<uint32_t> offs;
            for (const Ent &e : dir)
                if (e.off != 0xFFFFFu && data0 + e.off < bk.size()) offs.push_back(e.off);
            std::sort(offs.begin(), offs.end());
            offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
            auto segEnd = [&](uint32_t o) {
                auto it = std::upper_bound(offs.begin(), offs.end(), o);
                return it == offs.end() ? uint32_t(bk.size() - data0) : *it;
            };
            auto walk = [&](size_t p, size_t pe, auto &&fn) {
                uint32_t fr = 0;
                while (p < pe)
                {
                    const uint8_t c = bk[p];
                    if (c < 0x60u) { fr += c; ++p; continue; }
                    if (c == 0x60u && p + 1 >= pe) return false;
                    const size_t sz = c == 0x60u ? bk[p + 1] : size_t(c - 0x60u);
                    p += c == 0x60u ? 2u : 1u;
                    if (p + sz > pe) return false;
                    fn(fr, p, sz);
                    p += sz;
                    ++fr;
                }
                return true;
            };
            if (pass == 0)
            {
                // Nov 10 LIGHTS: global obj 0xB, scene x 0, tracks y1..y8 = the 8 CMotionLight slots, same key stream
                // CMotionLight::Action 0x1ff9d0 reads ([count][s16 dir x3][u16 RGB x3]). -> own CEKey bank with light
                // motion ids 0x3F00+slot (stock stage lights use 14000/15000 + stage*10 + slot).
                std::vector<uint8_t> lrows, ldata;
                uint32_t lcnt = 0;
                g_svr06LightMask = 0;
                for (uint32_t y = 1; y <= 8; ++y)
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        if ((dir[i].key & 0x3FFFFFFFu) != (0x000B0000u | y) || dir[i].off == 0xFFFFFu || data0 + dir[i].off >= bk.size())
                            continue;
                        uint32_t r[4];
                        std::memcpy(r, &bk[0x104u + 16u * i], 16);
                        r[0] = (r[0] & 0xC0000000u) | ((0x3F00u + y - 1u) << 16);
                        r[1] = static_cast<uint32_t>(ldata.size());
                        ldata.insert(ldata.end(), bk.begin() + data0 + dir[i].off, bk.begin() + data0 + segEnd(dir[i].off));
                        lrows.insert(lrows.end(), reinterpret_cast<uint8_t *>(r), reinterpret_cast<uint8_t *>(r) + 16);
                        g_svr06LightMask |= static_cast<uint8_t>(1u << (y - 1u));
                        ++lcnt;
                        break;
                    }
                g_svr06LightHost.assign(bk.begin(), bk.begin() + 0x100);
                g_svr06LightHost.insert(g_svr06LightHost.end(), reinterpret_cast<uint8_t *>(&lcnt), reinterpret_cast<uint8_t *>(&lcnt) + 4);
                g_svr06LightHost.insert(g_svr06LightHost.end(), lrows.begin(), lrows.end());
                g_svr06LightHost.insert(g_svr06LightHost.end(), ldata.begin(), ldata.end());
                g_svr06LightHost.resize(g_svr06LightHost.size() + 32u, 0);
                if (lcnt == 0) g_svr06LightHost.clear();
                std::fprintf(stderr, "[svr06light] %u light tracks (mask %02x, bank %zu B)%c", lcnt, g_svr06LightMask, g_svr06LightHost.size(), 10);
                g_svr06Lights[static_cast<uint16_t>(no)] = {g_svr06LightHost, g_svr06LightMask}; // STEP 2: per event
                g_svr06LightMax = std::max(g_svr06LightMax, g_svr06LightHost.size());
                // STEP 5: grayscale window from the camera track (obj 0x0A, take y0) attr blocks: 3A <type> <frames>
                Svr06Gray gw;
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uint32_t key = dir[i].key;
                    if (((key >> 16) & 0x3FFFu) != 0x0Au || (key & 0xFFu) != 0u || dir[i].off == 0xFFFFFu || data0 + dir[i].off >= bk.size())
                        continue;
                    const uint16_t sc = static_cast<uint16_t>(((key >> 8) & 0xFFu) / 5u);
                    walk(data0 + dir[i].off, data0 + segEnd(dir[i].off), [&](uint32_t fr, size_t p, size_t sz) {
                        if (!sz || !(bk[p] & 0x80u))
                            return;
                        const size_t b1 = std::min(p + 1u + (bk[p] & 0x7Fu), std::min(p + sz, bk.size()));
                        for (size_t q = p + 1u; q + 2u < b1 + 1u; ++q)
                        {
                            if (bk[q] != 0x3Au || bk[q + 1] < 0x1Eu || bk[q + 1] > 0x26u)
                                continue;
                            const uint16_t at = static_cast<uint16_t>(fr + bk[q + 2] / 2u); // fade midpoint
                            if (bk[q + 1] >= 0x1Fu)
                            {
                                if (gw.onSc == 0xFFFFu || sc < gw.onSc || (sc == gw.onSc && at < gw.onFr))
                                    gw.onSc = sc, gw.onFr = at;
                            }
                            else if (gw.onSc != 0xFFFFu && (sc > gw.onSc || at > gw.onFr))
                                gw.offSc = sc, gw.offFr = at;
                            q += 2u;
                        }
                    });
                }
                if (gw.onSc != 0xFFFFu)
                {
                    g_svr06Gray[static_cast<uint16_t>(no)] = gw;
                    std::fprintf(stderr, "[svr06ent] event %u grayscale (SvR06 MakeCamEff 31): on scene %u f%u, off scene %u f%u%c", no,
                                 gw.onSc, gw.onFr, gw.offSc, gw.offFr, 10);
                }
            }
            std::map<uint32_t, std::vector<float>> shiftAt; // scene x -> per frame entrant shift
            std::set<uint32_t> entrantOffs;
            uint32_t keys = 0, tracks = 0, camKeys = 0;
            // STEP 4: every cast object (char with a slot on: singles obj 0, tags obj 0 + partner obj 4) gets the stage
            // shift + per-frame resample; the camera follows the entrant (obj 0) only.
            std::set<uint32_t> castObjs = {0u};
            if (auto sit = g_svr06Script.find(static_cast<uint16_t>(no)); sit != g_svr06Script.end())
            {
                const std::vector<uint8_t> &d = sit->second;
                auto u16 = [&d](uint32_t o) { uint16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, &d[o], 2); return v; };
                auto u32 = [&d](uint32_t o) { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, &d[o], 4); return v; };
                const uint32_t t = u32(0x48) + 8u;
                for (uint32_t c = 0; c < u16(0x42) && t + 28u * c + 28u <= d.size(); ++c)
                {
                    const uint32_t q = t + 28u * c, nsl = u32(q + 20) & 0xFFFFu, bp = u32(q + 24) + 8u;
                    bool anyOn = false;
                    for (uint32_t k = 0; k < 5u && 5u * k < nsl; ++k)
                        anyOn |= bp + 212u * 5u * k + 3u <= d.size() && d[bp + 212u * 5u * k + 2u] != 0u;
                    if (d[q + 4] == 1u && d[q + 9] != 1u && anyOn)
                        castObjs.insert(c);
                }
            }
            std::vector<float> partnerTab;
            for (const Ent &e : dir)
            {
                const uint32_t obj = (e.key >> 16) & 0x3FFFu;
                if (!castObjs.count(obj) || (e.key & 0xFFu) != 0u || e.off == 0xFFFFFu ||
                    data0 + e.off >= bk.size() || entrantOffs.count(e.off))
                    continue;
                entrantOffs.insert(e.off);
                ++tracks;
                partnerTab.clear();
                std::vector<float> &tab = obj == 0u ? shiftAt[(e.key >> 8) & 0xFFu] : partnerTab;
                std::vector<uint8_t> setAt;
                float lastShift = 0.f;
                walk(data0 + e.off, data0 + segEnd(e.off), [&](uint32_t fr, size_t p, size_t sz) {
                    size_t q = p;
                    if (sz && (bk[q] & 0x80u)) q += 1u + (bk[q] & 0x7Fu);
                    const size_t recEnd = p + sz;
                    auto patchF = [&](size_t ro) {
                        float xyz[3]; std::memcpy(xyz, &bk[ro], 12);
                        lastShift = shift(xyz[2]);
                        xyz[1] -= lastShift;
                        std::memcpy(&bk[ro], xyz, 12); ++keys;
                    };
                    if (q < recEnd)
                    {
                        const uint8_t op = bk[q];
                        if (op == 0x7d || op == 0x7f) { if (q + 14 <= recEnd) patchF(q + 2); }
                        else if (op == 0x7c || op == 0x7e || op == 0x7a || (op >= 0x50 && op <= 0x6f))
                        {
                            const size_t ro = (op >= 0x50 && op <= 0x6f) ? q + 1 : q + 2;
                            if (ro + 6 <= recEnd)
                            {
                                int16_t xyz[3]; std::memcpy(xyz, &bk[ro], 6);
                                lastShift = shift(float(xyz[2]));
                                const float y = float(xyz[1]) - lastShift;
                                xyz[1] = static_cast<int16_t>(std::lround(std::clamp(y, -32768.f, 32767.f)));
                                std::memcpy(&bk[ro], xyz, 6); ++keys;
                            }
                        }
                        else if (op < 0x15u && recEnd >= q + 14 && bk[recEnd - 14] == 0x7fu)
                            patchF(recEnd - 12); // trailing 0x7f root after per-bone subs
                    }
                    if (tab.size() <= fr) tab.resize(fr + 1u, 0.f), setAt.resize(fr + 1u, 0);
                    tab[fr] = lastShift; setAt[fr] = 1;
                });
                for (size_t k = 1; k < tab.size(); ++k) // frames without a record hold the previous shift
                    if (!setAt[k]) tab[k] = tab[k - 1];
            }
            if (camShift)
            {
                std::set<uint32_t> camOffs;
                for (const Ent &e : dir)
                {
                    if ((e.key >> 16) != 0x400Au || e.off == 0xFFFFFu || data0 + e.off >= bk.size() || camOffs.count(e.off)) continue;
                    camOffs.insert(e.off);
                    auto it = shiftAt.find((e.key >> 8) & 0xFFu);
                    if (it == shiftAt.end() || it->second.empty()) continue;
                    const std::vector<float> &tab = it->second;
                    walk(data0 + e.off, data0 + segEnd(e.off), [&](uint32_t fr, size_t p, size_t sz) {
                        size_t q = p;
                        if (sz && (bk[q] & 0x80u)) q += 1u + (bk[q] & 0x7Fu);
                        if (p + sz != q + 29u) return;
                        float y; std::memcpy(&y, &bk[q + 5], 4);
                        y -= tab[std::min<size_t>(fr, tab.size() - 1)];
                        std::memcpy(&bk[q + 5], &y, 4); ++camKeys;
                    });
                }
            }
            // Nov 10 (Jordan: whole-body shake on the ramp; bone probe = 0 spikes on all 71 bones -> view, not model): camera
            // tracks 0x400A key every 2nd frame with count 1 like the entrant did -> split them to per-frame count-0 keys too.
            // 29 B key [count][f32 pos x3][f32 rot x3 rad][f32 fov]. HCTP_SVR06_CAMRESAMPLE=0 = off.
            const bool camResample = !(std::getenv("HCTP_SVR06_CAMRESAMPLE") && std::getenv("HCTP_SVR06_CAMRESAMPLE")[0] == '0');
            std::set<uint32_t> camTrackOffs;
            for (const Ent &e : dir)
                if ((e.key >> 16) == 0x400Au && e.off != 0xFFFFFu && data0 + e.off < bk.size()) camTrackOffs.insert(e.off);
            uint32_t added = 0, camAdded = 0, camCnt[3] = {};
            if (resample && !offs.empty())
            {
                std::vector<uint8_t> nd;
                std::map<uint32_t, uint32_t> remap;
                if (offs.front() != 0u) nd.insert(nd.end(), bk.begin() + data0, bk.begin() + data0 + offs.front());
                for (uint32_t o : offs)
                {
                    remap[o] = static_cast<uint32_t>(nd.size());
                    const size_t ps = data0 + o, pe = data0 + segEnd(o);
                    if (camResample && camTrackOffs.count(o) && !entrantOffs.count(o))
                    {
                        std::vector<uint8_t> out;
                        float pc[7] = {}; bool hc = false;
                        auto putCam = [&](const std::vector<uint8_t> &r) {
                            if (r.size() < 0xA0u) out.push_back(uint8_t(0x60u + r.size()));
                            else out.push_back(0x60u), out.push_back(uint8_t(r.size()));
                            out.insert(out.end(), r.begin(), r.end());
                        };
                        size_t p = ps;
                        while (p < pe)
                        {
                            const uint8_t c = bk[p];
                            if (c < 0x60u) { out.push_back(c); ++p; continue; }
                            if (c == 0x60u && p + 1 >= pe) { out.insert(out.end(), bk.begin() + p, bk.begin() + pe); break; }
                            const size_t sz = c == 0x60u ? bk[p + 1] : size_t(c - 0x60u);
                            const size_t r0 = p + (c == 0x60u ? 2u : 1u), r1 = r0 + sz;
                            if (r1 > pe) { out.insert(out.end(), bk.begin() + p, bk.begin() + pe); break; }
                            size_t q = r0;
                            if (sz && (bk[q] & 0x80u)) q += 1u + (bk[q] & 0x7Fu);
                            if (q < r1 && r1 == q + 29u)
                            {
                                float v[7]; std::memcpy(v, &bk[q + 1], 28);
                                ++camCnt[std::min<uint8_t>(bk[q], 2u)];
                                if (bk[q] == 1u && r1 < pe && bk[r1] >= 1u && bk[r1] < 0x60u)
                                {
                                    float m[7];
                                    for (int k = 0; k < 7; ++k)
                                    {
                                        if (!hc) { m[k] = v[k]; continue; }
                                        float d = v[k] - pc[k];
                                        if (k >= 3 && k <= 5) d = std::remainder(d, 6.28318531f); // shortest turn
                                        m[k] = pc[k] + 0.5f * d;
                                    }
                                    std::vector<uint8_t> mid(bk.begin() + r0, bk.begin() + r1), tgt(bk.begin() + q, bk.begin() + r1);
                                    mid[q - r0] = 0; tgt[0] = 0;
                                    std::memcpy(&mid[q - r0 + 1], m, 28);
                                    putCam(mid);
                                    putCam(tgt);
                                    ++camAdded;
                                    if (bk[r1] > 1u) out.push_back(uint8_t(bk[r1] - 1u));
                                    std::memcpy(pc, v, 28); hc = true;
                                    p = r1 + 1u;
                                    continue;
                                }
                                std::memcpy(pc, v, 28); hc = true;
                            }
                            out.insert(out.end(), bk.begin() + p, bk.begin() + r1);
                            p = r1;
                        }
                        nd.insert(nd.end(), out.begin(), out.end());
                        continue;
                    }
                    if (!entrantOffs.count(o)) { nd.insert(nd.end(), bk.begin() + ps, bk.begin() + pe); continue; }
                    std::vector<uint8_t> out;
                    int16_t prev[21][3] = {}; bool has[21] = {}; float prevRoot[3] = {}; bool hasRoot = false;
                    auto putRec = [&](const std::vector<uint8_t> &r) {
                        if (r.size() < 0xA0u) out.push_back(uint8_t(0x60u + r.size()));
                        else out.push_back(0x60u), out.push_back(uint8_t(r.size()));
                        out.insert(out.end(), r.begin(), r.end());
                    };
                    auto readBone = [&](size_t at, uint8_t op, int16_t v[3]) {
                        const size_t n16 = boneSz(kWide[op]) / 2u;
                        v[0] = v[1] = v[2] = 0;
                        for (size_t k = 0; k < n16; ++k) std::memcpy(&v[k], &bk[at + 2u + 2u * k], 2);
                        return n16;
                    };
                    size_t p = ps;
                    while (p < pe)
                    {
                        const uint8_t c = bk[p];
                        if (c < 0x60u) { out.push_back(c); ++p; continue; }
                        if (c == 0x60u && p + 1 >= pe) { out.insert(out.end(), bk.begin() + p, bk.begin() + pe); break; }
                        const size_t sz = c == 0x60u ? bk[p + 1] : size_t(c - 0x60u);
                        const size_t r0 = p + (c == 0x60u ? 2u : 1u), r1 = r0 + sz;
                        if (r1 > pe) { out.insert(out.end(), bk.begin() + p, bk.begin() + pe); break; }
                        size_t q = r0;
                        if (sz && (bk[q] & 0x80u)) q += 1u + (bk[q] & 0x7Fu);
                        struct Sub { size_t at; uint8_t op; };
                        std::vector<Sub> subs;
                        bool ok = q < r1, allOne = true;
                        for (size_t t = q; t < r1 && ok;)
                        {
                            const uint8_t op = bk[t];
                            if (op < 21u) { subs.push_back({t, op}); allOne = allOne && bk[t + 1] == 1u; t += 2u + boneSz(kWide[op]); }
                            else if (op == 0x7f) { subs.push_back({t, op}); allOne = allOne && bk[t + 1] == 1u; t += 14u; }
                            else if (op == 0x7d)
                            {
                                subs.push_back({t, op}); allOne = false;
                                size_t u = t + 14u;
                                for (int k = 0; k < 21; ++k) u += boneSz(kWide[k]);
                                t = u;
                            }
                            else ok = false;
                            if (t > r1) ok = false;
                        }
                        const bool canSplit = ok && allOne && r1 < pe && bk[r1] >= 1u && bk[r1] < 0x60u && (r1 - q) < 0xFFu;
                        if (canSplit)
                        {
                            std::vector<uint8_t> mid(bk.begin() + r0, bk.begin() + r1), tgt(bk.begin() + q, bk.begin() + r1);
                            const size_t qo = q - r0;
                            for (const Sub &sb : subs)
                            {
                                const size_t rel = sb.at - r0;
                                mid[rel + 1] = 0; tgt[rel - qo + 1] = 0;
                                if (sb.op < 21u)
                                {
                                    int16_t v[3]; const size_t n16 = readBone(sb.at, sb.op, v);
                                    for (size_t k = 0; k < n16; ++k)
                                    {
                                        const int16_t pv = has[sb.op] ? prev[sb.op][k] : v[k];
                                        const int16_t d = static_cast<int16_t>(uint16_t(v[k]) - uint16_t(pv)); // shortest wrap
                                        const int16_t m = static_cast<int16_t>(uint16_t(pv) + uint16_t(int16_t(d / 2)));
                                        std::memcpy(&mid[rel + 2u + 2u * k], &m, 2);
                                        prev[sb.op][k] = v[k];
                                    }
                                    has[sb.op] = true;
                                }
                                else
                                {
                                    float v[3], m[3]; std::memcpy(v, &bk[sb.at + 2], 12);
                                    for (int k = 0; k < 3; ++k) m[k] = hasRoot ? 0.5f * (prevRoot[k] + v[k]) : v[k], prevRoot[k] = v[k];
                                    hasRoot = true;
                                    std::memcpy(&mid[rel + 2], m, 12);
                                }
                            }
                            putRec(mid);
                            putRec(tgt);
                            ++added;
                            if (bk[r1] > 1u) out.push_back(uint8_t(bk[r1] - 1u));
                            p = r1 + 1u;
                            continue;
                        }
                        if (ok)
                            for (const Sub &sb : subs)
                            {
                                if (sb.op < 21u)
                                {
                                    int16_t v[3]; const size_t n16 = readBone(sb.at, sb.op, v);
                                    for (size_t k = 0; k < n16; ++k) prev[sb.op][k] = v[k];
                                    has[sb.op] = true;
                                    continue;
                                }
                                std::memcpy(prevRoot, &bk[sb.at + 2], 12); hasRoot = true;
                                if (sb.op == 0x7d)
                                {
                                    size_t u = sb.at + 14u;
                                    for (uint8_t k = 0; k < 21; ++k)
                                    {
                                        const size_t n16 = boneSz(kWide[k]) / 2u;
                                        for (size_t j = 0; j < n16; ++j) std::memcpy(&prev[k][j], &bk[u + 2u * j], 2);
                                        has[k] = true; u += boneSz(kWide[k]);
                                    }
                                }
                            }
                        out.insert(out.end(), bk.begin() + p, bk.begin() + r1);
                        p = r1;
                    }
                    nd.insert(nd.end(), out.begin(), out.end());
                }
                for (uint32_t i = 0; i < n; ++i)
                    if (remap.count(dir[i].off)) std::memcpy(&bk[0x104u + 16u * i + 4u], &remap[dir[i].off], 4);
                bk.resize(data0);
                bk.insert(bk.end(), nd.begin(), nd.end());
                bSz = static_cast<uint32_t>((bk.size() + 0xFFu) / 0x100u);
            }
            std::fprintf(stderr, "[svr06ent] build8: %u camera keys shifted, %u entrant records split to per-frame keys (bank %zu B)%c",
                         camKeys, added, bk.size(), 10);
            std::fprintf(stderr, "[svr06ent] nov10 cam resample: %u camera keys split (count0 %u count1 %u count2+ %u)%c",
                         camAdded, camCnt[0], camCnt[1], camCnt[2], 10);
            bk.resize((bk.size() + 0x7FFu) & ~size_t(0x7FFu));
            passSec[pass] = static_cast<uint32_t>(g_svr06Cache.size() / 0x800u);
            passSz[pass] = bSz;
            g_svr06Cache.insert(g_svr06Cache.end(), bk.begin(), bk.end());
            fileIdx = 3;
            std::fprintf(stderr, "[svr06ent] %s: %u entrant tracks, %u root keys moved (cache sector %u)%c",
                         pass == 0 ? "stage height" : "plane (no shift)", tracks, keys, passSec[pass], 10);
        }
        if (g_svrPacFiles.size() < 4u)
            g_svrPacFiles.resize(4u);
        g_svrPacFiles[2] = {"MODS/SVR06/evt.pac", "MODS\\SVR06\\EVT.PAC"};
        g_svrPacFiles[3] = {"MODS/SVR06/CEVN.PAC", "MODS\\SVR06\\CEVN.PAC"};
        const uint16_t v = fixedEv != 0u ? fixedEv : static_cast<uint16_t>(g_svr06NextEv++);
        for (int pass = 0; pass < 2; ++pass)
        {
            const uint16_t ev = static_cast<uint16_t>(v + 100u * pass);
            g_svrEvents[ev] = {static_cast<uint16_t>(no), tmpl, 6, donorEv, svrKindOf(v)};
            g_svrPac["/EVET/" + std::to_string(ev)] = {fileIdx, passSec[pass], passSz[pass]};
            g_svr06Raw[ev] = {bSec, bSzRaw};
        }
        g_svr06Registered[static_cast<uint16_t>(no)] = v;
        // STEP 5: props with a slot on -> SvR06 bg.pac SOBJ under svr06PropId (served from file 4, nothing copied)
        if (auto sit = g_svr06Script.find(static_cast<uint16_t>(no)); sit != g_svr06Script.end() && svrLoadPacs())
        {
            const std::vector<uint8_t> &d = sit->second;
            auto u16 = [&d](uint32_t o) { uint16_t x = 0; if (o + 2 <= d.size()) std::memcpy(&x, &d[o], 2); return x; };
            auto u32 = [&d](uint32_t o) { uint32_t x = 0; if (o + 4 <= d.size()) std::memcpy(&x, &d[o], 4); return x; };
            const uint32_t t = u32(0x48) + 8u;
            for (uint32_t c = 0; c < u16(0x42) && t + 28u * c + 28u <= d.size(); ++c)
            {
                const uint32_t q = t + 28u * c, nsl = u32(q + 20) & 0xFFFFu, bp = u32(q + 24) + 8u;
                const uint16_t model = u16(q + 2);
                bool anyOn = false;
                for (uint32_t k = 0; k < 5u && 5u * k < nsl; ++k)
                    anyOn |= bp + 212u * 5u * k + 3u <= d.size() && d[bp + 212u * 5u * k + 2u] != 0u;
                if (d[q + 6] != 1u || !anyOn || svr06PropId(model) == model)
                    continue;
                const uint16_t hexId = static_cast<uint16_t>(std::stoul(std::to_string(model), nullptr, 16));
                auto so = g_svr06SobjDir.find(hexId);
                if (so == g_svr06SobjDir.end())
                {
                    std::fprintf(stderr, "[svr06ent] event %u prop %u: not in SvR06 bg.pac%c", no, model, 10);
                    continue;
                }
                g_svrPac["/SOBJ/" + std::to_string(svr06PropId(model))] = {4, so->second.first, so->second.second};
                std::fprintf(stderr, "[svr06ent] event %u prop %u -> SOBJ %u (%u B)%c", no, model, svr06PropId(model),
                             so->second.second * 0x100u, 10);
            }
        }
        std::fprintf(stderr, "[svr06ent] SvR06 event %u -> HCTP event %u/%u (bank %u/%u B, donor ev %d / template 0x%02X)%c", no, v,
                     unsigned(v + 100u), passSz[0] * 0x100u, passSz[1] * 0x100u, donorEv, tmpl, 10);
        return v;
    }
    // STEP 2: write the combined host cache once; on failure every SvR06 event falls back to its raw evt.pac bank.
    void svr06CacheFlush()
    {
        if (g_svr06Cache.empty())
            return;
        std::ofstream o("MODS/SVR06/CEVN.PAC", std::ios::binary | std::ios::trunc);
        const bool ok = static_cast<bool>(o.write(reinterpret_cast<const char *>(g_svr06Cache.data()), static_cast<std::streamsize>(g_svr06Cache.size())));
        if (!ok)
            for (const auto &kv : g_svr06Raw)
                g_svrPac["/EVET/" + std::to_string(kv.first)] = {2, kv.second.first, kv.second.second};
        std::fprintf(stderr, "[svr06ent] CEVN.PAC cache %zu B for %zu events: %s%c", g_svr06Cache.size(), g_svr06Registered.size(),
                     ok ? "ok" : "WRITE FAILED, raw banks (no stage shift)", 10);
        std::vector<uint8_t>().swap(g_svr06Cache);
    }
    // STEP 2: MODS\SVR06\entrances.txt "<svr06 no> <hctp event> <donor hctp event> <name>" (tools\stage_entrance_port.py)
    void svr06TableLoad()
    {
        std::ifstream f("MODS/SVR06/entrances.txt");
        const bool opened = static_cast<bool>(f);
        int rows = 0, ok = 0;
        for (std::string line; f && std::getline(f, line);)
        {
            unsigned no, ev; int donor;
            if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%u %u %d", &no, &ev, &donor) < 3 || ev < 3000u || ev > 32000u)
                continue;
            ++rows;
            unsigned light = 0;
            if (const size_t lp = line.find("light="); lp != std::string::npos)
                light = static_cast<unsigned>(std::strtoul(line.c_str() + lp + 6, nullptr, 10));
            // Nov 10 Mr. America: a 2nd row of an already registered SvR06 event = alias (same bank/row/props/pyro), own
            // number; light=<HCTP light motion> replaces the SvR06 lights on that number (svr06LightsStart)
            if (auto base = g_svr06Registered.find(static_cast<uint16_t>(no)); base != g_svr06Registered.end() && base->second != ev)
            {
                for (uint16_t d : {uint16_t(0u), uint16_t(100u)})
                    if (auto be = g_svrEvents.find(static_cast<uint16_t>(base->second + d)); be != g_svrEvents.end())
                    {
                        const uint16_t ae = static_cast<uint16_t>(ev + d);
                        g_svrEvents[ae] = be->second;
                        g_svrEvents[ae].donorEv = static_cast<int16_t>(donor);
                        if (auto bp = g_svrPac.find("/EVET/" + std::to_string(base->second + d)); bp != g_svrPac.end())
                            g_svrPac["/EVET/" + std::to_string(ae)] = bp->second;
                        if (light != 0u)
                            g_svrLightOverride[ae] = static_cast<uint16_t>(light);
                    }
                std::fprintf(stderr, "[svr06ent] alias %u/%u = SvR06 %u (event %u), light override %u%c", ev, ev + 100u, no,
                             unsigned(base->second), light, 10);
                ++ok;
                continue;
            }
            const int got = svr06Register(no, 0x2Du, static_cast<uint16_t>(ev), static_cast<int16_t>(donor));
            ok += got > 0 ? 1 : 0;
            if (got > 0 && light != 0u)
                g_svrLightOverride[static_cast<uint16_t>(got)] = g_svrLightOverride[static_cast<uint16_t>(got + 100)] =
                    static_cast<uint16_t>(light);
        }
        if (!g_svr06Registered.count(4044u)) // no table: keep the Nov 9 Brock proof row (CAM 3944 sentinel)
            svr06Register(4044u, 0xFFu, 0, 3044);
        svr06CacheFlush();
        std::fprintf(stderr, "[svr06ent] table: %d SvR06 rows, %d registered%s%c", rows, ok, opened ? "" : " (MODS\\SVR06\\entrances.txt missing)", 10);
    }
    void svr06EnsureRow(uint8_t *rdram, PS2Runtime *runtime, uint32_t header, uint16_t ev, const SvrEvent &se)
    {
        const uint32_t evCount = gU16(rdram, header + 8), evRows = gU32(rdram, header + 0xC);
        for (uint32_t i = 0; i < evCount; ++i)
            if (gU16(rdram, evRows + 12u * i) == ev)
                return;
        auto sit = g_svr06Script.find(se.svrNo);
        if (sit == g_svr06Script.end())
            return;
        const std::vector<uint8_t> &d = sit->second;
        auto u16 = [&d](uint32_t o) { uint16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, &d[o], 2); return v; };
        auto u32 = [&d](uint32_t o) { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, &d[o], 4); return v; };
        const uint16_t nObj = u16(0x42);
        const uint32_t t = u32(0x48) + 8u;
        const uint32_t chars = runtime->guestMalloc(28u * nObj + 16u);
        std::vector<SvrCastObj> casts; // STEP 3/4: on-chars in order = casts (tag 4201: obj 0 entrant, obj 4 partner)
        for (uint32_t c = 0; c < nObj; ++c)
        {
            const uint32_t q = t + 28u * c;
            if (q + 28u > d.size())
                break;
            uint8_t ch[28];
            std::memcpy(ch, &d[q], 28);
            uint16_t model; std::memcpy(&model, ch + 2, 2);
            const uint32_t nsl = u32(q + 20) & 0xFFFFu, bp = u32(q + 24) + 8u;
            bool anyOn = false;
            for (uint32_t k = 0; k < 5u && 5u * k < nsl; ++k)
                anyOn |= bp + 212u * 5u * k + 3u <= d.size() && d[bp + 212u * 5u * k + 2u] != 0u;
            const bool isChar = ch[4] == 1u, isProp = ch[6] == 1u, isGlobal = ch[9] == 1u;
            bool keep = anyOn && !isGlobal;
            if (isChar && keep)
            {
                casts.push_back({static_cast<uint16_t>(c), static_cast<uint16_t>(casts.size()), uint16_t(5u)});
                model = 0;                     // runtime cast (SetCast fills entrant/partner; svrApplyCastVisibility hides extras)
            }
            else if (isProp && model == 2021u)
                model = 2019u;                 // SvR06 belt -> HCTP belt (op 0xB6 = HCTP belt op)
            else if (isProp && keep)
                model = svr06PropId(model);    // STEP 5: SvR06 bg.pac prop (was: same-number HCTP stock prop)
            std::memcpy(ch + 2, &model, 2);
            const uint32_t w4 = 0xFFFFFFFFu, w5 = 0u, ns = keep ? 5u : 0u;
            std::memcpy(ch + 12, &w4, 4);
            std::memcpy(ch + 16, &w5, 4);
            std::memcpy(ch + 20, &ns, 4);
            const uint32_t scenes = runtime->guestMalloc(40u * 5u + 16u);
            for (uint32_t k = 0; k < 5u; ++k)
            {
                uint8_t s40[40] = {};
                const uint32_t so = bp + 212u * 5u * k;
                if (5u * k < nsl && so + 212u <= d.size())
                {
                    std::memcpy(s40, &d[so], 36);
                    std::memcpy(s40 + 36, &d[so + 208u], 4);
                }
                const uint16_t seg = static_cast<uint16_t>(k);
                std::memcpy(s40, &seg, 2);
                gPut(rdram, scenes + 40u * k, s40, 40);
            }
            std::memcpy(ch + 24, &scenes, 4);
            gPut(rdram, chars + 28u * c, ch, 28);
            std::fprintf(stderr, "[svr06ent] obj %u model %u type %02x%02x%02x%02x%02x%02x%02x%02x -> %s%c", c, model, ch[4], ch[5],
                         ch[6], ch[7], ch[8], ch[9], ch[10], ch[11], keep ? "kept" : "hidden", 10);
        }
        g_svrRowCast[ev] = {chars, casts};
        const uint32_t rows = runtime->guestMalloc(12u * (evCount + 1u) + 16u);
        std::vector<uint8_t> tmp(12u * evCount);
        std::memcpy(tmp.data(), rdram + (evRows & 0x1FFFFFFu), tmp.size());
        gPut(rdram, rows, tmp.data(), tmp.size());
        uint8_t nr[12] = {};
        std::memcpy(nr, &ev, 2);
        std::memcpy(nr + 2, &nObj, 2);
        std::memcpy(nr + 8, &chars, 4);
        gPut(rdram, rows + 12u * evCount, nr, 12);
        const uint32_t evCount1 = evCount + 1u;
        gPut(rdram, header + 0xC, &rows, 4);
        gPut(rdram, header + 8, &evCount1, 4);
        // effects = the donor HCTP event's own row (BGM + titantron of the template)
        int16_t donor = se.donorEv != 0 ? se.donorEv : int16_t(3044); // STEP 2: table donor (BGM/tron rows), else
        if (se.donorEv == 0 && se.tmpl != 0xFFu)                        // tmpl 0xFF = CAM proof row (Brock 3044)
            std::memcpy(&donor, rdram + 0x4C0F54u + se.tmpl * 0x170u + 0xECu, 2);
        const uint32_t efCount = gU16(rdram, header + 0x10), efRows = gU32(rdram, header + 0x14);
        uint32_t donorEnt = 0, donorN = 0;
        for (uint32_t i = 0; i < efCount; ++i)
            if (gU16(rdram, efRows + 8u * i) == static_cast<uint16_t>(donor))
                donorN = gU16(rdram, efRows + 8u * i + 2), donorEnt = gU32(rdram, efRows + 8u * i + 4);
        if (auto gw = g_svr06Gray.find(se.svrNo); gw != g_svr06Gray.end())
        {
            // STEP 5: donor entries + HCTP type 6 ColorFilter {type, on sc, on fr, off sc, off fr, npar 1, params}
            const uint32_t ents = runtime->guestMalloc(16u * (donorN + 1u) + 32u);
            for (uint32_t j = 0; j < donorN; ++j)
            {
                uint8_t e[16];
                std::memcpy(e, rdram + ((donorEnt + 16u * j) & 0x1FFFFFFu), 16);
                gPut(rdram, ents + 16u * j, e, 16);
            }
            const uint32_t par = ents + 16u * (donorN + 1u);
            const uint32_t kind = 3u, valAt = par + 8u, gray = 1u;
            gPut(rdram, par, &kind, 4);
            gPut(rdram, par + 4u, &valAt, 4);
            gPut(rdram, valAt, &gray, 4);
            const uint16_t e16[6] = {6u, gw->second.onSc, gw->second.onFr, gw->second.offSc, gw->second.offFr, 1u};
            gPut(rdram, ents + 16u * donorN, e16, 12);
            gPut(rdram, ents + 16u * donorN + 12u, &par, 4);
            donorEnt = ents;
            ++donorN;
            std::fprintf(stderr, "[svr06ent] event %u: + ColorFilter grayscale effect (scene %u f%u -> scene %u f%u)%c", ev,
                         gw->second.onSc, gw->second.onFr, gw->second.offSc, gw->second.offFr, 10);
        }
        const uint32_t frows = runtime->guestMalloc(8u * (efCount + 1u) + 16u);
        std::vector<uint8_t> ftmp(8u * efCount);
        std::memcpy(ftmp.data(), rdram + (efRows & 0x1FFFFFFu), ftmp.size());
        gPut(rdram, frows, ftmp.data(), ftmp.size());
        uint8_t fr[8];
        const uint16_t dn16 = static_cast<uint16_t>(donorN);
        std::memcpy(fr, &ev, 2); std::memcpy(fr + 2, &dn16, 2); std::memcpy(fr + 4, &donorEnt, 4);
        gPut(rdram, frows + 8u * efCount, fr, 8);
        const uint32_t efCount1 = efCount + 1u;
        gPut(rdram, header + 0x14, &frows, 4);
        gPut(rdram, header + 0x10, &efCount1, 4);
        std::fprintf(stderr, "[svr06ent] SEVP header %08x: + event %u (SvR06 %u, %u objs, donor %d: %u effects)%c", header, ev,
                     se.svrNo, nObj, donor, donorN, 10);
    }
    // STEP 3: casts shown = team size of the match snapshot (singles 1); tag events (kind 2) at least 2 (CAM tag preview
    // has no snapshot). Extra cast objects get scene count 0 (object indices kept: bank keys/parents use them).
    void svrApplyCastVisibility(uint8_t *rdram, uint16_t ev, const SvrEvent &se)
    {
        g_svrCastMap.clear();
        auto rc = g_svrRowCast.find(ev);
        if (rc == g_svrRowCast.end())
            return;
        int teamSize[8] = {}, maxTeam = 1;
        for (int i = 0; i < 8; ++i)
            if (g_entranceTeam[i] >= 0 && g_entranceTeam[i] < 8)
                maxTeam = std::max(maxTeam, ++teamSize[g_entranceTeam[i]]);
        const int visible = se.kind == 2u ? std::max(2, maxTeam) : maxTeam;
        int shown = 0;
        for (const SvrCastObj &o : rc->second.second)
        {
            const uint16_t ns = o.order < visible ? o.ns : uint16_t(0);
            gPut(rdram, rc->second.first + 28u * o.obj + 0x14u, &ns, 2);
            shown += ns != 0u ? 1 : 0;
            g_svrCastMap.push_back(o.obj);
        }
        std::fprintf(stderr, "[svrent] event %u casts: %zu objs, %d shown (team %d, %s)%c", ev, rc->second.second.size(), shown,
                     maxTeam, se.kind == 2u ? "tag" : "single", 10);
    }
    // SetCast 0x167280 (this, u16 obj, u16 code): InitEventSystem casts member k at obj k -> k-th SvR cast object.
    PS2Runtime::RecompiledFunction g_origSetCast = nullptr;
    void setCastSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t k = regU(ctx, 5) & 0xFFFFu;
        if (g_svrEventActive && k < g_svrCastMap.size() && g_svrCastMap[k] != k)
        {
            std::fprintf(stderr, "[svrent] SetCast %u -> obj %u (code %u)%c", k, unsigned(g_svrCastMap[k]), regU(ctx, 6) & 0xFFFFu, 10);
            SET_GPR_U32(ctx, 5, g_svrCastMap[k]);
        }
        else if (g_svrEventActive && !g_svrCastMap.empty() && k >= g_svrCastMap.size())
        {
            // more members than SvR cast objects: obj k would be a camera/prop/rope -> drop this cast
            std::fprintf(stderr, "[svrent] SetCast %u dropped (event has %zu cast objs)%c", k, g_svrCastMap.size(), 10);
            ctx->pc = regU(ctx, 31);
            return;
        }
        g_origSetCast(rdram, ctx, runtime);
    }
    void evsSetEventSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = regU(ctx, 4) & 0x1FFFFFFu;
        const uint16_t ev = static_cast<uint16_t>(regU(ctx, 5));
        auto it = g_svrEvents.find(ev);
        g_svrEventActive = it != g_svrEvents.end();
        g_svr06Active = g_svrEventActive && it->second.game == 6;
        g_svr06LightsSet = false;
        g_svrCurEv = ev;
        // STEP 2: per-event SvR06 light bank + pyro armed by the SvR06 event id (any PCEN/GMEV pack), previews and matches
        static const bool pyroOn = !(std::getenv("HCTP_SVR06_PYRO") && std::getenv("HCTP_SVR06_PYRO")[0] == '0');
        if (g_svr06Active)
        {
            g_svrGrayArmed = g_svrGrayArmed || g_svr06Gray.count(it->second.svrNo) != 0;
            auto lt = g_svr06Lights.find(it->second.svrNo);
            g_svr06LightHost = lt != g_svr06Lights.end() ? lt->second.first : std::vector<uint8_t>();
            g_svr06LightMask = lt != g_svr06Lights.end() ? lt->second.second : uint8_t(0);
            const std::string id = std::to_string(it->second.svrNo);
            if (pyroOn && hctpPyroHas(id.c_str()))
                hctpPyroArm(runtime, id.c_str(), 0u);
            else
                hctpPyroDisarm();
        }
        else if (ev >= 3000u && ev < 8000u)
            hctpPyroDisarm();
        if (ev >= 3000u && ev < 8000u)
            std::fprintf(stderr, "[svrent] SetEvent call ev %u (svr %d)%c", ev, g_svrEventActive ? 1 : 0, 10);
        if (g_svrEventActive)
        {
            const uint32_t header = gU32(rdram, self + 0xC);
            if (header != 0u && it->second.game == 6u)
                svr06EnsureRow(rdram, runtime, header, ev, it->second);
            else if (header != 0u)
                svrEnsureRow(rdram, runtime, header, ev, it->second);
            svrApplyCastVisibility(rdram, ev, it->second);
            std::fprintf(stderr, "[svrent] SetEvent %u (SvR04 %u)%c", ev, it->second.svrNo, 10);
        }
        g_origEvsSetEvent(rdram, ctx, runtime);
    }
    // Nov 9 SvR LIGHTS (Jordan: original SvR04 lights, untouched). CMotionLight::Action 0x1ff9d0 reads its light keys
    // through CEKey* 0x526e88 (or 0x526e90 when the slot's Set a3 bit 0 is set); CEKey = 8 bank pointers, Search
    // 0x168960 binary-searches each bank (T[256], n @0x100, 16 B rows @0x104, key & 0x3FFFFFFF) and takes the first hit.
    // SvR04 ENTR/EMOT bank 1 (BPE) = same layout and holds every light id SvR entrances use (5105.., 4012..). While an
    // SvR event plays the SvR bank goes FIRST in both CEKeys (others shifted down), restored at End/Release.
    // HCTP_SVRLIGHTS=0 -> old behaviour (SvR light ops skipped, HCTP lighting kept).
    uint32_t g_svrEmotBank = 0;                // guest copy of the decoded bank, 0 = not loaded
    int g_svrEmotState = 0;                    // 0 untried, 1 ok, -1 failed
    uint32_t g_svrLightKey[2] = {}, g_svrLightSaved[2][8] = {};
    bool g_svrLightSwapped = false;
    bool svrLightsEnabled()
    {
        static const bool on = !(std::getenv("HCTP_SVRLIGHTS") && std::strcmp(std::getenv("HCTP_SVRLIGHTS"), "0") == 0);
        return on;
    }
    // "BPE " entry -> bytes (Mod Maker svrfmt.bpe_decode, byte-pair expansion, per-block pair tables)
    bool svrBpeDecode(const uint8_t *d, size_t n, std::vector<uint8_t> &out)
    {
        if (n < 16 || std::memcmp(d, "BPE ", 4) != 0)
            return false;
        uint32_t packed, unpacked;
        std::memcpy(&packed, d + 8, 4);
        std::memcpy(&unpacked, d + 12, 4);
        size_t pos = 16;
        const size_t end = 16u + packed;
        if (end > n)
            return false;
        out.clear();
        out.reserve(unpacked);
        while (pos < end)
        {
            uint8_t left[256], right[256] = {};
            for (int i = 0; i < 256; ++i)
                left[i] = static_cast<uint8_t>(i);
            int c = 0, count = d[pos++];
            for (;;)
            {
                if (count > 127)
                    c += count - 127, count = 0;
                if (c >= 256)
                    break;
                for (int k = 0; k <= count && c < 256; ++k, ++c)
                {
                    if (pos >= end)
                        return false;
                    left[c] = d[pos++];
                    if (c != left[c])
                        right[c] = d[pos++];
                }
                if (c >= 256 || pos >= end)
                    break;
                count = d[pos++];
            }
            if (pos + 2 > end)
                return false;
            const size_t size = d[pos] | (d[pos + 1] << 8);
            pos += 2;
            if (pos + size > end)
                return false;
            uint8_t stack[512];
            for (size_t i = 0; i < size; ++i)
            {
                int sp = 0;
                stack[sp++] = d[pos + i];
                while (sp > 0)
                {
                    const uint8_t s = stack[--sp];
                    if (left[s] == s)
                        out.push_back(s);
                    else if (sp + 2 <= 512)
                        stack[sp++] = right[s], stack[sp++] = left[s];
                    else
                        return false;
                }
            }
            pos += size;
        }
        return out.size() == unpacked;
    }
    bool svrEmotLoad(PS2Runtime *runtime)
    {
        if (g_svrEmotState != 0)
            return g_svrEmotState > 0;
        g_svrEmotState = -1;
        if (g_svrEmotSz == 0u)
        {
            std::fprintf(stderr, "[svrlight] SvR04 evtEnt.pac has no ENTR/EMOT%c", 10);
            return false;
        }
        std::ifstream f("MODS/SVR04/evtEnt.pac", std::ios::binary);
        std::vector<uint8_t> pac(g_svrEmotSz * 0x100u);
        f.seekg(static_cast<std::streamoff>(g_svrEmotSec) * 0x800);
        f.read(reinterpret_cast<char *>(pac.data()), static_cast<std::streamsize>(pac.size()));
        uint32_t n = 0;
        if (!f || std::memcmp(pac.data(), "PAC ", 4) != 0 || (std::memcpy(&n, &pac[4], 4), n < 2u) || 16u + 8u * n > pac.size())
        {
            std::fprintf(stderr, "[svrlight] ENTR/EMOT is not a PAC%c", 10);
            return false;
        }
        const uint32_t base = 8u + 8u * n, e = 8u + 8u * 1u; // entry 1 = the entrance light bank
        const uint32_t off = pac[e + 2] | (pac[e + 3] << 8) | (pac[e + 4] << 16), sz = pac[e + 5] | (pac[e + 6] << 8) | (pac[e + 7] << 16);
        std::vector<uint8_t> bank;
        if (base + off + sz > pac.size() || !svrBpeDecode(&pac[base + off], sz, bank) || bank.size() < 0x104u)
        {
            std::fprintf(stderr, "[svrlight] EMOT bank 1 BPE decode failed%c", 10);
            return false;
        }
        // Nov 9 test: guestMalloc of the whole bank (824 KB) failed -> keep only entrance light ids 4000-5999
        // (280 motions, ~30 KB): T[256] + n + rows (offsets rebased) + their data. Rows stay sorted by key.
        {
            uint32_t n0 = 0;
            std::memcpy(&n0, &bank[0x100], 4);
            const size_t data0 = 0x104u + 16u * size_t(n0);
            if (data0 > bank.size())
                return false;
            std::vector<uint8_t> rows, data;
            uint32_t kept = 0;
            for (uint32_t k = 0; k < n0; ++k)
            {
                uint32_t r[4], nextOff = static_cast<uint32_t>(bank.size() - data0);
                std::memcpy(r, &bank[0x104u + 16u * k], 16);
                for (uint32_t j = k + 1; j < n0; ++j)
                {
                    uint32_t o;
                    std::memcpy(&o, &bank[0x104u + 16u * j + 4u], 4);
                    if (o != 0xFFFFFu) { nextOff = o; break; }
                }
                const uint32_t id = (r[0] >> 16) & 0x3FFFu;
                if (id < 4000u || id >= 6000u)
                    continue;
                if (r[1] != 0xFFFFFu)
                {
                    if (nextOff < r[1] || data0 + nextOff > bank.size())
                        continue;
                    const uint32_t newOff = static_cast<uint32_t>(data.size());
                    data.insert(data.end(), bank.begin() + data0 + r[1], bank.begin() + data0 + nextOff);
                    r[1] = newOff;
                }
                rows.insert(rows.end(), reinterpret_cast<uint8_t *>(r), reinterpret_cast<uint8_t *>(r) + 16);
                ++kept;
            }
            std::vector<uint8_t> small(bank.begin(), bank.begin() + 0x100);
            small.insert(small.end(), reinterpret_cast<uint8_t *>(&kept), reinterpret_cast<uint8_t *>(&kept) + 4);
            small.insert(small.end(), rows.begin(), rows.end());
            small.insert(small.end(), data.begin(), data.end());
            bank.swap(small);
        }
        const uint32_t g = runtime->guestMalloc(static_cast<uint32_t>(bank.size()), 64u);
        if (g == 0u)
        {
            std::fprintf(stderr, "[svrlight] guestMalloc %zu failed%c", bank.size(), 10);
            return false;
        }
        std::memcpy(runtime->memory().getRDRAM() + (g & 0x1FFFFFFu), bank.data(), bank.size());
        g_svrEmotBank = g & 0x1FFFFFFu;
        uint32_t cnt = 0;
        std::memcpy(&cnt, &bank[0x100], 4);
        std::fprintf(stderr, "[svrlight] SvR04 EMOT bank 1: %zu B, %u light motions at %08x%c", bank.size(), cnt, g_svrEmotBank, 10);
        g_svrEmotState = 1;
        return true;
    }
    void svrLightSwapIn(uint8_t *rdram, PS2Runtime *runtime)
    {
        if (g_svrLightSwapped || !svrLightsEnabled() || !svrEmotLoad(runtime))
            return;
        for (int k = 0; k < 2; ++k)
        {
            const uint32_t key = gU32(rdram, 0x526e88u + 8u * k) & 0x1FFFFFFu;
            g_svrLightKey[k] = (key != 0u && (k == 0 || key != g_svrLightKey[0])) ? key : 0u;
            if (g_svrLightKey[k] == 0u)
                continue;
            for (uint32_t i = 0; i < 8; ++i)
                g_svrLightSaved[k][i] = gU32(rdram, key + 4u * i);
            gPut(rdram, key, &g_svrEmotBank, 4);
            for (uint32_t i = 0; i < 7; ++i)
                gPut(rdram, key + 4u * (i + 1u), &g_svrLightSaved[k][i], 4);
            std::fprintf(stderr, "[svrlight] CEKey %d %08x: SvR bank first (old %08x %08x %08x, slot7 %08x dropped)%c", k, key,
                         g_svrLightSaved[k][0], g_svrLightSaved[k][1], g_svrLightSaved[k][2], g_svrLightSaved[k][7], 10);
        }
        g_svrLightSwapped = true;
    }
    void svrLightRestore(uint8_t *rdram)
    {
        if (!g_svrLightSwapped)
            return;
        for (int k = 0; k < 2; ++k)
            if (g_svrLightKey[k] != 0u && (gU32(rdram, g_svrLightKey[k] + 0u) == g_svrEmotBank ||
                                           (g_svr06LightBank != 0u && gU32(rdram, g_svrLightKey[k] + 0u) == g_svr06LightBank)))
                for (uint32_t i = 0; i < 8; ++i)
                    gPut(rdram, g_svrLightKey[k] + 4u * i, &g_svrLightSaved[k][i], 4);
        g_svrLightSwapped = false;
        std::fprintf(stderr, "[svrlight] HCTP light banks restored%c", 10);
    }
    // STEP 5: palette back to normal after a grayscale SvR06 entrance (CColorFilterMangaer::GetInstance 0x196270,
    // ChangeColorPallte 0x1962e0 (inst, 0)); only when the hooked original really returned (ctx->pc == ra).
    void svrGrayRestore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ra)
    {
        if (!g_svrGrayArmed || ctx->pc != ra)
            return;
        g_svrGrayArmed = false;
        PS2Runtime::RecompiledFunction inst = runtime->lookupFunction(0x196270u), change = runtime->lookupFunction(0x1962e0u);
        if (inst == nullptr || change == nullptr)
            return;
        const __m128i a0 = ctx->r[4], a1 = ctx->r[5], v0 = ctx->r[2], v1 = ctx->r[3], r31 = ctx->r[31];
        SET_GPR_U32(ctx, 31, ra);
        inst(rdram, ctx, runtime);
        const uint32_t mgr = regU(ctx, 2);
        if (mgr != 0u)
        {
            SET_GPR_U32(ctx, 4, mgr);
            SET_GPR_U32(ctx, 5, 0u);
            SET_GPR_U32(ctx, 31, ra);
            change(rdram, ctx, runtime);
        }
        ctx->r[4] = a0, ctx->r[5] = a1, ctx->r[2] = v0, ctx->r[3] = v1, ctx->r[31] = r31;
        ctx->pc = ra;
        std::fprintf(stderr, "[svr06ent] grayscale palette restored (filter manager %08x)%c", mgr, 10);
    }
    void evsEndSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_svrEventActive = false;
        g_svr06Active = false;
        svrLightRestore(rdram);
        const uint32_t ra = regU(ctx, 31);
        g_origEvsEnd(rdram, ctx, runtime);
        svrGrayRestore(rdram, ctx, runtime, ra);
    }
    void evsReleaseSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_svrEventActive = false;
        g_svr06Active = false;
        svrLightRestore(rdram);
        const uint32_t ra = regU(ctx, 31);
        g_origEvsRelease(rdram, ctx, runtime);
        svrGrayRestore(rdram, ctx, runtime, ra);
    }
    // ---- Nov 10 RAMP JITTER fix (camera 30 Hz vs body 60 Hz, [camroot] TEST 11). Code read (hctp_dis/svr06_dis):
    // HCTP CEventMotionCamera::FrameActive 0x1646f0 runs every tick but SetNextFrame 0x164b60 lets it read a key only on
    // even ticks (frame = tick>>1); AnalyzeAttribute 0x164b90 SNAPS the position (this+0xA0) and gives the rotation a
    // CLerpXYX (this+0x80; ticks byte +0x60, step +0x61, frac = step/ticks) of only 1 tick for count-0 keys
    // ((c+1)*2 ticks for c>0) -> with SvR06's per-frame count-0 keys the camera holds still every 2nd tick while the body
    // (DataAnalysis blend (c+1)*2 = 2 ticks) moves every tick. SvR06 MotionCameraBase::SetKey(int) 0x22d310 instead keeps
    // a float frame (+0x40, AddMotFrame) and reads the current + next key (AnalysisMotion/AnalysisMotionNext) = per-tick
    // interpolation. Port: count-0 SvR06 keys lerp rotation (+0x60 = 2) and position over 2 ticks, like the body
    // (tick 2f = half way, 2f+1 = key f); first key + cuts (jump > max(1.5, 6x last step) world units) still snap.
    // HCTP_SVR_CAMLERP=0 off, =all also for SvR04 events (default SvR06 only: Bret SvR04 entrance is Jordan-approved).
    struct SvrCamLerp
    {
        float from[3], to[3], step;
        int n, k;
    };
    std::map<uint32_t, SvrCamLerp> g_svrCamLerp;
    PS2Runtime::RecompiledFunction g_origCamFrameActive = nullptr;
    int svrCamLerpMode()
    {
        static const int m = [] {
            const char *e = std::getenv("HCTP_SVR_CAMLERP");
            if (e && e[0] == '0')
                return 0;
            if (e && (e[0] == 'a' || e[0] == 'A'))
                return 2;
            return 1;
        }();
        return m;
    }
    bool svrCamLerpOn()
    {
        const int m = svrCamLerpMode();
        return g_svrEventActive && (m == 2 || (m == 1 && g_svr06Active));
    }
    void camFrameActiveSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (svrCamLerpOn())
        {
            const uint32_t self = regU(ctx, 4) & 0x1FFFFFFu;
            auto it = g_svrCamLerp.find(self);
            if (it != g_svrCamLerp.end() && it->second.k < it->second.n)
            {
                SvrCamLerp &l = it->second;
                ++l.k;
                const float t = static_cast<float>(l.k) / static_cast<float>(l.n);
                for (int i = 0; i < 3; ++i)
                {
                    const float w = l.from[i] + (l.to[i] - l.from[i]) * t;
                    std::memcpy(rdram + self + 0xA0u + 4u * i, &w, 4);
                }
            }
        }
        g_origCamFrameActive(rdram, ctx, runtime);
    }
    // CEventMotionCamera::AnalyzeAttribute(this, rec, opSizeTable, len): rec = [0x80|n attr bytes][key]. SvR keys are
    // 19 B [count][f32 x y z][s16 rot x3] (f32 in the same 1/100 units as HCTP's s16) -> hand HCTP a 13 B copy and set
    // the exact float position afterwards (this+0xA0 = pos * 0.01, 0x45d4f0).
    void camAnalyzeSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = regU(ctx, 4) & 0x1FFFFFFu, rec = regU(ctx, 5) & 0x1FFFFFFu;
        const int32_t len = static_cast<int32_t>(regU(ctx, 7));
        if (!g_svrEventActive || rec == 0u || rdram[self + 0x71u] != 0u || len <= 0 || len > 0x7F)
            return g_origCamAnalyze(rdram, ctx, runtime);
        const uint32_t attr = (rdram[rec] & 0x80u) ? (rdram[rec] & 0x7Fu) + 1u : 0u;
        // SvR06 (Nov 9): 29 B [count][f32 pos x3][f32 rot x3, radians][f32 fov] (Mod Maker conv_cam_pose inverse:
        // rot = s16 * 2pi/65536); fov dropped (HCTP camera fov kept).
        const bool k29 = static_cast<uint32_t>(len) == attr + 29u;
        if (static_cast<uint32_t>(len) != attr + 19u && !k29)
            return g_origCamAnalyze(rdram, ctx, runtime);
        if (g_svrCamScratch == 0u)
            g_svrCamScratch = runtime->guestMalloc(0x100u);
        float pos[3];
        std::memcpy(pos, rdram + rec + attr + 1u, 12);
        uint8_t key[13];
        key[0] = rdram[rec + attr];
        for (int i = 0; i < 3; ++i)
        {
            const float r = std::round(pos[i]);
            const int16_t s = static_cast<int16_t>(r > 32767.f ? 32767.f : (r < -32768.f ? -32768.f : r));
            std::memcpy(key + 1 + 2 * i, &s, 2);
        }
        if (k29)
            for (int i = 0; i < 3; ++i)
            {
                float a;
                std::memcpy(&a, rdram + rec + attr + 13u + 4u * i, 4);
                const int32_t u = static_cast<int32_t>(std::lround(a * (65536.0 / 6.283185307179586)));
                const int16_t s = static_cast<int16_t>(u & 0xFFFF);
                std::memcpy(key + 7 + 2 * i, &s, 2);
            }
        else
            std::memcpy(key + 7, rdram + rec + attr + 13u, 6);
        std::memcpy(rdram + g_svrCamScratch, rdram + rec, attr);
        std::memcpy(rdram + g_svrCamScratch + attr, key, 13);
        static int logged = 0;
        if (logged++ < 6)
            std::fprintf(stderr, "[svrent] cam key f32 %.1f %.1f %.1f (attr %u)%c", pos[0], pos[1], pos[2], attr, 10);
        SET_GPR_U32(ctx, 5, g_svrCamScratch);
        SET_GPR_U32(ctx, 7, attr + 13u);
        float cur[3];
        std::memcpy(cur, rdram + self + 0xA0u, 12);
        g_origCamAnalyze(rdram, ctx, runtime);
        const float scale = 0.01f;
        float to[3];
        for (int i = 0; i < 3; ++i)
            to[i] = pos[i] * scale;
        // RAMP JITTER fix (see camFrameActiveSvr): 2-tick lerp for count-0 keys after the first key, cuts snap.
        bool lerped = false;
        if (svrCamLerpOn() && key[0] == 0u)
        {
            uint32_t tick = 0;
            std::memcpy(&tick, rdram + self + 0x1C0u, 4);
            const float dx = to[0] - cur[0], dy = to[1] - cur[1], dz = to[2] - cur[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            SvrCamLerp &l = g_svrCamLerp[self];
            const bool first = tick <= 1u;
            const float lim = std::max(1.5f, 6.f * l.step);
            const bool cut = !first && d > lim;
            if (!first && !cut && std::isfinite(d))
            {
                std::memcpy(l.from, cur, 12);
                std::memcpy(l.to, to, 12);
                l.n = 2;
                l.k = 1;
                l.step = d;
                if (rdram[self + 0x80u + 0x60u] == 1u && rdram[self + 0x80u + 0x61u] == 0u)
                    rdram[self + 0x80u + 0x60u] = 2u;
                for (int i = 0; i < 3; ++i)
                {
                    const float w = cur[i] + (to[i] - cur[i]) * 0.5f;
                    std::memcpy(rdram + self + 0xA0u + 4u * i, &w, 4);
                }
                lerped = true;
            }
            else
            {
                std::memcpy(l.from, to, 12);
                std::memcpy(l.to, to, 12);
                l.n = l.k = 0;
                l.step = first ? 0.f : l.step;
                static int cuts = 0;
                if (cut && cuts++ < 40)
                    std::fprintf(stderr, "[svrcamlerp] cut snap obj %08x tick %u jump %.2f (lim %.2f)%c", self, tick, d, lim, 10);
            }
            static int logged = 0;
            if (lerped && logged++ < 4)
                std::fprintf(stderr, "[svrcamlerp] 2-tick lerp obj %08x tick %u step %.3f rot ticks %u%c", self, tick, d,
                             rdram[self + 0x80u + 0x60u], 10);
        }
        if (!lerped)
            for (int i = 0; i < 3; ++i)
                std::memcpy(rdram + self + 0xA0u + 4u * i, &to[i], 4);
    }
    // CMotionLight::Set(this, slot, motion, a3) from CAttribute::SetAttr op 0x11: SvR light motion ids are not HCTP
    // EVT.PAC light motions -> keep HCTP's lighting while an SvR event plays.
    void motionLightSetSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = regU(ctx, 31);
        g_motionLightThis = regU(ctx, 4) & 0x1FFFFFFu;
        if (g_svrEventActive && ra >= 0x1b3120u && ra < 0x1b4e30u && g_svrLightOverride.count(g_svrCurEv))
        {
            ctx->pc = ra; // Nov 10 Mr. America: light override owns the lights; the bank's own light ops are dropped
            return;
        }
        if (g_svrEventActive && ra >= 0x1b3120u && ra < 0x1b4e30u && svrLightsEnabled())
        {
            svrLightSwapIn(rdram, runtime);
            if (g_svrLightSwapped)
            {
                static int logged = 0;
                if (logged++ < 64)
                    std::fprintf(stderr, "[svrlight] SvR light op slot %u motion %u a3 %u%c", regU(ctx, 5), regU(ctx, 6),
                                 regU(ctx, 7), 10);
                return g_origMotionLightSet(rdram, ctx, runtime);
            }
        }
        if (g_svrEventActive && ra >= 0x1b3120u && ra < 0x1b4e30u)
        {
            static int logged = 0;
            if (logged++ < 64)
                std::fprintf(stderr, "[svrent] SvR light op slot %u motion %u -> HCTP lighting kept%c", regU(ctx, 5),
                             regU(ctx, 6), 10);
            ctx->pc = ra;
            return;
        }
        g_origMotionLightSet(rdram, ctx, runtime);
    }
    // ---- Nov 10 SvR06 build 10: per-frame driver on CEventSystem::Action 0x166e00 (a0 = CEventSystem; scene u16 +0x18,
    // frame u16 +0x1A, frame max +0x1E). (1) hands the event clock to the host pyro (hctp_svr06_pyro.cpp), (2) plays the
    // pyro SE when the pyro window opens: HCTP SE 0xCF = stock Brock 3044 pyro (bank op C0 CF at scene 2 frame 230;
    // SetAttr 0x1b46dc -> PlaySE_FromAttribute(obj, id, 0)); HCTP_SVR06_PYROSE=<hex id>, 0 = off. SvR06's own op is
    // C1 <u16 id> (4044: 0x232F scene 0 f0, 0x2333 scene 15 f160) - bank not ported yet. (3) SvR06 lights: the bank's
    // 8 light tracks -> CMotionLight::Set(slot, 0x3F00+slot) with the host light bank first in both CEKeys
    // (restored at End/Release by svrLightRestore). HCTP_SVR06_LIGHTS=0 = off.
    PS2Runtime::RecompiledFunction g_origEvsActionSvr = nullptr;
    void svr06LightsStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_svr06LightsSet)
            return;
        g_svr06LightsSet = true;
        // Nov 10 Mr. America (Jordan): same SvR06 entrance, lights replaced by one HCTP light motion (3052 = the unused
        // red/white/blue motion, notes\LIGHTING_LIST_20261026.md) on the colour slots 0-2 (wrestler) + 4-6 (arena);
        // ambients 3/7 keep the current lighting. HCTP's own light banks stay in the CEKeys (no SvR06 bank swap).
        if (auto lo = g_svrLightOverride.find(g_svrCurEv); lo != g_svrLightOverride.end())
        {
            if (g_motionLightThis == 0u || g_origMotionLightSet == nullptr)
            {
                std::fprintf(stderr, "[svr06light] override %u skipped (CMotionLight %08x)%c", lo->second, g_motionLightThis, 10);
                return;
            }
            const __m128i a0 = ctx->r[4], a1 = ctx->r[5], a2 = ctx->r[6], a3 = ctx->r[7];
            const uint32_t pc = ctx->pc;
            for (uint32_t slot : {0u, 1u, 2u, 4u, 5u, 6u})
            {
                SET_GPR_U32(ctx, 4, g_motionLightThis);
                SET_GPR_U32(ctx, 5, slot);
                SET_GPR_U32(ctx, 6, lo->second);
                SET_GPR_U32(ctx, 7, 0u);
                g_origMotionLightSet(rdram, ctx, runtime); // leaf (no calls)
            }
            ctx->r[4] = a0, ctx->r[5] = a1, ctx->r[6] = a2, ctx->r[7] = a3;
            ctx->pc = pc;
            std::fprintf(stderr, "[svr06light] event %u: HCTP light %u on slots 0-2,4-6 (SvR06 lights replaced)%c", g_svrCurEv,
                         lo->second, 10);
            return;
        }
        static const bool on = !(std::getenv("HCTP_SVR06_LIGHTS") && std::getenv("HCTP_SVR06_LIGHTS")[0] == '0');
        if (!on || g_svr06LightHost.empty() || g_motionLightThis == 0u || g_origMotionLightSet == nullptr || g_svrLightSwapped)
        {
            std::fprintf(stderr, "[svr06light] skipped (on %d, bank %zu B, CMotionLight %08x, swapped %d)%c", on ? 1 : 0,
                         g_svr06LightHost.size(), g_motionLightThis, g_svrLightSwapped ? 1 : 0, 10);
            return;
        }
        if (g_svr06LightBank == 0u)
        {
            const uint32_t g = runtime->guestMalloc(static_cast<uint32_t>(std::max(g_svr06LightMax, g_svr06LightHost.size())), 64u);
            if (g == 0u)
            {
                std::fprintf(stderr, "[svr06light] guestMalloc failed%c", 10);
                return;
            }
            g_svr06LightBank = g & 0x1FFFFFFu;
        }
        std::memcpy(rdram + g_svr06LightBank, g_svr06LightHost.data(), g_svr06LightHost.size());
        for (int k = 0; k < 2; ++k)
        {
            const uint32_t key = gU32(rdram, 0x526e88u + 8u * k) & 0x1FFFFFFu;
            g_svrLightKey[k] = (key != 0u && (k == 0 || key != g_svrLightKey[0])) ? key : 0u;
            if (g_svrLightKey[k] == 0u)
                continue;
            for (uint32_t i = 0; i < 8; ++i)
                g_svrLightSaved[k][i] = gU32(rdram, key + 4u * i);
            gPut(rdram, key, &g_svr06LightBank, 4);
            for (uint32_t i = 0; i < 7; ++i)
                gPut(rdram, key + 4u * (i + 1u), &g_svrLightSaved[k][i], 4);
        }
        g_svrLightSwapped = true;
        const __m128i a0 = ctx->r[4], a1 = ctx->r[5], a2 = ctx->r[6], a3 = ctx->r[7];
        const uint32_t pc = ctx->pc;
        int set = 0;
        for (uint32_t slot = 0; slot < 8; ++slot)
        {
            if (!(g_svr06LightMask & (1u << slot)))
                continue;
            SET_GPR_U32(ctx, 4, g_motionLightThis);
            SET_GPR_U32(ctx, 5, slot);
            SET_GPR_U32(ctx, 6, 0x3F00u + slot);
            SET_GPR_U32(ctx, 7, 0u);
            g_origMotionLightSet(rdram, ctx, runtime); // leaf (no calls)
            ++set;
        }
        ctx->r[4] = a0, ctx->r[5] = a1, ctx->r[6] = a2, ctx->r[7] = a3;
        ctx->pc = pc;
        std::fprintf(stderr, "[svr06light] %d SvR06 light slots set on CMotionLight %08x (bank %08x)%c", set, g_motionLightThis,
                     g_svr06LightBank, 10);
    }
    void evsActionSvr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sys = regU(ctx, 4) & 0x1FFFFFFu, ra = regU(ctx, 31);
        g_origEvsActionSvr(rdram, ctx, runtime);
        if (!g_svr06Active || sys == 0u || sys >= 0x1FFFF00u || ctx->pc != ra)
            return;
        if (rdram[sys + 0x54u] == 0u || gU32(rdram, sys + 0x10u) != 2u)
            return;
        uint16_t sc, fr, mx;
        std::memcpy(&sc, rdram + sys + 0x18u, 2);
        std::memcpy(&fr, rdram + sys + 0x1Au, 2);
        std::memcpy(&mx, rdram + sys + 0x1Eu, 2);
        svr06LightsStart(rdram, ctx, runtime);
        if (!hctpPyroEventTick(sc, fr, mx))
            return;
        static const uint32_t seId = [] { const char *v = std::getenv("HCTP_SVR06_PYROSE"); return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0xCFu; }();
        PS2Runtime::RecompiledFunction playSe = seId != 0u ? runtime->lookupFunction(0x3ada80u) : nullptr;
        std::fprintf(stderr, "[pyro] build10 pyro window at scene %u frame %u -> HCTP SE 0x%02X%s%c", sc, fr, seId, playSe ? "" : " (off)", 10);
        if (playSe == nullptr)
            return;
        const __m128i a0 = ctx->r[4], a1 = ctx->r[5], a2 = ctx->r[6], v0 = ctx->r[2], v1 = ctx->r[3];
        SET_GPR_U32(ctx, 4, 0u);
        SET_GPR_U32(ctx, 5, seId);
        SET_GPR_U32(ctx, 6, 0u);
        playSe(rdram, ctx, runtime); // PlaySE_FromAttribute(obj 0 = not positional, id, 0)
        ctx->r[4] = a0, ctx->r[5] = a1, ctx->r[6] = a2, ctx->r[2] = v0, ctx->r[3] = v1;
        ctx->pc = ra;
    }
    // Nov 9 log-only probe (HCTP_PROP_PROBE=1): live state of SvR event props (model id >= 2800) after
    // CSD3MotionWeapon::ActionLink 0x1c2790. attr = this+0x24: +0x18 attach code, +0x30 model, +0x34 obj, +0x3c parent.
    PS2Runtime::RecompiledFunction g_origWeaponActionLink = nullptr;
    void weaponActionLinkProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = regU(ctx, 4) & 0x1FFFFFFu;
        g_origWeaponActionLink(rdram, ctx, runtime);
        if (!g_svrEventActive)
            return;
        const uint32_t attr = gU32(rdram, self + 0x24u) & 0x1FFFFFFu;
        if (attr == 0u || gU32(rdram, attr + 0x30u) < 2800u || gU32(rdram, attr + 0x30u) >= 3000u)
            return;
        static uint32_t last = 0xFFFFFFFFu;
        static int count = 0, logged = 0;
        const uint32_t code = gU32(rdram, attr + 0x18u), obj = gU32(rdram, attr + 0x34u) & 0x1FFFFFFu;
        const bool changed = code != last;
        last = code;
        if ((!changed && (++count % 60) != 0) || logged >= 200)
            return;
        ++logged;
        float m[3] = {};
        if (obj != 0u)
            std::memcpy(m, rdram + obj + 0x40u, 12);
        std::fprintf(stderr, "[propprobe] this %08x model %u attach %d parent %08x obj %08x pos %.1f %.1f %.1f flag188 %02x off %.2f %.2f %.2f%c",
                     self, gU32(rdram, attr + 0x30u), static_cast<int>(code), gU32(rdram, attr + 0x3Cu), obj, m[0], m[1], m[2],
                     rdram[self + 0x188u], *reinterpret_cast<float *>(rdram + attr + 0x20u),
                     *reinterpret_cast<float *>(rdram + attr + 0x24u), *reinterpret_cast<float *>(rdram + attr + 0x28u), 10);
    }
    // Nov 9 GLASSES: prop attach code 4 asks CSkinModel::FindBone(model, "atama_d") (ActionLink 0x1c2e40, string
    // 0x464ce8). SvR04 entrance bodies (Bret 7704) have "atama" but no "atama_d" -> -1 -> glasses at 0,0,0. Native
    // copy of FindBone 0x150b10 (leaf: name = first 16 B of each 80 B entry, table *(model+0xC8): +0x14 n, +0x20
    // entries); when "<x>_d" is missing, retry "<x>". Found names behave exactly as stock.
    int32_t findBoneNative(uint8_t *rdram, uint32_t model, const char *name16)
    {
        const uint32_t ymp = gU32(rdram, model + 0xC8u) & 0x1FFFFFFu;
        if (ymp == 0u)
            return -1;
        const uint32_t n = gU32(rdram, ymp + 0x14u), tab = gU32(rdram, ymp + 0x20u) & 0x1FFFFFFu;
        for (uint32_t i = 0; i < n && tab + 80u * (i + 1u) <= 0x2000000u; ++i)
            if (std::memcmp(rdram + tab + 80u * i, name16, 16) == 0)
                return static_cast<int32_t>(i);
        return -1;
    }
    void findBoneFallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t model = regU(ctx, 4) & 0x1FFFFFFu, str = regU(ctx, 5) & 0x1FFFFFFu;
        char name[16] = {};
        for (int i = 0; i < 16 && rdram[str + i] != 0; ++i)
            name[i] = static_cast<char>(rdram[str + i]);
        int32_t r = findBoneNative(rdram, model, name);
        const size_t len = strnlen(name, 16);
        if (r < 0 && len > 2 && name[len - 2] == '_' && name[len - 1] == 'd')
        {
            char alt[16] = {};
            std::memcpy(alt, name, len - 2);
            r = findBoneNative(rdram, model, alt);
            static int logged = 0;
            if (r >= 0 && logged++ < 8)
                std::fprintf(stderr, "[findbone] %s missing -> %s (bone %d)%c", name, alt, r, 10);
        }
        SET_GPR_U32(ctx, 2, static_cast<uint32_t>(r));
        ctx->pc = regU(ctx, 31);
    }
    void svrEntranceInstall(PS2Runtime &runtime)
    {
        if (g_svrEvents.empty())
            return;
        if (!(std::getenv("HCTP_FINDBONE_FALLBACK") && std::getenv("HCTP_FINDBONE_FALLBACK")[0] == '0'))
            runtime.replaceFunction(0x150b10u, findBoneFallback);
        // Nov 9: probe on by default while SvR events exist (log-only, 200 lines max) for Bret's glasses; =0 off.
        if (const char *pp = std::getenv("HCTP_PROP_PROBE"); (pp == nullptr || pp[0] != '0') &&
            (g_origWeaponActionLink = runtime.lookupFunction(0x1c2790u)) != nullptr)
            runtime.replaceFunction(0x1c2790u, weaponActionLinkProbe);
        auto hookFn = [&runtime](uint32_t addr, PS2Runtime::RecompiledFunction &orig, PS2Runtime::RecompiledFunction fn) {
            if (orig != nullptr || (orig = runtime.lookupFunction(addr)) == nullptr)
                return false;
            runtime.replaceFunction(addr, fn);
            return true;
        };
        bool ok = hookFn(0x166fe0u, g_origEvsSetEvent, evsSetEventSvr);
        ok = hookFn(0x166fa0u, g_origEvsEnd, evsEndSvr) && ok;
        ok = hookFn(0x166d80u, g_origEvsRelease, evsReleaseSvr) && ok;
        ok = hookFn(0x164b90u, g_origCamAnalyze, camAnalyzeSvr) && ok;
        ok = hookFn(0x1646f0u, g_origCamFrameActive, camFrameActiveSvr) && ok;
        ok = hookFn(0x1ff8c0u, g_origMotionLightSet, motionLightSetSvr) && ok;
        ok = hookFn(0x166e00u, g_origEvsActionSvr, evsActionSvr) && ok;
        ok = hookFn(0x167280u, g_origSetCast, setCastSvr) && ok; // STEP 3: SvR cast object order
        if (g_origCFileSearch == nullptr)
            ok = hookFn(0x1482f0u, g_origCFileSearch, cfileSearchLoose) && ok;
        std::cerr << "[svrent] SvR entrance reader: " << g_svrEvents.size() / 2 << " SvR04 events" << (ok ? "" : " (a hook FAILED)")
                  << std::endl;
    }
    // MODS\ROSTER\<folder>\slot.txt: id=<hex, >= A2> template=<hex stock id> name=<select/record name>
    // ring=<ring name> entrance=<event no>; models model1.pac..model5.pac (else the first *.pac = attire 1);
    // record.bin optional (0x170, stock layout). Folders without id= get the next free id after the highest given one.
    int rosterxScan()
    {
        std::error_code ec;
        std::vector<std::filesystem::path> dirs;
        for (const auto &d : std::filesystem::directory_iterator("MODS/ROSTER", ec))
            if (d.is_directory(ec))
                dirs.push_back(d.path());
        std::sort(dirs.begin(), dirs.end());
        uint16_t nextId = kRosterxFirstId;
        std::set<uint16_t> used;
        for (const auto &dir : dirs)
        {
            RosterxSlot s;
            s.folder = dir.filename().string();
            s.name = s.folder;
            std::ifstream cfg(dir / "slot.txt");
            if (!cfg)
                continue; // MODS\ROSTER folders without slot.txt = old carrier mods (ignored)
            unsigned svrEvent = 0u;
            for (std::string line; std::getline(cfg, line);)
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                unsigned v;
                if (std::sscanf(line.c_str(), "id=%x", &v) == 1 && v >= kRosterxFirstId && v < 0x8000u)
                    s.id = static_cast<uint16_t>(v);
                else if (std::sscanf(line.c_str(), "template=%x", &v) == 1 && v < 0x56u)
                    s.tmpl = static_cast<uint8_t>(v);
                else if (std::sscanf(line.c_str(), "music=%u", &v) == 1 && v < 256u)
                    s.music = static_cast<int>(v);
                else if (std::sscanf(line.c_str(), "video=%u", &v) == 1 && v < 256u)
                    s.video = static_cast<int>(v);
                else if (std::sscanf(line.c_str(), "portrait=%x", &v) == 1 && v < 0x56u)
                    s.portrait = static_cast<uint8_t>(v);
                else if (std::sscanf(line.c_str(), "entrance=svr04:%u", &v) == 1)
                    svrEvent = v; // original SvR04 event, read from MODS\SVR04\evtEnt.pac
                else if (std::sscanf(line.c_str(), "entrance=%u", &v) == 1)
                    s.entrance = static_cast<int16_t>(v);
                else if (line.rfind("name=", 0) == 0)
                    s.name = line.substr(5);
                else if (line.rfind("ring=", 0) == 0)
                    s.ring = line.substr(5);
            }
            if (svrEvent != 0u)
                if (const int ev = svrEntranceRegister(svrEvent, s.tmpl); ev > 0)
                    s.entrance = static_cast<int16_t>(ev);
            for (int a = 1; a <= 5; ++a)
                if (auto p = dir / ("model" + std::to_string(a) + ".pac"); std::filesystem::exists(p, ec))
                    s.pacs.push_back(p.string());
                else
                    break;
            if (s.pacs.empty())
                for (const auto &f : std::filesystem::directory_iterator(dir, ec))
                {
                    std::string ext = f.path().extension().string();
                    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ext == ".pac")
                    {
                        s.pacs.push_back(f.path().string());
                        break;
                    }
                }
            if (s.pacs.empty())
            {
                std::fprintf(stderr, "[rosterx] %s: no model pac, skipped%c", s.folder.c_str(), 10);
                continue;
            }
            if (std::ifstream rb(dir / "record.bin", std::ios::binary); rb)
            {
                s.record.resize(0x170);
                if (!rb.read(reinterpret_cast<char *>(s.record.data()), 0x170))
                    s.record.clear();
            }
            s.attires = static_cast<uint8_t>(std::min<size_t>(s.pacs.size(), 5));
            if (s.id != 0 && used.count(s.id))
            {
                std::fprintf(stderr, "[rosterx] %s: id 0x%X already used, reassigned%c", s.folder.c_str(), s.id, 10);
                s.id = 0;
            }
            if (s.id != 0)
                used.insert(s.id);
            g_rosterx.push_back(std::move(s));
        }
        for (auto &s : g_rosterx)
            if (s.id == 0)
            {
                while (used.count(nextId))
                    ++nextId;
                s.id = nextId;
                used.insert(nextId);
            }
        // Step 1: static model numbers 86..99 (14 slots). Step 3 replaces this with a per-match pool.
        uint8_t model = kRosterxModelFirst;
        for (auto it = g_rosterx.begin(); it != g_rosterx.end();)
        {
            if (model > kRosterxModelLast)
            {
                // Nov 10 step 3: more slots than model numbers -> unbound now, bound on demand (rosterxBind).
                it->modelNo = 0;
                std::fprintf(stderr, "[rosterx] slot %s id 0x%X template 0x%X pooled (model no bound per match)%c",
                             it->name.c_str(), it->id, it->tmpl, 10);
                ++it;
                continue;
            }
            it->modelNo = model++;
            rosterxInstallModels(*it);
            std::fprintf(stderr, "[rosterx] slot %s id 0x%X template 0x%X model %u (%u attires)%c", it->name.c_str(), it->id,
                         it->tmpl, it->modelNo, it->attires, 10);
            ++it;
        }
        return static_cast<int>(g_rosterx.size());
    }
    // Select list (expandReaderList): one 0x80 CHSL record per slot, cloned from the template's record.
    uint32_t rosterxAppendSelect(uint8_t *rdram, uint8_t *out, uint32_t n)
    {
        (void)rdram;
        for (const auto &s : g_rosterx)
        {
            const uint8_t *tmpl = nullptr;
            for (uint32_t i = 0; i < kStockSelect && !tmpl; ++i)
                if (rdS16(out + i * kSelectRec + 2) == s.tmpl)
                    tmpl = out + i * kSelectRec;
            if (!tmpl)
            {
                std::fprintf(stderr, "[rosterx] %s: template 0x%X not in the select list%c", s.name.c_str(), s.tmpl, 10);
                continue;
            }
            uint8_t *const rec = out + n * kSelectRec;
            std::memcpy(rec, tmpl, kSelectRec);
            const int16_t id = static_cast<int16_t>(s.id);
            std::memcpy(rec + 2, &id, 2);
            std::memset(rec + 4, 0, 0x20);
            std::strncpy(reinterpret_cast<char *>(rec + 4), s.name.c_str(), 0x1F);
            // +0x24..0x67 finisher display: kept from the template (Nov 5: slots now play the template's moveset).
            rec[0x72] = 0;                    // never a special referee
            for (uint32_t set : {0x68u, 0x74u})
            {
                const int16_t first = rdS16(tmpl + set);
                for (uint32_t a = 0; a < 5; ++a)
                {
                    int16_t v = -1;
                    if (a < s.attires && first != -1)
                        v = rdS16(tmpl + set + a * 2) != -1 ? rdS16(tmpl + set + a * 2) : first;
                    std::memcpy(rec + set + a * 2, &v, 2);
                }
            }
            ++n;
        }
        return n;
    }
    PS2Runtime::RecompiledFunction g_rxOrigGetCharaGameProp = nullptr, g_rxOrigName = nullptr, g_rxOrigRing = nullptr,
                                   g_rxOrigSdEntry = nullptr, g_rxOrigExist = nullptr, g_rxOrigSeason = nullptr,
                                   g_rxOrigMatchNo = nullptr;
    uint32_t rxArg(R5900Context *ctx, int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); }
    void rosterxGetCharaGameProp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t id = rxArg(ctx, 4), ra = rxArg(ctx, 31);
        RosterxSlot *s = rosterxById(id);
        if (!s && (s = rosterxFromCodeId(rdram, id)) != nullptr)
        {
            static std::set<uint32_t> seen; // measurement: code/100 sites that reached the slot
            if (seen.size() < 64 && seen.insert(ra).second)
                std::fprintf(stderr, "[rosterx] GetCharaGameProp(0x%X) ra %08x -> %s%c", id, ra, s->name.c_str(), 10);
        }
        if (s)
        {
            if (const uint32_t rec = rosterxRecord(rdram, runtime, *s))
            {
                SET_GPR_U32(ctx, 2, rec);
                ctx->pc = ra;
                return;
            }
        }
        g_rxOrigGetCharaGameProp(rdram, ctx, runtime);
    }
    template <uint32_t Off>
    void rosterxNameAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction orig)
    {
        RosterxSlot *s = rosterxById(rxArg(ctx, 4));
        if (!s)
            s = rosterxFromCodeId(rdram, rxArg(ctx, 4));
        if (s)
            if (const uint32_t rec = rosterxRecord(rdram, runtime, *s))
            {
                SET_GPR_U32(ctx, 2, rec + Off);
                ctx->pc = rxArg(ctx, 31);
                return;
            }
        orig(rdram, ctx, runtime);
    }
    void rosterxName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { rosterxNameAt<0x116>(rdram, ctx, rt, g_rxOrigName); }
    void rosterxRing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { rosterxNameAt<0x12C>(rdram, ctx, rt, g_rxOrigRing); }
    void rosterxSdEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (RosterxSlot *s = rosterxById(rxArg(ctx, 4)))
            if (const uint32_t rec = rosterxRecord(rdram, runtime, *s))
            {
                SET_GPR_U32(ctx, 2, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(rdram[rec + 0x163])) ^ 6));
                ctx->pc = rxArg(ctx, 31);
                return;
            }
        g_rxOrigSdEntry(rdram, ctx, runtime);
    }
    template <uint32_t Value>
    void rosterxConst(R5900Context *ctx, uint8_t *rdram, PS2Runtime *runtime, PS2Runtime::RecompiledFunction orig)
    {
        if (rosterxById(rxArg(ctx, 4)))
        {
            SET_GPR_U32(ctx, 2, Value);
            ctx->pc = rxArg(ctx, 31);
            return;
        }
        orig(rdram, ctx, runtime);
    }
    void rosterxExist(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { rosterxConst<1>(ctx, rdram, rt, g_rxOrigExist); }
    // Season: new slots stay out until Season data exists for them (plan step 6).
    void rosterxSeason(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { rosterxConst<0>(ctx, rdram, rt, g_rxOrigSeason); }
    // GetMatchWrestlerNo 0x12cce0 (a0 id, a1 attire, a2 backstage, a3 entrance, t0 special, t1 CAW): stock formula on the
    // slot's model no (special -> 90 instead of attire*10, entrance +3, backstage +1 when that loose key exists).
    // ---- Nov 10 step 3: per-match model-number pool (86..99 = 14 numbers, > 14 slots) ----
    // A slot without a number takes the least recently used number whose model is not in a live cast code.
    // The game's CFile cache (list *(0x524E18)+4, node {+4 next, +8 entry}, entry name at +0, CompareKeyword
    // 0x147350) may still hold the old slot's "/EMD2/<no>xx" data: those entries get their name spoiled so the
    // next search misses and loads the new files.
    uint64_t g_rosterxUseClock = 0;
    void rosterxSpoilCache(uint8_t *rdram, uint32_t modelNo)
    {
        char key[8];
        std::snprintf(key, sizeof(key), "%02u", modelNo);
        uint32_t head;
        std::memcpy(&head, rdram + 0x524E18u, 4);
        if (head == 0u || head >= 0x2000000u - 8u)
            return;
        uint32_t node;
        std::memcpy(&node, rdram + head + 4u, 4);
        for (int guard = 0; node != 0u && node < 0x2000000u - 0x10u && guard < 4096; ++guard)
        {
            uint32_t entry, next;
            std::memcpy(&entry, rdram + node + 8u, 4);
            std::memcpy(&next, rdram + node + 4u, 4);
            if (entry != 0u && entry < 0x2000000u - 0x40u)
            {
                char *n = reinterpret_cast<char *>(rdram + entry);
                const std::string name(n, strnlen(n, 0x3F));
                std::string up = name;
                for (char &c : up)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                const size_t at = up.find("EMD2");
                if (at != std::string::npos && up.size() >= at + 9u && up.compare(at + 5u, 2, key) == 0)
                {
                    n[0] = '~';
                    std::fprintf(stderr, "[rosterx] pool: cache entry %s dropped%c", name.c_str(), 10);
                }
            }
            node = next;
        }
    }
    void rosterxBind(uint8_t *rdram, RosterxSlot &s)
    {
        s.lastUse = ++g_rosterxUseClock;
        if (s.modelNo != 0u)
            return;
        RosterxSlot *victim = nullptr;
        bool used[100] = {};
        for (auto &o : g_rosterx)
            if (o.modelNo != 0u)
                used[o.modelNo] = true;
        uint8_t no = 0;
        for (uint8_t m = kRosterxModelFirst; m <= kRosterxModelLast && no == 0u; ++m)
            if (!used[m])
                no = m;
        if (no == 0u)
        {
            for (auto &o : g_rosterx)
                if (o.modelNo != 0u && !rosterxCastHasModel(rdram, o.modelNo) && (!victim || o.lastUse < victim->lastUse))
                    victim = &o;
            if (!victim)
            {
                std::fprintf(stderr, "[rosterx] pool: %s has no free model number (all 14 live)%c", s.name.c_str(), 10);
                return;
            }
            no = victim->modelNo;
            victim->modelNo = 0;
            if (victim->rec != 0u)
            {
                int16_t t;
                std::memcpy(&t, rdram + 0x4C0F54u + victim->tmpl * 0x170u + 0x114u, 2);
                std::memcpy(rdram + victim->rec + 0x114u, &t, 2);
            }
        }
        s.modelNo = no;
        if (s.rec != 0u)
        {
            const int16_t m = no;
            std::memcpy(rdram + s.rec + 0x114u, &m, 2);
        }
        rosterxSpoilCache(rdram, no);
        rosterxInstallModels(s, true);
        std::fprintf(stderr, "[rosterx] pool: %s -> model %u%s%s%c", s.name.c_str(), unsigned(no), victim ? " from " : "",
                     victim ? victim->name.c_str() : "", 10);
    }
    void rosterxMatchNo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        RosterxSlot *s = rosterxById(rxArg(ctx, 4));
        if (!s)
            return g_rxOrigMatchNo(rdram, ctx, runtime);
        rosterxBind(rdram, *s);
        if (s->modelNo == 0u)
            return g_rxOrigMatchNo(rdram, ctx, runtime);
        const uint32_t attire = rxArg(ctx, 5) < s->attires ? rxArg(ctx, 5) : 0u;
        uint32_t code = s->modelNo * 100u + (rxArg(ctx, 8) != 0u ? 90u : attire * 10u);
        if (rxArg(ctx, 7) != 0u)
            code += 3u;
        else if (rxArg(ctx, 6) != 0u && g_loosePac.count("/EMD2/" + std::to_string(code + 1u)))
            code += 1u;
        SET_GPR_U32(ctx, 2, code & 0xFFFFu);
        ctx->pc = rxArg(ctx, 31);
    }
    // Per-id stock tables: call the original with the template id in the id register.
    struct RosterxSub
    {
        uint32_t addr;
        int reg;
        const char *what;
        bool code; // register holds a cast code (model*100 + attire/part), not a roster id
        PS2Runtime::RecompiledFunction orig;
    };
    RosterxSub g_rxSubs[] = {
        // Nov 5: corset/mike/belt/IsDress take CAST CODES (andi 0xFFFF; -10000 test) - Modern Piper was oversized.
        {0x2abb50u, 4, "GetCorsetScale", true, nullptr},       {0x2ac030u, 4, "GetMikePos", true, nullptr},
        {0x2ac230u, 4, "GetSBeltPos", true, nullptr},          {0x2ab710u, 4, "GetBeltScale", true, nullptr},
        {0x2abd70u, 4, "GetHeadGearScale", true, nullptr},     {0x2af300u, 5, "IsDress", true, nullptr},
        {0x370420u, 4, "GetMovesPosture", false, nullptr},     {0x24ce00u, 5, "GetSuperStarFace64", false, nullptr},
        {0x26ff20u, 5, "GetUsedCostume", false, nullptr},      {0x24ce70u, 4, "WresParamChangeOK", false, nullptr},
        {0x12d2a0u, 4, "IsBrapanRoster", false, nullptr},      {0x12d300u, 4, "IsFirstBloodRoster", false, nullptr},
        // Nov 5: CSuperStarReader::RegistSuperStar(this, slot, a2 id): id >= 0x56 -> CAW silhouette 0x56-0x58 (the green
        // portrait). Template id -> template's face (own SPF faces = plan step 7).
        {0x24c220u, 6, "RegistSuperStar", false, nullptr},
    };
    template <size_t I>
    void rosterxSub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const RosterxSub &h = g_rxSubs[I];
        const uint32_t v = rxArg(ctx, h.reg);
        if (h.addr == 0x2af300u)
        {
            static int allLogged = 0;
            if (allLogged++ < 24)
                std::fprintf(stderr, "[rosterx] IsDress call a1 %u (ra %08x)%c", v & 0xFFFFu, rxArg(ctx, 31), 10);
        }
        if (h.code)
        {
            const uint32_t c = v & 0xFFFFu;
            if (c < 10000u)
                // Nov 9: IsDress gets the live cast code (8600 < 10000 can never be a CAW, CAWs are 10000+), so take
                // the slot by model even when CAW block 0 is in use (rosterxFromCodeId refused it -> no jacket).
                if (const RosterxSlot *s = (h.addr == 0x2af300u && rosterxByModel(c / 100u)) ? rosterxByModel(c / 100u)
                                                                                             : rosterxFromCodeId(rdram, c / 100u))
                {
                    // Nov 6 ROSTERX DRESS: IsDress (0x2af300, id >= 0x56 -> 0) = 1 when the slot ships "<attire>2.pac"
                    // (raw XXA2 entrance dress, e.g. SvR Bret jacket 7702; served by rosterxInstallModels).
                    if (h.addr == 0x2af300u && !s->pacs.empty())
                    {
                        char own[8];
                        std::snprintf(own, sizeof(own), "%u2.pac", (c % 100u) / 10u);
                        std::error_code ec;
                        const bool has = std::filesystem::exists(std::filesystem::path(s->pacs[0]).parent_path() / own, ec);
                        static int dressLogged = 0;
                        if (dressLogged++ < 16)
                            std::fprintf(stderr, "[rosterx] IsDress code %u -> slot %s dress %s %s%c", c, s->name.c_str(), own,
                                         has ? "YES" : "no", 10);
                        if (has)
                        {
                            SET_GPR_U32(ctx, 2, 1u);
                            ctx->pc = rxArg(ctx, 31);
                            return;
                        }
                    }
                    SET_GPR_U32(ctx, h.reg, rosterxTemplateCode(rdram, *s, c));
                }
        }
        else if (const RosterxSlot *s = rosterxById(v))
            SET_GPR_U32(ctx, h.reg, (h.addr == 0x24c220u && s->portrait) ? s->portrait : s->tmpl);
        h.orig(rdram, ctx, runtime);
    }
    template <size_t... I>
    void rosterxInstallSubs(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_rxSubs[I].orig = runtime.lookupFunction(g_rxSubs[I].addr),
          g_rxSubs[I].orig && runtime.replaceFunction(g_rxSubs[I].addr, rosterxSub<I>)),
         ...);
    }
    // CCharProp ctor 0x1cd1d0 (a0 prop, a1 CChar): copies the roster record (now the slot's) to prop+0x20, then the
    // media-defaults wrapper applies MEDIA_DEFAULTS by code/100 (skips codes >= 8600). Slots: apply the template's row.
    // MOVESET (Nov 5, code read): GetWazaTbl 0x1e44b0 = CChar+0x1B4 chunk 'CPRO' (0x4350524F = this CCharProp), table
    // at prop+0x30 = record+0x10 (u16 per move). The ctor copies a2 (0x170) to prop+0x20. CreateGameChar 0x1fb150 fills
    // a2 (its sp+0xD0) from 0x4C0F54 + (code/100)*0x170 DIRECTLY (not GetCharaGameProp): code 8600 -> record 86 = not
    // the template (CAM moveset preset area). Same buffer goes to CComCommon::CreateACom 0x2313a0 (AI). Fix: point a2 /
    // a1 at the slot's record (template org record + slot fields) -> template moves, stats, AI.
    RosterxSlot *rosterxFromChara(const uint8_t *rdram, uint32_t chara)
    {
        if (!chara || chara > 0x2000000u - 0x34u)
            return nullptr;
        uint32_t code;
        std::memcpy(&code, rdram + chara + 0x30u, 4);
        return code < 10000u ? rosterxFromCodeId(rdram, code / 100u) : nullptr;
    }
    PS2Runtime::RecompiledFunction g_rxOrigPropCtor = nullptr, g_rxOrigCreateACom = nullptr, g_rxOrigWinnerProp = nullptr,
                                   g_rxOrigSuccessAct = nullptr;
    void rosterxCreateACom(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (RosterxSlot *s = rosterxFromChara(rdram, rxArg(ctx, 4) & 0x1FFFFFFu))
            if (const uint32_t rec = rosterxRecord(rdram, runtime, *s))
                SET_GPR_U32(ctx, 5, rec);
        g_rxOrigCreateACom(rdram, ctx, runtime);
    }
    // tagWinner::GetProp 0x39b480 (a0 tagWinner, a1 slot): code = *(*(a0+a1*4)+0xA); code/100 = 86 fails slti 0x56 ->
    // CAW block index 86-100 = -14 (garbage record on the results/doramock path). Slot -> its record.
    void rosterxWinnerProp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sel;
        std::memcpy(&sel, rdram + ((rxArg(ctx, 4) + rxArg(ctx, 5) * 4u) & 0x1FFFFFFu), 4);
        sel &= 0x1FFFFFFu;
        if (sel && sel < 0x2000000u - 0xCu)
        {
            uint16_t code;
            std::memcpy(&code, rdram + sel + 0xAu, 2);
            if (code < 10000u)
                if (RosterxSlot *s = rosterxFromCodeId(rdram, code / 100u))
                    if (const uint32_t rec = rosterxRecord(rdram, runtime, *s))
                    {
                        SET_GPR_U32(ctx, 2, rec);
                        ctx->pc = rxArg(ctx, 31);
                        return;
                    }
        }
        g_rxOrigWinnerProp(rdram, ctx, runtime);
    }
    // IsSuccessAct 0x39d1a0 (a0 tagWinner): reads both codes (+0xA of *(a0), *(a0+4)) ONLY in its first block, then uses
    // registers -> swap slot codes to the template's for the call, restore after (safe even if it pauses later).
    void rosterxSuccessAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t at[2] = {};
        uint16_t old[2] = {};
        for (uint32_t i = 0; i < 2; ++i)
        {
            uint32_t sel;
            std::memcpy(&sel, rdram + ((rxArg(ctx, 4) + i * 4u) & 0x1FFFFFFu), 4);
            sel &= 0x1FFFFFFu;
            if (!sel || sel >= 0x2000000u - 0xCu)
                continue;
            std::memcpy(&old[i], rdram + sel + 0xAu, 2);
            if (old[i] < 10000u)
                if (const RosterxSlot *s = rosterxFromCodeId(rdram, old[i] / 100u))
                {
                    const uint16_t t = static_cast<uint16_t>(rosterxTemplateCode(rdram, *s, old[i]));
                    std::memcpy(rdram + sel + 0xAu, &t, 2);
                    at[i] = sel;
                }
        }
        g_rxOrigSuccessAct(rdram, ctx, runtime);
        for (uint32_t i = 0; i < 2; ++i)
            if (at[i])
                std::memcpy(rdram + at[i] + 0xAu, &old[i], 2);
    }
    void rosterxPropCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = rxArg(ctx, 4) & 0x1FFFFFFu, chara = rxArg(ctx, 5) & 0x1FFFFFFu;
        if (RosterxSlot *s = rosterxFromChara(rdram, chara))
            if (const uint32_t rec = rosterxRecord(rdram, runtime, *s))
            {
                SET_GPR_U32(ctx, 6, rec);
                static std::set<uint16_t> told;
                if (told.insert(s->id).second)
                    std::fprintf(stderr, "[rosterx] %s: CCharProp from slot record (template 0x%X moveset)%c", s->name.c_str(),
                                 s->tmpl, 10);
            }
        g_rxOrigPropCtor(rdram, ctx, runtime);
        if (!self || self > 0x2000000u - 0x190u || !chara || chara > 0x2000000u - 0x34u)
            return;
        uint32_t code;
        std::memcpy(&code, rdram + chara + 0x30u, 4);
        if (code < 10000u)
            if (const RosterxSlot *s = rosterxFromCodeId(rdram, code / 100u))
            {
                const bool ok = (s->music < 0 && s->video < 0) && applyMediaDefaults(rdram, self + 0x20u, rosterxTemplateCode(rdram, *s, code));
                std::fprintf(stderr, "[rosterx] %s media: template row %s, music %u video %u%c", s->name.c_str(),
                             ok ? "applied" : "none", rdram[self + 0x20u + 0x15fu], rdram[self + 0x20u + 0x15eu], 10);
            }
    }
    // Nov 9: Create-a-Moveset picker (CSuperStarListDrawer::SetSuperStarList 0x3934c0, type 0 = wrestler list) walks ids
    // 0..0xA1 only. After it returns, slot ids (0xA2+) are inserted alphabetically (record name +0x116) into the list
    // (this+0x94 buffer of {u8 flag; u32 id@+4}, count this+0x98). The list buffer is malloc'd by 0x1001F0 at ra
    // 0x393580 (0xA2*8): enlarge it so the inserts fit.
    PS2Runtime::RecompiledFunction g_rxOrigSSList = nullptr, g_rxOrigMalloc = nullptr;
    void rosterxMallocSSList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rxArg(ctx, 31) == 0x393580u && rxArg(ctx, 4) < 0xC00u)
            SET_GPR_U32(ctx, 4, 0xC00u);
        g_rxOrigMalloc(rdram, ctx, runtime);
    }
    std::string rosterxLowerName(const uint8_t *rdram, uint32_t id, const RosterxSlot *s)
    {
        std::string n = s ? s->name : std::string(reinterpret_cast<const char *>(rdram + 0x4C0F54u + id * 0x170u + 0x116u), 0x15);
        n = n.substr(0, n.find('\0'));
        for (char &c : n)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return n;
    }
    void rosterxSSList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = rxArg(ctx, 4) & 0x1FFFFFFu, ra = rxArg(ctx, 31);
        g_rxOrigSSList(rdram, ctx, runtime);
        if (ctx->pc != ra || !self || self > 0x2000000u - 0xA0u)
            return;
        uint32_t type, buf, count;
        std::memcpy(&type, rdram + self + 0x24u, 4);
        std::memcpy(&buf, rdram + self + 0x94u, 4);
        std::memcpy(&count, rdram + self + 0x98u, 4);
        buf &= 0x1FFFFFFu;
        if (type != 0u || !buf || count == 0u || count > 0x100u)
            return;
        for (const RosterxSlot &s : g_rosterx)
        {
            uint32_t pos = count;
            const std::string key = rosterxLowerName(rdram, s.id, &s);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t id;
                std::memcpy(&id, rdram + buf + i * 8u + 4u, 4);
                if (id == s.id)
                {
                    pos = 0xFFFFFFFFu; // already there
                    break;
                }
                if (pos == count && id < 0xA2u && rosterxLowerName(rdram, id, nullptr) > key)
                    pos = i;
            }
            if (pos == 0xFFFFFFFFu)
                continue;
            std::memmove(rdram + buf + (pos + 1u) * 8u, rdram + buf + pos * 8u, (count - pos) * 8u);
            std::memset(rdram + buf + pos * 8u, 0, 8);
            rdram[buf + pos * 8u] = 1;
            const uint32_t id32 = s.id;
            std::memcpy(rdram + buf + pos * 8u + 4u, &id32, 4);
            ++count;
        }
        std::memcpy(rdram + self + 0x98u, &count, 4);
    }
    // Installed LAST in applyHctp so it wraps the cut-roster / Hogan / media hooks on the same functions.
    void installRosterExpansion(PS2Runtime &runtime)
    {
        // Nov 10 STEP 1: SvR04 entrance table registers at boot, independent of roster expansion; slot.txt
        // entrance=svr04:<no> then reuses the table number.
        svrEntranceTableLoad();
        const char *rxEnv = std::getenv("HCTP_ROSTER_EXPAND");
        const bool rx = rxEnv != nullptr && rxEnv[0] == '1';
        const int rxSlots = rx ? rosterxScan() : 0;
        // Nov 9 SvR06 proof: CAM row 3944 plays the ORIGINAL SvR06 Brock 4044 (evt.pac) + its pyro. HCTP_SVR06_ENT=0 -> old
        // behaviour (stock 3044 + pyro overlay).
        // Nov 10: default on (CAM row "Brock Lesnar 2" in the normal launch); HCTP_SVR06_PYRO=0 off
        // Nov 10 STEP 2: every SvR06 single from MODS\SVR06\entrances.txt at its fixed 7400+ number; 3944 -> Brock's number.
        if (!(std::getenv("HCTP_SVR06_ENT") && std::getenv("HCTP_SVR06_ENT")[0] == '0'))
        {
            svr06TableLoad();
            if (auto b = g_svr06Registered.find(4044u); b != g_svr06Registered.end())
                g_svr06CamEvent = b->second;
        }
        svrEntranceInstall(runtime); // SvR04 table + slot.txt entrance=svr04:<no> -> SvR entrance reader
        if (!rx)
            return;
        if (rxSlots == 0)
        {
            std::cerr << "[rosterx] roster expansion on, no MODS\\ROSTER\\*\\slot.txt slots" << std::endl;
            return;
        }
        auto hook = [&runtime](uint32_t addr, PS2Runtime::RecompiledFunction &orig, PS2Runtime::RecompiledFunction fn)
        { return (orig = runtime.lookupFunction(addr)) != nullptr && runtime.replaceFunction(addr, fn); };
        bool ok = hook(0x12cec0u, g_rxOrigGetCharaGameProp, rosterxGetCharaGameProp);
        ok = hook(0x12cde0u, g_rxOrigName, rosterxName) && ok;
        ok = hook(0x12ce50u, g_rxOrigRing, rosterxRing) && ok;
        ok = hook(0x12cfd0u, g_rxOrigSdEntry, rosterxSdEntry) && ok;
        ok = hook(0x12d3c0u, g_rxOrigExist, rosterxExist) && ok;
        ok = hook(0x12d050u, g_rxOrigSeason, rosterxSeason) && ok;
        ok = hook(0x12cce0u, g_rxOrigMatchNo, rosterxMatchNo) && ok;
        rosterxInstallSubs(runtime, std::make_index_sequence<std::size(g_rxSubs)>{});
        ok = hook(0x1cd1d0u, g_rxOrigPropCtor, rosterxPropCtor) && ok;
        ok = hook(0x2313a0u, g_rxOrigCreateACom, rosterxCreateACom) && ok;
        ok = hook(0x39b480u, g_rxOrigWinnerProp, rosterxWinnerProp) && ok;
        ok = hook(0x39d1a0u, g_rxOrigSuccessAct, rosterxSuccessAct) && ok;
        ok = hook(0x3934c0u, g_rxOrigSSList, rosterxSSList) && ok;
        ok = hook(0x1001f0u, g_rxOrigMalloc, rosterxMallocSSList) && ok;
        std::cerr << "[rosterx] roster expansion: " << g_rosterx.size() << " new slots" << (ok ? "" : " (a hook FAILED)")
                  << std::endl;
        installSelectList(runtime); // no-op when the cut-roster restore installed it
        // Nov 7: slot entrance attires (<a>4.pac) ride the cut-roster GetEntranceCastNo hook (the CAW branch of
        // 0x2af480 returns the ring code for model nos >= 0x56); install it here when no cut wrestler did.
        if (!g_cutEntranceCodes.empty() && g_origGetEntranceCastNo == nullptr &&
            hook(0x2af480u, g_origGetEntranceCastNo, getEntranceCastNoCutRoster))
            std::cerr << "[rosterx] entrance attire hook on (" << g_cutEntranceCodes.size() << " codes)" << std::endl;
    }
    // ===================== end ROSTER EXPANSION =====================
    uint32_t rosterxCodeIdImpl(const uint8_t *rdram, uint32_t code)
    {
        if (code >= 10000u)
            return (code - 10000u) / 100u + 0x56u;
        if (const RosterxSlot *s = rosterxFromCodeId(rdram, code / 100u))
            return s->id; // Nov 9: the slot's own id (commentary / Finkel rows, names); was the template's (Piper lines for Bret)
        return code / 100u;
    }

    // ===================== ANY-BONE PHASE 1 (HCTP_ANYBONE=0 off; Nov 6, spec notes\ANY_BONE_ANIMATION_RESEARCH_20261106.md s9) =====
    // CSkinModel::FastUpdate 0x151160 (a0 = model): +0xEC bone count, +0xF0 -> bone records (80 B: name @0, parent @0x30),
    // +0xCC -> LOCAL matrices (64 B, 4x4 floats) written by motion/face/IK before FastUpdate (verified: lw 0xCC/0xEC/0xF0($s1)).
    // Host prelude only, no guest calls: SvR-only face bones get their bind local rotation times the driver bone's rotation
    // change from bind (slerp-scaled), recomputed from cached binds every frame. Stock/SYM models have none of these names.
    struct AnyBoneRule
    {
        uint16_t bone, driver;
        float scale;
        float bind[16], driverBind[16];
    };
    // Nov 6 FACE FIX: HCTP face motion writes u/d_kuchi, l/r_kuchi, l/r_mabuta, l/r_eye, l/r_mayu as an atama-space
    // rotation from HCTP's bind (identity; d_kuchi Y 0.1396). SvR binds those bones at (-90,90,0) (kuchi under d_kuchi),
    // so the raw write threw the lips/eyes off. Retarget: rel_atama = svrBindRel * (hctpBind^T * motion), local =
    // rel_atama * parentRel^T, SvR bind translation kept. lastOut/lastIn keep it idempotent when motion skips a frame.
    struct AnyBoneFace
    {
        uint16_t bone;
        int16_t parentFace; // index into faces[] or -1 (= parent is atama)
        float bindRel[9], hctpBind[9], localBind[9], pos[4];
        float lastIn[16], lastOut[16];
        bool have = false;
        float rel[9];
    };
    struct AnyBoneModel
    {
        uint32_t bones = 0, count = 0;
        std::vector<AnyBoneRule> rules;
        std::vector<AnyBoneFace> faces;
        // Nov 7 JAW FRAME: face bones whose base (bone record +0x20) is not HCTP's near-identity base.
        struct Jaw { uint16_t bone; float B[9]; float saved[16]; };
        std::vector<Jaw> jaws;
    };
    void abEuler(float rx, float ry, float rz, float *m) // row-vector X*Y*Z (tools\retarget_svr_molly.py rot), 3x3
    {
        const float cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry), cz = std::cos(rz), sz = std::sin(rz);
        const float X[9] = {1, 0, 0, 0, cx, sx, 0, -sx, cx}, Y[9] = {cy, 0, -sy, 0, 1, 0, sy, 0, cy}, Z[9] = {cz, sz, 0, -sz, cz, 0, 0, 0, 1};
        float t[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t[i * 3 + j] = X[i * 3] * Y[j] + X[i * 3 + 1] * Y[3 + j] + X[i * 3 + 2] * Y[6 + j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i * 3 + j] = t[i * 3] * Z[j] + t[i * 3 + 1] * Z[3 + j] + t[i * 3 + 2] * Z[6 + j];
    }
    void abMul3(const float *a, const float *b, float *o, bool bT = false) // o = a * b (or a * b^T)
    {
        float t[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t[i * 3 + j] = bT ? a[i * 3] * b[j * 3] + a[i * 3 + 1] * b[j * 3 + 1] + a[i * 3 + 2] * b[j * 3 + 2]
                                  : a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        std::memcpy(o, t, sizeof(t));
    }
    std::map<uint32_t, AnyBoneModel> g_anyBone;
    PS2Runtime::RecompiledFunction g_origFastUpdate = nullptr;
    void abQuat(const float *m, float q[4]) // 3x3 rotation (rows 0..2 of a 4x4) -> quaternion (x, y, z, w)
    {
        const float t = m[0] + m[5] + m[10];
        if (t > 0.f)
        {
            const float s = std::sqrt(t + 1.f) * 2.f;
            q[3] = 0.25f * s, q[0] = (m[6] - m[9]) / s, q[1] = (m[8] - m[2]) / s, q[2] = (m[1] - m[4]) / s;
        }
        else if (m[0] > m[5] && m[0] > m[10])
        {
            const float s = std::sqrt(1.f + m[0] - m[5] - m[10]) * 2.f;
            q[3] = (m[6] - m[9]) / s, q[0] = 0.25f * s, q[1] = (m[4] + m[1]) / s, q[2] = (m[8] + m[2]) / s;
        }
        else if (m[5] > m[10])
        {
            const float s = std::sqrt(1.f + m[5] - m[0] - m[10]) * 2.f;
            q[3] = (m[8] - m[2]) / s, q[0] = (m[4] + m[1]) / s, q[1] = 0.25f * s, q[2] = (m[9] + m[6]) / s;
        }
        else
        {
            const float s = std::sqrt(1.f + m[10] - m[0] - m[5]) * 2.f;
            q[3] = (m[1] - m[4]) / s, q[0] = (m[8] + m[2]) / s, q[1] = (m[9] + m[6]) / s, q[2] = 0.25f * s;
        }
    }
    void abQuatMat(const float q[4], float *m) // inverse of abQuat (writes the 3x3 part)
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + z * w), m[2] = 2 * (x * z - y * w);
        m[4] = 2 * (x * y - z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + x * w);
        m[8] = 2 * (x * z + y * w), m[9] = 2 * (y * z - x * w), m[10] = 1 - 2 * (x * x + y * y);
    }
    bool abSided(const std::string &s, const char *rest) // "l_<rest>" / "r_<rest>" prefix match
    {
        return s.size() > 2 && (s[0] == 'l' || s[0] == 'r') && s[1] == '_' && s.compare(2, std::strlen(rest), rest) == 0;
    }
    bool abForeignName(const std::string &s)
    {
        for (const char *p : {"sita_", "u_kuchi_0", "d_kuchi_0", "hana", "kami_b", "hira_", "momo_x", "kubi_x", "kote_x2"})
            if (s.rfind(p, 0) == 0 || abSided(s, p))
                return true;
        return abSided(s, "u_kuchi") || abSided(s, "mayu_0") || s == "l_hoho" || s == "r_hoho" || s == "l_meziri" ||
               s == "r_meziri";
    }
    bool abRule(const std::string &s, std::string &driver, float &scale) // spec s9 step 3 follow rules
    {
        scale = 1.f;
        if (s.size() == 7 && s.rfind("sita_0", 0) == 0 && s[6] >= '0' && s[6] <= '5')
            return driver = "d_kuchi", scale = 1.f - 0.1f * float(s[6] - '0'), true;
        if (s.rfind("d_kuchi_0", 0) == 0)
            return driver = "d_kuchi", true;
        if (s.rfind("u_kuchi_0", 0) == 0 || s == "l_u_kuchi" || s == "r_u_kuchi")
            return driver = "u_kuchi", true;
        if (abSided(s, "mayu_0"))
            return driver = s.substr(0, 2) + "mayu", true;
        if (s == "l_hoho" || s == "r_hoho")
            return driver = s.substr(0, 2) + "kuchi", scale = 0.5f, true;
        if (s == "l_meziri" || s == "r_meziri")
            return driver = s.substr(0, 2) + "mabuta", scale = 0.5f, true;
        return false; // momo_x / kubi_x / kote_x2 / hana / kami / hira: follow their parent unchanged
    }
    void fastUpdateAnyBone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t model = rxArg(ctx, 4) & 0x1FFFFFFu;
        ++g_mprobeFast[model]; // TEMP diag HCTP_MATCHUP_PROBE (mprobe2)
        recWatchCheck(rdram, "FastUpdate", rxArg(ctx, 31));
        AnyBoneModel *restore = nullptr; // Nov 7 JAW: face bones whose HCTP motion input is put back after skinning
        uint32_t restoreLocals = 0u;
        if (model && model < 0x2000000u - 0x100u)
        {
            const uint32_t count = rdU32(rdram, model + 0xECu), bones = rdU32(rdram, model + 0xF0u) & 0x1FFFFFFu,
                           locals = rdU32(rdram, model + 0xCCu) & 0x1FFFFFFu;
            if (count > 0 && count <= 256 && bones && locals && bones + count * 80u < 0x2000000u &&
                locals + count * 64u < 0x2000000u)
            {
                AnyBoneModel &m = g_anyBone[model];
                if (m.bones != bones || m.count != count)
                {
                    m = AnyBoneModel{bones, count, {}};
                    auto name = [&](uint32_t i) {
                        char n[17] = {};
                        std::memcpy(n, rdram + bones + i * 80u, 16);
                        return std::string(n);
                    };
                    int foreign = 0;
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const std::string n = name(i);
                        std::string drv;
                        float sc;
                        if (!abForeignName(n))
                            continue;
                        ++foreign;
                        if (!abRule(n, drv, sc))
                            continue;
                        for (uint32_t d = 0; d < count; ++d)
                            if (name(d) == drv)
                            {
                                // Nov 6: a child of its driver (d_kuchi_0N, sita_*, u_kuchi_00) already follows it by FK;
                                // a rule would apply the jaw motion twice (Jordan: lips deform weirdly on SvR faces).
                                bool ancestor = false;
                                for (int32_t p = static_cast<int32_t>(rdU32(rdram, bones + i * 80u + 0x30u)), g = 0;
                                     p >= 0 && p < static_cast<int32_t>(count) && g < 64; ++g,
                                             p = static_cast<int32_t>(rdU32(rdram, bones + uint32_t(p) * 80u + 0x30u)))
                                    if (uint32_t(p) == d)
                                        ancestor = true;
                                if (ancestor)
                                    break;
                                AnyBoneRule r{static_cast<uint16_t>(i), static_cast<uint16_t>(d), sc, {}, {}};
                                std::memcpy(r.bind, rdram + locals + i * 64u, 64);
                                std::memcpy(r.driverBind, rdram + locals + d * 64u, 64);
                                m.rules.push_back(r);
                                break;
                            }
                    }
                    // FACE FIX set (foreign models only): HCTP face names whose bind rotation is not HCTP's.
                    int atama = -1;
                    for (uint32_t i = 0; i < count && foreign; ++i)
                        if (name(i) == "atama")
                            atama = static_cast<int>(i);
                    // Nov 7 FACE NATIVE: OFF by default. HCTP's face writer is already any-skeleton: CSD3MotionFace ctor 0x1c0500
                    // (0x1c0800-0x1c0820) builds each face bone's base from the MODEL's own bone record +0x20 (func_145110),
                    // ActionLink 0x1c0be0 (0x1c0d08-0x1c0d8c) writes local = expression rot x that base (SvR06 0x214080 same:
                    // RotXYX x per-bone base). Converting again = the jaw flicker. HCTP_ANYBONE_FACE=1 = old conversion.
                    if (foreign && atama >= 0 && std::getenv("HCTP_ANYBONE_FACE") && std::getenv("HCTP_ANYBONE_FACE")[0] == '1')
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            const std::string n = name(i);
                            static const char *kFace[] = {"u_kuchi", "d_kuchi", "l_kuchi", "r_kuchi", "l_mabuta", "r_mabuta",
                                                          "l_eye", "r_eye", "l_mayu", "r_mayu"};
                            if (std::find_if(std::begin(kFace), std::end(kFace), [&](const char *f) { return n == f; }) == std::end(kFace))
                                continue;
                            const int32_t par = static_cast<int32_t>(rdU32(rdram, bones + i * 80u + 0x30u));
                            AnyBoneFace f{};
                            f.bone = static_cast<uint16_t>(i);
                            f.parentFace = -1;
                            if (par != atama)
                            {
                                for (size_t k = 0; k < m.faces.size(); ++k)
                                    if (m.faces[k].bone == uint32_t(par))
                                        f.parentFace = static_cast<int16_t>(k);
                                if (f.parentFace < 0)
                                    continue; // parent neither atama nor a fixed face bone: leave as is
                            }
                            float r3[3];
                            std::memcpy(r3, rdram + bones + i * 80u + 0x20u, 12);
                            std::memcpy(f.pos, rdram + bones + i * 80u + 0x10u, 12);
                            f.pos[3] = 1.f;
                            // Nov 7 JAW FIX: SvR builds bind rotations with objMatrix::RotZYX (SvR04 0x1105a0, labels csv),
                            // not abEuler's order -> d_kuchi (-90,125,0) came out wrong = jaw inside the head. Take the
                            // atama-relative bind straight from the model's inverse binds (+0xD4, skin = invBind * world):
                            // bindRel = inv(IB_bone) * IB_atama; localBind = bindRel * parentRel^T for face children.
                            const uint32_t ibs = rdU32(rdram, model + 0xD4u) & 0x1FFFFFFu;
                            float b[9];
                            abEuler(r3[0], r3[1], r3[2], b);
                            if (ibs && ibs + count * 64u < 0x2000000u)
                            {
                                float ib[16], ia[16];
                                std::memcpy(ib, rdram + ibs + i * 64u, 64);
                                std::memcpy(ia, rdram + ibs + uint32_t(atama) * 64u, 64);
                                const float B[9] = {ib[0], ib[1], ib[2], ib[4], ib[5], ib[6], ib[8], ib[9], ib[10]};
                                const float A[9] = {ia[0], ia[1], ia[2], ia[4], ia[5], ia[6], ia[8], ia[9], ia[10]};
                                const float det = B[0] * (B[4] * B[8] - B[5] * B[7]) - B[1] * (B[3] * B[8] - B[5] * B[6]) +
                                                  B[2] * (B[3] * B[7] - B[4] * B[6]);
                                if (std::fabs(det) > 1e-6f)
                                {
                                    const float k = 1.f / det;
                                    const float Bi[9] = {(B[4] * B[8] - B[5] * B[7]) * k, (B[2] * B[7] - B[1] * B[8]) * k,
                                                         (B[1] * B[5] - B[2] * B[4]) * k, (B[5] * B[6] - B[3] * B[8]) * k,
                                                         (B[0] * B[8] - B[2] * B[6]) * k, (B[2] * B[3] - B[0] * B[5]) * k,
                                                         (B[3] * B[7] - B[4] * B[6]) * k, (B[1] * B[6] - B[0] * B[7]) * k,
                                                         (B[0] * B[4] - B[1] * B[3]) * k};
                                    abMul3(Bi, A, f.bindRel);
                                    if (f.parentFace >= 0)
                                        abMul3(f.bindRel, m.faces[f.parentFace].bindRel, b, true);
                                    else
                                        std::memcpy(b, f.bindRel, sizeof(b));
                                }
                                else if (f.parentFace >= 0)
                                    abMul3(b, m.faces[f.parentFace].bindRel, f.bindRel);
                                else
                                    std::memcpy(f.bindRel, b, sizeof(b));
                            }
                            else if (f.parentFace >= 0)
                                abMul3(b, m.faces[f.parentFace].bindRel, f.bindRel);
                            else
                                std::memcpy(f.bindRel, b, sizeof(b));
                            std::memcpy(f.localBind, b, sizeof(b));
                            const bool hctpLike = std::fabs(r3[0]) < 0.01f && std::fabs(r3[2]) < 0.01f &&
                                                  std::fabs(r3[1] - (n == "d_kuchi" ? 0.1396f : 0.f)) < 0.01f && f.parentFace < 0;
                            if (hctpLike)
                                continue; // already HCTP convention
                            abEuler(0.f, n == "d_kuchi" ? 0.1396f : 0.f, 0.f, f.hctpBind);
                            m.faces.push_back(f);
                        }
                    // Nov 7 JAW FRAME (notes side-session JAW_FACE_NATIVE_1107.md): face keys ride in each MOVE's motion
                    // (CCharMotionPts::ActionLink 0x1cc880 -> CSD3MotionFace::SetData 0x1c0f40), authored for HCTP's
                    // near-identity face bases; ActionLink 0x1c0be0 writes local = R x B (B = XYZ Euler of bone record
                    // +0x20, matches jawprobe frame 0). SvR bases are rotated (Bret d_kuchi -90,125,0) -> R turns about
                    // the wrong axis (jaw wobble). Apply the same pose in the SvR bone's frame: local = B x R = B C B^T.
                    // Foreign models only; HCTP_JAW_FRAME=0 = off.
                    static const bool s_jawFrame = !(std::getenv("HCTP_JAW_FRAME") && std::getenv("HCTP_JAW_FRAME")[0] == '0');
                    if (foreign && s_jawFrame && m.faces.empty())
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            static const char *kJaw[] = {"u_kuchi", "d_kuchi", "l_kuchi", "r_kuchi", "l_mabuta", "r_mabuta",
                                                         "l_mayu", "r_mayu", "u_ha", "d_ha"};
                            const std::string n = name(i);
                            if (std::find_if(std::begin(kJaw), std::end(kJaw), [&](const char *f) { return n == f; }) == std::end(kJaw))
                                continue;
                            float r3[3];
                            std::memcpy(r3, rdram + bones + i * 80u + 0x20u, 12);
                            if (std::fabs(r3[0]) < 0.01f && std::fabs(r3[2]) < 0.01f && std::fabs(r3[1]) < 0.2f)
                                continue; // HCTP-like base (rotation about Y only, small): R x B == B x R already
                            AnyBoneModel::Jaw j{};
                            j.bone = static_cast<uint16_t>(i);
                            abEuler(r3[0], r3[1], r3[2], j.B);
                            m.jaws.push_back(j);
                        }
                    if (foreign && std::getenv("HCTP_MATCHUP_PROBE"))
                    {
                        g_recWatch[bones + 80u] = {model, true};
                        std::fprintf(stderr, "[recwatch] model 0x%X recs 0x%X ymp 0x%X locals 0x%X%c", model, bones,
                                     rdU32(rdram, model + 0xC8u), locals, 10);
                    }
                    if (foreign)
                        std::fprintf(stderr, "[anybone] model 0x%X %d foreign bones, %zu driven, %zu face retargeted, %zu jaw-frame%c",
                                     model, foreign, m.rules.size(), m.faces.size(), m.jaws.size(), 10);
                }
                for (const AnyBoneRule &r : m.rules)
                {
                    float cur[16], d[16] = {}, q[4], out[16];
                    std::memcpy(cur, rdram + locals + r.driver * 64u, 64);
                    for (int i = 0; i < 3; ++i) // delta = driverBind^T * cur (rotation part)
                        for (int j = 0; j < 3; ++j)
                            d[i * 4 + j] = r.driverBind[i] * cur[j] + r.driverBind[4 + i] * cur[4 + j] +
                                           r.driverBind[8 + i] * cur[8 + j];
                    abQuat(d, q);
                    if (r.scale != 1.f)
                    {
                        if (q[3] < 0.f)
                            for (float &c : q)
                                c = -c;
                        const float ang = std::acos(std::min(1.f, q[3])), sa = std::sin(ang);
                        if (sa > 1e-5f)
                        {
                            const float k = std::sin(ang * r.scale) / sa;
                            q[0] *= k, q[1] *= k, q[2] *= k, q[3] = std::cos(ang * r.scale);
                        }
                    }
                    abQuatMat(q, d);
                    std::memcpy(out, r.bind, 64); // keep the bind translation; rotation = bind * delta
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            out[i * 4 + j] = r.bind[i * 4] * d[j] + r.bind[i * 4 + 1] * d[4 + j] + r.bind[i * 4 + 2] * d[8 + j];
                    std::memcpy(rdram + locals + r.bone * 64u, out, 64);
                }
                for (AnyBoneFace &f : m.faces) // after the follow rules (they read the raw motion values)
                {
                    float cur[16];
                    std::memcpy(cur, rdram + locals + f.bone * 64u, 64);
                    if (f.have && std::memcmp(cur, f.lastOut, 64) == 0)
                        std::memcpy(cur, f.lastIn, 64); // motion skipped this bone this frame: reuse its last input
                    else
                        std::memcpy(f.lastIn, cur, 64);
                    float rh[9] = {cur[0], cur[1], cur[2], cur[4], cur[5], cur[6], cur[8], cur[9], cur[10]};
                    float diff = 0.f, dh = 0.f; // distance to the SvR bind / to HCTP's bind
                    for (int i = 0; i < 9; ++i)
                        diff = std::max(diff, std::fabs(rh[i] - f.localBind[i])),
                        dh = std::max(dh, std::fabs(rh[i] - f.hctpBind[i]));
                    // Nov 7 JAW FLICKER (log 09:07): the game writes this bone in BOTH forms on alternate frames -
                    // HCTP form (near HCTP's bind) and already bind-relative SvR form (near the SvR bind). Convert
                    // only the HCTP form; SvR-form input is kept as is (converting it again = jaw inside the head).
                    const bool svrForm = diff < dh;
                    if (svrForm)
                    {
                        if (f.parentFace >= 0)
                            abMul3(rh, m.faces[f.parentFace].rel, f.rel);
                        else
                            std::memcpy(f.rel, rh, sizeof(rh));
                        std::memcpy(f.lastOut, cur, 64);
                        f.have = true;
                        continue; // local left untouched
                    }
                    float p[9], loc[9];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            p[i * 3 + j] = f.hctpBind[i] * rh[j] + f.hctpBind[3 + i] * rh[3 + j] + f.hctpBind[6 + i] * rh[6 + j];
                    abMul3(f.bindRel, p, f.rel);
                    if (f.parentFace >= 0)
                        abMul3(f.rel, m.faces[f.parentFace].rel, loc, true);
                    else
                        std::memcpy(loc, f.rel, sizeof(loc));
                    float out[16] = {loc[0], loc[1], loc[2], 0.f, loc[3], loc[4], loc[5], 0.f, loc[6], loc[7], loc[8], 0.f,
                                     f.pos[0], f.pos[1], f.pos[2], 1.f};
                    std::memcpy(rdram + locals + f.bone * 64u, out, 64);
                    if (static int s_jawLog = 0; (s_jawLog++ % 2400) < 8 && s_jawLog < 30000) // Nov 7 TEMP jaw diag (8 faces every ~5 s)
                        std::fprintf(stderr, "[anybone] face bone %u in [%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f] "
                                     "bindRel [%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f] out [%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f] par %d%c",
                                     f.bone, rh[0], rh[1], rh[2], rh[3], rh[4], rh[5], rh[6], rh[7], rh[8], f.bindRel[0], f.bindRel[1],
                                     f.bindRel[2], f.bindRel[3], f.bindRel[4], f.bindRel[5], f.bindRel[6], f.bindRel[7], f.bindRel[8],
                                     out[0], out[1], out[2], out[4], out[5], out[6], out[8], out[9], out[10], f.parentFace, 10);
                    std::memcpy(f.lastOut, out, 64);
                    f.have = true;
                }
                for (AnyBoneModel::Jaw &j : m.jaws) // Nov 7 JAW FRAME: C = R x B (game form) -> B x C x B^T = B x R
                {
                    float c[16], t[9], o[9];
                    std::memcpy(c, rdram + locals + j.bone * 64u, 64);
                    std::memcpy(j.saved, c, 64);
                    const float C[9] = {c[0], c[1], c[2], c[4], c[5], c[6], c[8], c[9], c[10]};
                    abMul3(C, j.B, t, true); // t = C x B^T = R
                    abMul3(j.B, t, o);       // o = B x R
                    c[0] = o[0], c[1] = o[1], c[2] = o[2], c[4] = o[3], c[5] = o[4], c[6] = o[5], c[8] = o[6], c[9] = o[7], c[10] = o[8];
                    std::memcpy(rdram + locals + j.bone * 64u, c, 64);
                }
                if (!m.faces.empty() || !m.jaws.empty())
                    restore = &m, restoreLocals = locals;
            }
        }
        // Nov 7 TEMP JAW PROBE (HCTP_JAW_PROBE=1): d_kuchi local of foreign models, 300 calls, before skinning.
        if (static const bool s_jp = std::getenv("HCTP_JAW_PROBE") && std::getenv("HCTP_JAW_PROBE")[0] == '1'; s_jp && model)
        {
            static int s_jpN = 0;
            auto it = g_anyBone.find(model);
            if (it != g_anyBone.end() && !it->second.rules.empty() && s_jpN < 300)
            {
                const uint32_t cnt = it->second.count, bn = it->second.bones, lc = rdU32(rdram, model + 0xCCu) & 0x1FFFFFFu;
                for (uint32_t i = 0; i < cnt && lc; ++i)
                    if (std::strncmp(reinterpret_cast<const char *>(rdram + bn + i * 80u), "d_kuchi", 8) == 0)
                    {
                        float l[16];
                        std::memcpy(l, rdram + lc + i * 64u, 64);
                        std::fprintf(stderr, "[jawprobe] %d model 0x%X ra 0x%X rot [%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f] pos %.3f %.3f %.3f%c",
                                     s_jpN++, model, rxArg(ctx, 31), l[0], l[1], l[2], l[4], l[5], l[6], l[8], l[9], l[10], l[12], l[13], l[14], 10);
                        break;
                    }
            }
        }
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        g_origFastUpdate(rdram, ctx, runtime);
        // Nov 7 JAW FIX 2 (jaw diag log 08:51: d_kuchi input already in SvR form = the game's motion blends from the
        // local we left there -> converted twice -> jaw inside the head). After skinning, put the HCTP-form input back,
        // so the game's motion/blend only ever sees HCTP values. Only when FastUpdate really returned.
        if (restore && ctx->pc == ra)
            for (const AnyBoneFace &fb : restore->faces)
                if (fb.have)
                    std::memcpy(rdram + restoreLocals + fb.bone * 64u, fb.lastIn, 64);
        if (restore && ctx->pc == ra) // Nov 7 JAW FRAME: give the game back its own R x B form
            for (const AnyBoneModel::Jaw &j : restore->jaws)
                std::memcpy(rdram + restoreLocals + j.bone * 64u, j.saved, 64);
    }
    // Nov 7 SvR THIGH LENGTH (SvR04 CCharIK ctor 0x1ab200, svr_all.c): IK limb lengths are sums of bone record offsets
    // (+0x10). SvR: left thigh +0x140 = rec(29 l_momo_x).x + rec(14 l_sune).x, right +0x144 = rec(30 r_momo_x) + rec(18).
    // HCTP ctor 0x1b83d0 (0x1b86c0 / 0x1b8710) takes only sune -> on SvR models the IK thigh is half length (Bret 2.18
    // instead of 4.35) = broken knees. After the ctor, add momo_x's offset for models that have it.
    PS2Runtime::RecompiledFunction g_origIkCtor = nullptr;
    void ikCtorSvrThigh(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = rxArg(ctx, 4) & 0x1FFFFFFu, ra = rxArg(ctx, 31);
        g_origIkCtor(rdram, ctx, runtime);
        if (ctx->pc != ra || !self || self > 0x2000000u - 0x160u)
            return;
        const uint32_t link = rdU32(rdram, self + 0x2Cu) & 0x1FFFFFFu;
        const uint32_t model = link ? rdU32(rdram, link + 0x20u) & 0x1FFFFFFu : 0u;
        if (!model || !hctpIkSvrTwistBone(rdram, model, 0))
            return;
        const uint32_t recs = rdU32(rdram, model + 0xF0u) & 0x1FFFFFFu;
        for (int side = 0; side < 2; ++side)
        {
            const int b = hctpIkSvrTwistBone(rdram, model, side + 1);
            if (b < 0 || !recs)
                continue;
            float x, len;
            std::memcpy(&x, rdram + recs + uint32_t(b) * 80u + 0x10u, 4);
            std::memcpy(&len, rdram + self + 0x140u + side * 4u, 4);
            const float before = len;
            len += x;
            std::memcpy(rdram + self + 0x140u + side * 4u, &len, 4);
            std::fprintf(stderr, "[svrtwist] IK 0x%X %s thigh %.3f -> %.3f (+momo_x, SvR04 ctor 0x1ab200)%c", self,
                         side ? "right" : "left", before, len, 10);
        }
    }
    void installAnyBone(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("HCTP_ANYBONE"); e && e[0] == '0')
            return;
        if ((g_origFastUpdate = runtime.lookupFunction(0x151160u)) != nullptr &&
            runtime.replaceFunction(0x151160u, fastUpdateAnyBone))
            std::cerr << "[anybone] FastUpdate prelude on (SvR face bones follow HCTP drivers)" << std::endl;
        if ((g_origIkCtor = runtime.lookupFunction(0x1b83d0u)) != nullptr && runtime.replaceFunction(0x1b83d0u, ikCtorSvrThigh))
            std::cerr << "[svrtwist] CCharIK ctor thigh-length fix on (SvR momo_x)" << std::endl;
        if (!(std::getenv("HCTP_SVR_BLOOD") && std::getenv("HCTP_SVR_BLOOD")[0] == '0') &&
            (g_origCharCtor = runtime.lookupFunction(0x1b9c40u)) != nullptr && runtime.replaceFunction(0x1b9c40u, charCtorClearBlood))
            std::cerr << "[svrblood] CChar ctor clears +0x383 blood flag" << std::endl;
        if (std::getenv("HCTP_MATCHUP_PROBE"))
        {
            if ((g_origEditYmpFn[0] = runtime.lookupFunction(0x2d49a0u)) != nullptr) runtime.replaceFunction(0x2d49a0u, editYmpProbe<0>);
            if ((g_origEditYmpFn[1] = runtime.lookupFunction(0x2d5e60u)) != nullptr) runtime.replaceFunction(0x2d5e60u, editYmpProbe<1>);
        }
        if (std::getenv("HCTP_REPLAY") && std::getenv("HCTP_REPLAY")[0] != '0' &&
            (g_origReplayThunkOn = runtime.lookupFunction(0x1f76e0u)) != nullptr && runtime.replaceFunction(0x1f76e0u, gameReplayThunkRestore))
            std::cerr << "[replay] REPLAY HUD restore on (HCTP_REPLAY=1)" << std::endl;
        if (std::getenv("HCTP_REPLAY") && std::getenv("HCTP_REPLAY")[0] != '0' &&
            (g_origCtrlReplayAnimOn = runtime.lookupFunction(0x2379c0u)) != nullptr)
            runtime.replaceFunction(0x2379c0u, ctrlReplayAnimCapture);
        // OFF (Nov 7, Jordan test): level 4 also runs the wrestlers' motion -> the move kept playing through the replay.
        if (std::getenv("HCTP_REPLAY_LEVEL4") &&
            (g_origActLevelInit = runtime.lookupFunction(0x2e0ec0u)) != nullptr)
            runtime.replaceFunction(0x2e0ec0u, actLevelInitReplay);
        // Nov 7: decoded GAME2D.PAC - tex set 0 IS the REPLAY art (sets are keyed 0..E), so no override by default;
        // only for experiments with HCTP_REPLAY_TEX=<hex>.
        if (std::getenv("HCTP_REPLAY_TEX") &&
            (g_origGetTexInfosKey = runtime.lookupFunction(0x1a8060u)) != nullptr)
            runtime.replaceFunction(0x1a8060u, getTexInfosKeyReplay);
        if (std::getenv("HCTP_REPLAY_PROBE") && std::getenv("HCTP_REPLAY_PROBE")[0] != '0')
        {
            if ((g_origMakeCamEff = runtime.lookupFunction(0x208890u)) != nullptr) runtime.replaceFunction(0x208890u, makeCamEffProbe);
            if ((g_origCamMotThunk = runtime.lookupFunction(0x2088f0u)) != nullptr) runtime.replaceFunction(0x2088f0u, camMotThunkProbe);
            if ((g_origMakeReplayI = runtime.lookupFunction(0x1f7350u)) != nullptr) runtime.replaceFunction(0x1f7350u, makeGameReplayIProbe);
            if ((g_origReplayAction = runtime.lookupFunction(0x1f6d50u)) != nullptr) runtime.replaceFunction(0x1f6d50u, gameReplayActionProbe);
            if ((g_origReplayThunk = runtime.lookupFunction(0x1f76e0u)) != nullptr) runtime.replaceFunction(0x1f76e0u, gameReplayThunkProbe);
            if ((g_origMakeReplayV = runtime.lookupFunction(0x1f72f0u)) != nullptr) runtime.replaceFunction(0x1f72f0u, makeGameReplayVProbe);
            if ((g_origBlackLine = runtime.lookupFunction(0x1cdc00u)) != nullptr) runtime.replaceFunction(0x1cdc00u, makeBlackLineProbe);
            if ((g_origCtrlReplayAnim = runtime.lookupFunction(0x2379c0u)) != nullptr) runtime.replaceFunction(0x2379c0u, ctrlReplayAnimProbe);
            if ((g_origFixCamReplay = runtime.lookupFunction(0x2071f0u)) != nullptr) runtime.replaceFunction(0x2071f0u, makeFixCamReplayProbe);
            std::cerr << "[replay] probe on (MakeCamEff / MakeGameReplay(int) / GameReplay::Action)" << std::endl;
        }
        if (std::getenv("HCTP_FLARE_PROBE") && (g_origFlareCalc = runtime.lookupFunction(0x228bf0u)) != nullptr)
            runtime.replaceFunction(0x228bf0u, flareCalcProbe);
        if (!(std::getenv("HCTP_SVR_BLOOD") && std::getenv("HCTP_SVR_BLOOD")[0] == '0') &&
            (g_origDrawCharBlood = runtime.lookupFunction(0x155dc0u)) != nullptr &&
            runtime.replaceFunction(0x155dc0u, drawCharSvrBlood))
            std::cerr << "[svrblood] DrawChar blood-layer skip on (SvR 'blood' textures until bleeding)" << std::endl;
    }
    // ===================== end ANY-BONE =====================

    void applyHctp(PS2Runtime &runtime)
    {
        installOptionRestore(runtime);
        if (std::getenv("HCTP_DTXLOG") && (g_dtxOrigExec = runtime.lookupFunction(0x3ca4f8u)) != nullptr)
            runtime.replaceFunction(0x3ca4f8u, dtxExecLogged);
        // CSR reads that test only FIELD (bit 13): yGsSyncV (vsync wait loop) and crisys_VSyncCallBack. Fenced, each
        // waited for the whole chain worker (7 + 4 s per 260 s demo run). Same exemption as Vendetta's 0x1A16C0.
        runtime.chain().allowUnfencedCsrRead(0x14c664u);
        runtime.chain().allowUnfencedCsrRead(0x15881cu);
        // In-game interlace shake (Nov 4): GameRender::FlickerFree 0x1fea60 (called by CRender::Synch every frame in
        // a match) copies the 512-line scene into the 256-line display buffer with a bilinear sprite whose V starts
        // at 0 when CSR.FIELD = 1 (ld at 0x1feb0c) and at 1 source line when FIELD = 0 - half a TV line, cancelled
        // by the field's own offset on a CRT, a half-line bounce every frame here (worst on still screens/pause).
        // Pin FIELD = 1 there (centred: each output line = rows 2y/2y+1). HCTP_FLICKER_FIELD=0 = raw (still counted).
        {
            const char *ff = std::getenv("HCTP_FLICKER_FIELD");
            const bool pin = !(ff && *ff == '0');
            ps2PinCsrField(0x1feb0cu, pin, true);
            std::cerr << "[hctp] flicker-free field pin " << (pin ? "on (FIELD=1)" : "off") << std::endl;
        }
        // DIAG [adxmon] (HCTP_ADXMON=1): every 5 s the first 0x40 bytes of the 4 ADXT handles (0x425cc8 + i*0xc4;
        // byte 0 = used, byte 1 = stat 0 stop 1 decinfo 2 prep 3 playing 4 end 5 error) - finds stuck music streams.
        if (std::getenv("HCTP_ADXMON"))
        {
            uint8_t *const ram = runtime.memory().getRDRAM();
            std::thread([ram] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    for (uint32_t i = 0; i < 4u; ++i)
                    {
                        const uint32_t h = 0x425cc8u + i * 0xc4u;
                        char line[400];
                        int n = std::snprintf(line, sizeof(line), "[adxmon] h%u 0x%x:", i, h);
                        for (uint32_t o = 0; o < 0x40u && n < 380; o += 4)
                        {
                            uint32_t w;
                            std::memcpy(&w, ram + h + o, 4);
                            n += std::snprintf(line + n, sizeof(line) - n, " %08x", w);
                        }
                        std::fprintf(stderr, "%s%c", line, 10);
                    }
                }
            }).detach();
        }
        {
            // NATIVE SOUND KIT step 1: capture every sound bank (HD header, BD samples, SQ sequences) as the game
            // hands it to the IOP: CCslRpcService::TransToIop(bank, tagTransHDBDSQ*) 0x2e19c0, info = {BD adr, BD
            // size, SPU adr, HD adr, HD size, SQ adr, SQ size} (Ghidra: CSkBankLoader::ReadEndProcess 0x2e2770).
            // The host copies feed the native synth (hctp_sk_bank()); HCTP_SKDUMP=<dir> also writes them out once
            // (local diag files - game data, never committed).
            g_skTransOrig = runtime.lookupFunction(0x2e19c0u);
            if (!g_skTransOrig || !runtime.replaceFunction(0x2e19c0u, skTransCaptured))
                std::cerr << "[skbank] TransToIop hook FAILED" << std::endl;
        }
        if (std::getenv("HCTP_NATIVE_SK") && std::getenv("HCTP_NATIVE_SK")[0] == '1')
        {
            // the effect banks' synth on the host (hctp_native_sk.inl); MIDI-sequence banks keep the IOP path
            bool ok = true;
            auto hook = [&](uint32_t addr, PS2Runtime::RecompiledFunction &orig, PS2Runtime::RecompiledFunction fn)
            {
                orig = runtime.lookupFunction(addr);
                ok = ok && orig && runtime.replaceFunction(addr, fn);
            };
            hook(0x2e3170u, g_skOrigBindMsin, skBindMsinBank);
            hook(0x2e5038u, g_skOrigMsinCtl, skMsinControl);
            hook(0x2e5138u, g_skOrigMsinStat, skMsinStatus);
            hook(0x2e4ad0u, g_skOrigHsynCtl, skHsynControl);
            hook(0x2e4a28u, g_skOrigHsynUnbind, skUnbind<0>);
            hook(0x2e4f90u, g_skOrigMsinUnbind, skUnbind<1>);
            std::cerr << (ok ? "[nativesk] native sound kit on (effect banks)" : "[nativesk] hook FAILED") << std::endl;
        }
        if (!std::getenv("HCTP_SHOW_YMEM"))
        {
            // RED BAR on the main menu: TopMenuSequence::TopDraw 0x138170 calls ymemDispMem(commonPage, 100) every
            // frame - the developers' heap meter (Ghidra: one 1-line objRect per 256 KB of the common ymem page,
            // 0x400000ff red = used, 0x40ff0000 blue = free, x 8..264, y 100 + 2 per row). On the PS2 it is not
            // visible (reference playthrough); here its lower rows show under the wrestler panel as a red strip.
            // Debug-only drawing whose return value TopDraw ignores: skipped (returns y like the original).
            if (!runtime.replaceFunction(0x1412c0u, [](uint8_t *, R5900Context *ctx, PS2Runtime *)
                                         { ctx->r[2] = _mm_cvtsi32_si128(_mm_cvtsi128_si32(ctx->r[5])); }))
                std::cerr << "[ymem] ymemDispMem skip FAILED" << std::endl;
        }
        if (std::getenv("HCTP_VU0_RECOMP"))
        {
            int n = 0;
#define HCTP_VU0_PUT(name, addr) n += runtime.replaceFunction(addr##u, name##_##addr) ? 1 : 0;
            HCTP_VU0_LIST(HCTP_VU0_PUT)
#undef HCTP_VU0_PUT
            std::cerr << "[vu0recomp] " << n << " libvu0 functions back to the recompiled game code" << std::endl;
        }
        if (std::getenv("HCTP_BONE_JITTER"))
        {
            g_boneJitterOrig = runtime.lookupFunction(0x155dc0u);
            if (!g_boneJitterOrig || !runtime.replaceFunction(0x155dc0u, boneJitterLogged))
                std::cerr << "[bonejit] hook FAILED" << std::endl;
            g_camMatOrig = runtime.lookupFunction(0x103ea8u);
            if (!g_camMatOrig || !runtime.replaceFunction(0x103ea8u, camMatLogged))
                std::cerr << "[camroot] camera hook FAILED" << std::endl;
            // Same probe on both world-matrix writers: CSkinModel::Update 0x150be0 and FastUpdate 0x151160
            g_mulN2Orig = runtime.lookupFunction(0x1510c0u);
            if (!g_mulN2Orig || !runtime.replaceFunction(0x1510c0u, mulN2Logged))
                std::cerr << "[mulring] hook FAILED" << std::endl;
            g_skinUpdateOrig = runtime.lookupFunction(0x150be0u);
            g_skinFastUpdateOrig = runtime.lookupFunction(0x151160u);
            if (!g_skinUpdateOrig || !runtime.replaceFunction(0x150be0u, [](uint8_t *r, R5900Context *c, PS2Runtime *p)
                                                              { skinUpdateCommon(r, c, p, g_skinUpdateOrig); }))
                std::cerr << "[skinupd] hook FAILED Update" << std::endl;
            if (!g_skinFastUpdateOrig || !runtime.replaceFunction(0x151160u, [](uint8_t *r, R5900Context *c, PS2Runtime *p)
                                                                  { skinUpdateCommon(r, c, p, g_skinFastUpdateOrig); }))
                std::cerr << "[skinupd] hook FAILED FastUpdate" << std::endl;
            g_ikAltOrig = runtime.lookupFunction(0x1b8860u);
            if (!g_ikAltOrig || !runtime.replaceFunction(0x1b8860u, ikAltLogged))
                std::cerr << "[ikalt] hook FAILED" << std::endl;
            // CMotionData::Hanten 0x1b6de0 (mirrors motion data in place) and its caller SetBoneData 0x1b6660: counted
            g_hantenOrig = runtime.lookupFunction(0x1b6de0u);
            g_setBoneDataOrig = runtime.lookupFunction(0x1b6660u);
            if (g_hantenOrig)
                runtime.replaceFunction(0x1b6de0u, [](uint8_t *r, R5900Context *c, PS2Runtime *p) { ++g_hantenCalls; g_hantenOrig(r, c, p); });
            if (g_setBoneDataOrig)
                runtime.replaceFunction(0x1b6660u, [](uint8_t *r, R5900Context *c, PS2Runtime *p) { ++g_setBoneDataCalls; g_setBoneDataOrig(r, c, p); });
        }
        if (std::getenv("HCTP_IKFLIP"))
        {
            g_ikFlipOrig = runtime.lookupFunction(0x1b8860u);
            if (!g_ikFlipOrig || !runtime.replaceFunction(0x1b8860u, ikFlipLogged))
                std::cerr << "[ikflip] hook FAILED" << std::endl;
        }
        if (std::getenv("HCTP_IKLOG"))
        {
            // TEMP diag (knee-down leg twitch): acosf (newlib, 0x11a5a8) inputs outside [-1, 1] and NaN results, with
            // the caller (CCharIK::ActionLink 0x1b8860..0x1b9650 = the two-bone leg / arm IK). Out-of-range input
            // = the law-of-cosines ratio past +-1: (x-x)/(x-x) is NaN on the PC but +-max on the PS2 FPU.
            g_ikAcosOrig = runtime.lookupFunction(0x11a5a8u);
            if (!g_ikAcosOrig || !runtime.replaceFunction(0x11a5a8u, ikAcosLogged))
                std::cerr << "[iklog] hook FAILED acosf" << std::endl;
        }
        if (std::getenv("HCTP_CRILOG"))
        {
            criHook<0>(runtime, 0x3b50c0u, "ADXF_ReadNw", true);
            criHook<1>(runtime, 0x3b5668u, "ADXF_GetStat");
            criHook<2>(runtime, 0x3c7128u, "cvFsReqRd", true);
            criHook<3>(runtime, 0x3cb060u, "dvCiReqRd", true);
            criHook<4>(runtime, 0x3c87c8u, "SRD_ReqRdDvd", true);
            criHook<5>(runtime, 0x3c90e0u, "SRD_ExecServer");
            criHook<6>(runtime, 0x3c8d30u, "srd_exec_dvd", true);
            criHook<7>(runtime, 0x3caa30u, "dvCiExecHndl");
            criHook<8>(runtime, 0x3caaf8u, "dvCiExecServer");
            criHook<9>(runtime, 0x3b5380u, "ADXF_ExecServer");
            criHook<10>(runtime, 0x3c7250u, "cvFsExecServer");
            criHook<11>(runtime, 0x3c8968u, "SRD_GetStat");
            criHook<12>(runtime, 0x3b53e0u, "ADXF_Seek", true);
            criHook<13>(runtime, 0x3b52b8u, "adxf_ExecOne");
            criHook<14>(runtime, 0x3b49e8u, "ADXF_Open", true);
            criHook<15>(runtime, 0x3b4b60u, "ADXF_OpenAfs", true);
            criHook<16>(runtime, 0x3b4c70u, "ADXF_Close", true);
            criHook<17>(runtime, 0x3b4180u, "adxf_LoadPtBothNw", true);
            criHook<18>(runtime, 0x3c6c90u, "cvFsOpen", true);
            criHook<19>(runtime, 0x3cadb0u, "dvCiOpen", true);
            criHook<20>(runtime, 0x3b5100u, "ADXF_Stop", true);
            criHook<21>(runtime, 0x3b51f0u, "ADXF_StopNw", true);
            criHook<22>(runtime, 0x3c6fe0u, "cvFsClose", true);
            criHook<23>(runtime, 0x3c72b0u, "cvFsGetStat");
            criHook<24>(runtime, 0x3cb308u, "dvCiGetStat");
            criHook<25>(runtime, 0x3b48a0u, "adxf_CreateAdxFs", true);
            criHook<26>(runtime, 0x3b4930u, "adxf_SetFileInfoEx", true);
            criHook<27>(runtime, 0x3cac88u, "dvCiGetFileSize", true);
            criHook<28>(runtime, 0x3c7318u, "cvFsGetFileSize", true);
            criHook<29>(runtime, 0x3bc440u, "ADXSTMF_CreateCvfs", true);
            criHook<30>(runtime, 0x3b3e18u, "adxf_SetCmdHstry", true);
            std::cerr << "[crilog] hooks installed" << std::endl;
        }
        (void)runtime;
        installTraceHooks(runtime);
        startPcProfiler();
        runtime.replaceFunction(0x11d810u, cdLayerSearchFile);
        installLooseEntrances(runtime);
        if (std::getenv("HCTP_NO_PPV") == nullptr)
            installPpvMenu(runtime);
        // native libsdr: sceSdRemote as SIF RPC to the real sdrdrv IRX (sound effects); the game's client data at
        // 0x48F240 and command buffer at 0x48F200 (from its sceSdRemoteInit / sceSdRemote). HCTP_SDR_STUB=1 = stand-in.
        if (std::getenv("HCTP_SDR_STUB") == nullptr)
        {
            ps2_stubs::setLibSdrNative(0x48F240u, 0x48F200u);
            std::cerr << "[hctp] libsdr: native (RPC to sdrdrv)" << std::endl;
        }
        if (!(runtime.replaceFunction(0x124448u, libcMallocR) && runtime.replaceFunction(0x121ae0u, libcCallocR) &&
              runtime.replaceFunction(0x123570u, libcFreeR) && runtime.replaceFunction(0x126c20u, libcReallocR)))
            std::cerr << "[heap] libc _r allocator override FAILED" << std::endl;
        const std::string moviesMode = std::getenv("HCTP_MOVIES") ? std::getenv("HCTP_MOVIES") : "";
        // Host catalog entries are loose MPEG files, not entries in the stock AFS.
        // Legacy launchers used =1 and sent those virtual entries to Sofdec -> black entrances.
        bool catalogHasVideo = false;
        {
            std::ifstream catalog("HOST_MEDIA.txt");
            for (std::string line; std::getline(catalog,line);)
                if (line.size()>1 && line[0]=='V' && line[1]==' ') { catalogHasVideo=true; break; }
        }
        if (moviesMode == "native")
            installNativeMovies(runtime);
        else if (moviesMode == "hybrid" || (moviesMode == "1" && catalogHasVideo))
            installNativeMovies(runtime,true);
        else if (moviesMode.empty() || moviesMode == "0")
        {
            const bool ok = runtime.replaceFunction(0x1377c0u, titleMovieLoopEndActSkip) &&
                            runtime.replaceFunction(0x23ddb0u, menuMovieCanUse) &&
                            runtime.replaceFunction(0x1b0ac0u, moviePlayerGetStat);
            std::cerr << "[hctp] movie skip hacks " << (ok ? "on" : "FAILED") << " (HCTP_MOVIES=1 = real movie code)" << std::endl;
        }
        if (std::getenv("HCTP_FONTLOG"))
        {
            g_origGetFontCell = runtime.lookupFunction(0x1625b0u);
            if (g_origGetFontCell)
                runtime.replaceFunction(0x1625b0u, getFontCellLogged);
            {
                PS2Runtime::RecompiledFunction fns[4] = {setTex0Logged<0>, setTex0Logged<1>, setTex0Logged<2>, setTex0Logged<3>};
                for (int i = 0; i < 4; ++i)
                {
                    g_origSetTex0[i] = runtime.lookupFunction(kSetTex0Addr[i]);
                    if (g_origSetTex0[i])
                        runtime.replaceFunction(kSetTex0Addr[i], fns[i]);
                }
            }
            g_origShowString = runtime.lookupFunction(0x162a20u);
            if (g_origShowString)
                runtime.replaceFunction(0x162a20u, showStringLogged);
        }
        if (std::getenv("HCTP_SPRLOG"))
        {
            g_origShowFast = runtime.lookupFunction(0x15eee0u);
            const bool ok = g_origShowFast && runtime.replaceFunction(0x15eee0u, showFastLogged);
            g_origSwitchToIdle = runtime.lookupFunction(0x3ea680u);
            if (g_origSwitchToIdle)
                runtime.replaceFunction(0x3ea680u, switchToIdleLogged);
            g_origSetData = runtime.lookupFunction(0x149940u);
            if (g_origSetData)
                runtime.replaceFunction(0x149940u, setDataLogged);
            std::cerr << "[sprlog] hook " << (ok ? "installed" : "NOT installed") << " orig=" << (g_origShowFast != nullptr) << std::endl;
        }
        if (const char *nadx = std::getenv("HCTP_NATIVE_ADX"); nadx && nadx[0] == '1')
            installNativeAdx(runtime);
        else if (std::getenv("HCTP_ADXTLOG"))
            installAdxtLog(runtime, std::make_integer_sequence<int, 15>{});
        if (std::getenv("HCTP_CHEERLOG"))
            installCheerLog(runtime, std::make_integer_sequence<int, 17>{});
        if (!std::getenv("HCTP_NO_CUT_ROSTER"))
        {
            g_origGetCharaGameProp = runtime.lookupFunction(0x12cec0u);
            const int slots = scanCutRosterModels();
            // Nov 6: always hooked (the Rosey name fix in restoreCutRoster must run with stock PACs too)
            const bool ok = g_origGetCharaGameProp &&
                            runtime.replaceFunction(0x12cec0u, getCharaGamePropCutRoster);
            if (slots > 0 && (g_origIsSeasonEntry = runtime.lookupFunction(0x12d050u)) != nullptr)
                runtime.replaceFunction(0x12d050u, isSeasonEntryCutRoster);
            if (slots > 0 && (g_origSeasonCharaFrameCtor = runtime.lookupFunction(0x316ba0u)) != nullptr)
                runtime.replaceFunction(0x316ba0u, seasonCharaFrameCtorCutRoster); // Nov 5: drops NPC slots only
            // Season roster: one Undertaker / one Hogan / ... (kOneOfGroups)
            runtime.replaceFunction(0x34ffe0u, checkRosterSpecialConditionGroups);
            runtime.replaceFunction(0x12d200u, isLegendExtended); // LEGEND banner list (kExtraLegends)
            if ((g_origSetRosterSpecialCondition = runtime.lookupFunction(0x34fd10u)) != nullptr)
                runtime.replaceFunction(0x34fd10u, setRosterSpecialConditionGroups);
            if ((g_origGetWarningMsg = runtime.lookupFunction(0x168e40u)) != nullptr)
                runtime.replaceFunction(0x168e40u, getWarningMsgGroups);
            if (slots > 0 && (g_origGetMatchWrestlerNo = runtime.lookupFunction(0x12cce0u)) != nullptr)
                runtime.replaceFunction(0x12cce0u, getMatchWrestlerNoCutRoster);
            std::cerr << "[hctp] cut roster restore: " << slots << " slots with models"
                      << (ok ? "" : " (hook FAILED)") << std::endl;
            if (slots > 0)
                installSelectList(runtime);
            if (!g_cutEntranceCodes.empty() && (g_origGetEntranceCastNo = runtime.lookupFunction(0x2af480u)) != nullptr &&
                runtime.replaceFunction(0x2af480u, getEntranceCastNoCutRoster))
                std::cerr << "[hctp] cut roster entrance attires: " << g_cutEntranceCodes.size() << std::endl;
        }
        if ((g_origInitEventSystem = runtime.lookupFunction(0x2ad340u)) != nullptr &&
            runtime.replaceFunction(0x2ad340u, std::getenv("HCTP_VICTORY_EVTK") ? initEventSystemEvtk : initEventSystemShirtTear))
            std::cerr << "[hctp] Hogan shirt-tear dress routing on" << std::endl;
        if ((g_origReallocatePage = runtime.lookupFunction(0x140c90u)) != nullptr &&
            runtime.replaceFunction(0x140c90u, reallocatePageBigModels))
            std::cerr << "[hctp] big-model char pages (SvR Molly) on" << std::endl;
        if (!(std::getenv("HCTP_KAMI_EXTRA") && std::getenv("HCTP_KAMI_EXTRA")[0] == '0') &&
            (g_origKamiMake = runtime.lookupFunction(0x1c5f50u)) != nullptr &&
            (g_origKamiAction = runtime.lookupFunction(0x1c5d70u)) != nullptr &&
            (g_origKamiDtor = runtime.lookupFunction(0x1c5cc0u)) != nullptr)
        {
            runtime.replaceFunction(0x1c5f50u, kamiMakeExtra);
            runtime.replaceFunction(0x1c5d70u, kamiActionExtra);
            runtime.replaceFunction(0x1c5cc0u, kamiDtorExtra);
            runtime.replaceFunction(0x1c6160u, kamiInitSeen);
            std::cerr << "[hctp] hair chains past 14 kept on host (Nero 3X Warrior) on" << std::endl;
        }
        if (!std::getenv("HCTP_INTERLACE_OFFSET"))
        {
            runtime.replaceFunction(0x102660u, gsSetHalfOffsetEven);
            runtime.replaceFunction(0x1028c8u, gsSetHalfOffsetEven);
            std::cerr << "[hctp] no-interlace half offset on" << std::endl;
        }
        if (!std::getenv("HCTP_FULL_GLOSS") && (g_origCharDraw = runtime.lookupFunction(0x1bb200u)) != nullptr)
            runtime.replaceFunction(0x1bb200u, charDrawReducedGloss);
        if (std::getenv("HCTP_NATIVE_MODEL_DIAG") &&
            (g_origSkinModelDraw = runtime.lookupFunction(0x155dc0u)) != nullptr)
        {
            const bool installed = runtime.replaceFunction(0x155dc0u, skinModelDrawDiagnostics);
            std::cerr << "[hctp] native CSkinModel identity diagnostic "
                      << (installed ? "on" : "NOT found") << " at 0x155dc0" << std::endl;
        }
        if (!std::getenv("HCTP_FULL_GLOSS"))
        {
            if ((g_origMenuWrDraw[0] = runtime.lookupFunction(0x243540u)) != nullptr)
                runtime.replaceFunction(0x243540u, menuWrestlerDrawReducedGloss<0>);
            if ((g_origMenuWrDraw[1] = runtime.lookupFunction(0x244620u)) != nullptr)
                runtime.replaceFunction(0x244620u, menuWrestlerDrawReducedGloss<1>);
        }
        if (std::getenv("HCTP_ROPELOG") && (g_origDrawRopeShadow = runtime.lookupFunction(0x217c50u)) != nullptr)
            runtime.replaceFunction(0x217c50u, drawRopeShadowLogged);
        if (std::getenv("HCTP_NO_ROPE_SHADOW"))
            runtime.replaceFunction(0x217c50u, skipRopeShadow);
        std::setvbuf(stdout, nullptr, _IONBF, 0); // diagnostics ([burst], [gs-trace]) survive a freeze / kill
        installImportedMedia(runtime);
        installHostMedia(runtime);
        installMediaDefaults(runtime);
        installAnyBone(runtime);
        installRosterExpansion(runtime); // keep LAST: wraps the hooks above on the same functions
        std::cerr << "[hctp] overrides applied" << std::endl;
    }
}

// Keep the commentary runtime decoupled from the override's private state,
// while exposing one stable host-side playback entry point.
extern "C" bool hctpNativePlayPbpClip(uint32_t fid)
{
    return hctpNativePlayAfsClipImpl(13u, fid);
}

extern "C" void hctpNativeStopCommentary()
{
    hctpNativeStopCommentaryImpl();
}

extern "C" bool hctpNativePlayAfsClip(uint32_t partition, uint32_t fid)
{
    return hctpNativePlayAfsClipImpl(partition, fid);
}

PS2_REGISTER_GAME_OVERRIDE("WWE SmackDown! Here Comes the Pain (USA)", "SLUS_207.87", 0x100008u, 0u, applyHctp)



// Create-a-Moveset picker (CEasySelectWrestler): its generated code computes record addresses as obj + 0x2C + i*0x80
// (inline, room for the stock 65). When that list has a long buffer the address is moved into it (hand edit in the
// 5 runner sites: AutoSelectAction 0x2fe18c, ChangeIntroduction 0x2fd698, SelectWresAction 0x2fdd44/0x2fddd0,
// SetCurrentRoster 0x2fe558).
uint32_t hctpEasyListAddr(uint32_t obj, uint32_t addr)
{
    auto it = g_frameLists.find(obj + 0x2Cu);
    if (it == g_frameLists.end() || it->second == 0u || addr < obj + 0x2Cu)
        return addr;
    const uint32_t index = (addr - obj - 0x2Cu) / kSelectRec;
    return index < g_selectCount ? it->second + index * kSelectRec : addr;
}

uint32_t hctpRosterCodeId(const uint8_t *rdram, uint32_t code) { return rosterxCodeIdImpl(rdram, code); }

// Nov 7 SvR TWIST LIST (notes\ANY_BONE_ANIMATION_RESEARCH_20261106.md s17): CCharIK::ActionLink 0x1b8860 re-solves
// the bones of the 12-entry twist list 0x4129f0 after the limb IK (loop 0x1b933c..0x1b95c4). SvR04 ActionLink 0x1ab6f0
// uses 14 entries 0x346020 = HCTP's with l/r_momo_x (enum 29/30) between momo and sune, limb groups >3,>7,>10, group
// ends 3,7,10,13. HCTP has no enum 29/30 (name table 0x4127c0 = 29 names), so those two are found by bone name.
// Hand edit in the runner ActionLink file calls these. Only for models that have "l_momo_x"; HCTP_ANYBONE=0 = off.
namespace
{
    std::map<uint32_t, std::pair<int, int>> g_svrTwistModels; // key = bone records ptr ^ count -> (l, r momo_x) or (-1,-1)
}
int hctpIkSvrTwistBone(const uint8_t *rdram, uint32_t model, int which) // which 0 = probe (returns 1 if SvR), 1 = l, 2 = r
{
    static const int s_off = (std::getenv("HCTP_ANYBONE") && std::getenv("HCTP_ANYBONE")[0] == '0') ? 1 : 0;
    if (s_off || model == 0u || model > PS2_RAM_SIZE - 0xF4u)
        return which ? -1 : 0;
    uint32_t count = 0u, recs = 0u;
    std::memcpy(&count, rdram + model + 0xECu, 4);
    std::memcpy(&recs, rdram + model + 0xF0u, 4);
    recs &= PS2_RAM_MASK;
    const uint32_t key = recs ^ (count << 24);
    auto it = g_svrTwistModels.find(key);
    if (it == g_svrTwistModels.end())
    {
        std::pair<int, int> v{-1, -1};
        if (count < 512u && recs != 0u && recs <= PS2_RAM_SIZE - count * 80u)
            for (uint32_t b = 0; b < count; ++b)
            {
                char nm[17] = {};
                std::memcpy(nm, rdram + recs + b * 80u, 16);
                if (std::strcmp(nm, "l_momo_x") == 0) v.first = static_cast<int>(b);
                if (std::strcmp(nm, "r_momo_x") == 0) v.second = static_cast<int>(b);
            }
        if (v.first < 0 || v.second < 0)
            v = {-1, -1};
        else
            std::fprintf(stderr, "[svrtwist] model 0x%X: SvR04 14-entry twist list (l_momo_x %d, r_momo_x %d)%c", model, v.first, v.second, 10);
        if (g_svrTwistModels.size() > 4096u)
            g_svrTwistModels.clear();
        it = g_svrTwistModels.emplace(key, v).first;
    }
    if (which == 0)
        return it->second.first >= 0 ? 1 : 0;
    return which == 1 ? it->second.first : it->second.second;
}

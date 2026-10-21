#include <utility>
#include <atomic>
// WWE SmackDown! Here Comes the Pain (USA, SLUS-20787) - runtime overrides for this game build.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides_hctp/ folder.
//
// HCTP_PCPROF=1: every 5 s, where the game thread / GS thread spend their time (host functions = recompiled game
// functions by name), via ps2_host_profile::samplePc / pcReport (Kernel/HostProfiler.cpp, needs the PDB: build-fast).
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ps2_host_profile.h"
#include "ps2_host_backend.h"
// native movie player (PS2Recomp/ps2xRuntime/src/lib/hctp_native_movie.cpp)
extern "C" bool hctpMovieOpen(int ch, const char *path, int fid, bool loop);
extern "C" void hctpMovieStop(int ch);
extern "C" void hctpMoviePause(int ch, bool pause);
extern "C" int hctpMovieStatus(int ch);
extern "C" bool hctpMovieFrame(int ch, uint8_t *rgba, int w, int h);
#include <initializer_list>
#include <cmath>
#include <mutex>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>
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
extern "C" void hctpCommentaryMove(uint32_t chara, uint32_t moveId);
extern "C" void hctpCommentaryParticipants(const uint32_t *rosterIds, uint32_t count);
extern "C" void hctpCommentaryArena(int stage);
extern "C" void hctpCommentaryCharaRoster(uint32_t chara, uint32_t rosterId);

namespace ps2_stubs
{
    void setLibSdrNative(uint32_t clientData, uint32_t commandBuffer); // Kernel/Stubs/Audio.cpp
}

namespace
{
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
        {0x20, 0, {0, 0, 0, 0, 0}, 0x2D, 3, "Hulk Hogan", 0},       // keeps its stats; Hogan / Hollywood / Mr. America
        {0x21, 0, {20, 13, 10, 13, 16}, 0x1D, 1, "Rosey", 3071},       // Jordan's recommended stats
        {0x23, 0, {18, 13, 14, 13, 15}, 0x1D, 1, "Jamal", 3071},       // Jordan's recommended stats
        {0x24, 0, {16, 13, 14, 15, 16}, 0x19, 1, "Billy Gunn", 3071},
        {0x29, 4, {8, 12, 14, 12, 8}, 0x0E, 1, "Molly Holly", 3084},
        {0x2B, 0, {10, 12, 17, 14, 13}, 0x17, 1, "Spike Dudley", 3071},
        {0x39, 0, {12, 13, 18, 15, 14}, 0x17, 1, "Billy Kidman", 3071},
        {0x4D, 0, {0, 0, 0, 0, 0}, 0x4C, 1, "Ultimate Warrior", 0}, // keeps its stats
        // Al Snow 0x3F left out: stock key 6300 is a Rock model (maybe the pads-off People's Elbow); needs his own key.
    };
    constexpr size_t kCutCount = sizeof(kCutRoster) / sizeof(kCutRoster[0]);
    bool g_cutRosterHasModel[kCutCount] = {};
    std::set<uint16_t> g_cutEntranceCodes; // decimal cast codes of restored entrance attires (e.g. 3204, 3214)
    // A slot is only un-hidden when its attire-1 model (key BCD(id) << 8) is in pac/CH.PAC (ids < 30) or pac/CH2.PAC
    // (DPAC directory at 0x800: tag, u16 count | 0x8000, then u16 key + u16 sectors per entry), so stock archives
    // keep the vanilla roster. Returns the number of slots enabled.
    int scanCutRosterModels()
    {
        int enabled = 0;
        for (size_t i = 0; i < kCutCount; ++i)
        {
            const uint8_t id = kCutRoster[i].id;
            std::ifstream f(id < 30 ? "pac/ch.pac" : "pac/ch2.pac", std::ios::binary);
            uint8_t dir[0x4000] = {};
            if (!f.read(reinterpret_cast<char *>(dir), sizeof(dir)))
                continue;
            uint16_t n;
            std::memcpy(&n, dir + 0x804, 2);
            n &= 0x7FFFu;
            const uint16_t key = static_cast<uint16_t>(((id / 10) << 12) | ((id % 10) << 8));
            for (uint32_t e = 0; e < n && 0x808u + e * 4u + 2u <= sizeof(dir); ++e)
            {
                uint16_t k;
                std::memcpy(&k, dir + 0x808 + e * 4, 2);
                if (k == key && !g_cutRosterHasModel[i])
                {
                    g_cutRosterHasModel[i] = true;
                    ++enabled;
                }
                // Hogan's 32A4 packs are gear-only leftovers (YOBJ = 9 parts: glasses/shirt/boa; no RTX3 skin sub 8,
                // 6 textures vs 28) - stock XXA4 packs (3904...) are full models. Drawn alone = floating gear, so Hogan
                // keeps his ring model until a merged body+gear pack exists (HCTP_HOGAN_GEAR=1 to test one).
                if ((k & 0xFF00u) == key && (k & 0xFu) == 4u && (id != 0x20 || std::getenv("HCTP_HOGAN_GEAR"))) // XXA4 -> cast code
                    g_cutEntranceCodes.insert(static_cast<uint16_t>(id * 100u + ((k >> 4) & 0xFu) * 10u + 4u));
            }
        }
        return enabled;
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
    PS2Runtime::RecompiledFunction g_origGetEntranceCastNo = nullptr;
    void getEntranceCastNoCutRoster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t code = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) & 0xFFFFu;
        g_origGetEntranceCastNo(rdram, ctx, runtime);
        const uint32_t entrance = code - code % 10u + 4u;
        if (code < 10000u && g_cutEntranceCodes.count(static_cast<uint16_t>(entrance)))
            SET_GPR_U32(ctx, 2, entrance);
    }
    // Shirt tear: CSd5EventData::InitEventSystem 0x2ad340 (a0 = this) calls SetDressCast(code - code%10 + 2) after the
    // entrance cast when member byte +0x155 is set (8 members of 0xd4 bytes from this+0x88, u16 cast code at +0).
    // Modern Hogan (0x20) never gets that byte, so his entrance gear stays on after the tear. Set it for him when the
    // after-tear pack /EMD2/32A2 is a loose file (MODS\PAC\EMD2\3202/3212/3222 = copies of his ring packs 32A0).
    PS2Runtime::RecompiledFunction g_origInitEventSystem = nullptr;
    void initEventSystemShirtTear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        for (uint32_t k = 0; self && k < 8; ++k)
        {
            uint8_t *const m = rdram + self + 0x88u + k * 0xD4u;
            uint16_t code;
            std::memcpy(&code, m, 2);
            if (code / 100u != 0x20u || code >= 10000u)
                continue;
            char key[8];
            std::snprintf(key, sizeof(key), "%04u", code - code % 10u + 2u);
            std::error_code ec;
            if (std::filesystem::exists(std::string("MODS/PAC/EMD2/") + key, ec) && !m[0x155 - 0x88])
            {
                m[0x155 - 0x88] = 1;
                std::fprintf(stderr, "[entrance] shirt tear on for cast %u (dress %s)%c", code, key, 10);
            }
        }
        g_origInitEventSystem(rdram, ctx, runtime);
    }
    void restoreCutRoster(uint8_t *rdram)
    {
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
    void getCharaGamePropCutRoster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        restoreCutRoster(rdram);
        g_origGetCharaGameProp(rdram, ctx, runtime);
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

    void readEndProcessSelectList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reader = gprU32(ctx, 4);
        g_origReadEndProcess(rdram, ctx, runtime);
        const uint32_t list = rdU32(rdram, reader + 0x20u);
        if (list == 0u || list >= 0x2000000u || g_readerLists.count(reader))
            return;
        std::vector<const CutWrestler *> extra;
        for (size_t i = 0; i < kCutCount; ++i)
            if (g_cutRosterHasModel[i])
                extra.push_back(&kCutRoster[i]);
        if (extra.empty())
            return;
        const uint32_t total = kStockSelect + static_cast<uint32_t>(extra.size());
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
        auto rl = g_readerLists.find(reader);
        if (ra < 0x24e410u || ra >= 0x24e810u || rl == g_readerLists.end() || g_selectCount > kSelectMax)
            return;
        uint32_t &buf = g_frameLists[dest];
        if (buf == 0u)
            buf = runtime->guestMalloc(kSelectMax * kSelectRec); // frames reuse addresses; buffer kept for reuse
        if (buf == 0u)
            return;
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
    // GetSelectRoster 0x26fb90 (leaf replacement)
    void getSelectRosterSelectList(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t ra = gprU32(ctx, 31);
        const bool longList = g_selectLongDepth > 0 || (ra >= 0x26fea0u && ra < 0x26ff1cu); // RosterID2IndexNo
        SET_GPR_U32(ctx, 2, longList ? g_selectCount : kStockSelect);
        ctx->pc = ra;
    }
    void installSelectList(PS2Runtime &runtime)
    {
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

    void returnValue(R5900Context *ctx, int32_t value)
    {
        ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }

    #include "hctp_media_routes.inl"

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
        std::lock_guard<std::mutex> lock(pl.m);
        int16_t *out = static_cast<int16_t *>(buffer);
        const uint32_t ch = pl.streamChannels ? pl.streamChannels : 2u;
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
            g_origAdxtStop(rdram, ctx, runtime);
            ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(adxt));
            ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(ra));
            ctx->pc = ra;
            std::lock_guard<std::mutex> alloc(g_adxAlloc);
            for (int i = 0; i < kAdxPlayers; ++i)
            {
                AdxPlayer &pl = g_adxPlayers[i];
                if (pl.adxt != 0u)
                    continue;
                std::lock_guard<std::mutex> lock(pl.m);
                char label[64];
                std::snprintf(label, sizeof(label), "%s %08x[%u]", indexed ? "StartMemIdx" : "StartMem", a1, indexed ? a2 : 0u);
                if (!adxSetup(pl, std::move(data), label))
                    break;
                adxPlayOn(i, adxt);
                if (g_nativeAdxLog)
                    std::fprintf(stderr, "[nadx] %s adxt=%x -> player %d%c", label, adxt, i, 10);
                return;
            }
            // no free player / not plain ADX: the library plays it (its arguments restored)
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(a1));
            ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(a2));
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
            // the library side must not keep playing an older stream on this handle
            g_origAdxtStop(rdram, ctx, runtime);
            ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(adxt));
            ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(ra));
            ctx->pc = ra;
            std::lock_guard<std::mutex> alloc(g_adxAlloc);
            for (int i = 0; i < kAdxPlayers; ++i)
            {
                AdxPlayer &pl = g_adxPlayers[i];
                if (pl.adxt != 0u)
                    continue;
                std::lock_guard<std::mutex> lock(pl.m);
                if (!adxLoad(pl, partition, fid))
                    break;
                adxPlayOn(i, adxt);
                if (g_nativeAdxLog)
                    std::fprintf(stderr, "[nadx] StartAfs adxt=%x pt=%u fid=%u -> player %d%c", adxt, partition, fid, i, 10);
                return;
            }
            std::fprintf(stderr, "[nadx] StartAfs adxt=%x pt=%u fid=%u: not played natively%c", adxt, partition, fid, 10);
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(partition));
            ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(fid));
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
            uint32_t ids[4];
            uint32_t n = 0;
            for (uint32_t slot = 0; slot < 4u; ++slot)
            {
                uint16_t code = 0;
                std::memcpy(&code, rdram + 0x4921CAu + slot * 0x20u, 2);
                if (code >= 25500u) // empty slot (cast code 0 is The Rock)
                    break;
                const uint32_t id = code >= 10000u ? (code - 10000u) / 100u + 0x56u : code / 100u;
                if (id < 0x56u)
                    ids[n++] = code < 10000u ? id | (code % 100u) << 8 : id; // + attire variant (nWo Hogan)
            }
            const int8_t stage = int8_t(rdram[0x4922C0u]); // GetNowStage 0x1f53d0 (arena / PPV)
            std::fprintf(stderr, "[hctp-pbp] arena (stage) %d%c", int(stage), 10);
            hctpCommentaryArena(stage);
            hctpCommentaryParticipants(ids, n);
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
        // the cast slot is only filled by now (at SetEvent time it can still hold 25500 = empty)
        uint16_t code = 0;
        std::memcpy(&code, rdram + 0x4921CAu + uint32_t(order) * 0x20u, 2);
        if (code >= 25500u && order > 0)
        {
            // later entrants: the entrance's cast list can hold the one walking out in slot 0 (seen: Undertaker as
            // entrance #1 with slot 1 still 25500); the engine ignores a repeat of an entrant already announced
            std::memcpy(&code, rdram + 0x4921CAu, 2);
            std::fprintf(stderr, "[hctp-pbp] entrance #%d: own cast slot empty, slot 0 = %u%c", order, code, 10);
        }
        if (code >= 25500u)
        {
            std::fprintf(stderr, "[hctp-pbp] entrance #%d starts: cast code %u = empty slot, silent%c", order, code, 10);
            return;
        }
        int roster = code >= 10000u ? int((code - 10000u) / 100u + 0x56u) : int(code / 100u | (code % 100u) << 8);
        // belt held (CTitleHistoryData::CheckTitleHold 0x324110): u16 holder ids at GetSaveData() 0x4B6450 + 0x9EFC,
        // 11 slots for 9 belts (belts 2 and 3 = tag titles, two holders each) -> belt + 1 above bit 16 for Finkel
        static const uint8_t kSlotBelt[11] = {0, 1, 2, 2, 3, 3, 4, 5, 6, 7, 8};
        int belt = -1;
        for (uint32_t s = 0; s < 11u && belt < 0; ++s)
        {
            uint16_t holder = 0;
            std::memcpy(&holder, rdram + 0x4C034Cu + s * 2u, 2);
            if (holder == uint16_t(roster & 0xFF))
                belt = kSlotBelt[s];
        }
        std::fprintf(stderr, "[hctp-pbp] entrance #%d starts: cast code %u -> roster 0x%02X attire %d belt %d%c", order, code,
                     roster & 0xFF, (roster >> 8) & 0xFF, belt, 10);
        if (belt >= 0)
            roster |= (belt + 1) << 16;
        hctpCommentaryEntrance(uint32_t(roster));
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
                hctpCommentaryCharaRoster(chara, cast >= 10000u ? (cast - 10000u) / 100u + 0x56u : cast / 100u);
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
    }
    void cfileSearchLoose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pathAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        char path[33] = {};
        for (int i = 0; i < 32 && rdram[pathAddr + i]; ++i)
            path[i] = static_cast<char>(std::toupper(rdram[pathAddr + i]));
        auto it = g_loosePac.find(path);
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
            for (char &c : name)
                if (c == '/' || c == '\\') c = sep;
            const int slot = g_loosePacNextSlot;
            if (name.size() >= 0x20 || slot < 32 || names[slot * 0x20] != 0)
            {
                std::fprintf(stderr, "[loosepac] %s: no free entry slot (name %s), stock file used%c", path, name.c_str(), 10);
                g_loosePac.erase(it);
                return g_origCFileSearch(rdram, ctx, runtime);
            }
            std::memcpy(names + slot * 0x20, name.c_str(), name.size() + 1);
            it->second.second = slot;
            --g_loosePacNextSlot;
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
    PS2Runtime::RecompiledFunction g_origGetEntranceEventNo = nullptr;
    void getEntranceEventNoPerAttire(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        const uint32_t order = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        g_origGetEntranceEventNo(rdram, ctx, runtime);
        if (order >= 8)
            return;
        uint32_t season; std::memcpy(&season, rdram + self + order * 0xD4u + 0x14Cu, 4);
        uint16_t code; std::memcpy(&code, rdram + 0x4921CAu + order * 0x20u, 2);
        if (season != 0 || code >= 10000u)
            return;
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
    // MODS\PAC\victory.txt "<n> <event 6200-6455>" re-points entries; HCTP_VICTORY_TEST=<event> plays that event for
    // every victory preview (to identify the unreferenced 62xx / 63xx events in CAM).
    std::map<uint32_t, uint32_t> g_victoryEvent; // n -> event
    PS2Runtime::RecompiledFunction g_origConvertDoramock = nullptr;
    void convertDoramockFromMoves(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t p = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0x1FFFFFFu;
        int32_t n; std::memcpy(&n, rdram + p, 4);
        g_origConvertDoramock(rdram, ctx, runtime);
        static const char *test = std::getenv("HCTP_VICTORY_TEST");
        auto it = g_victoryEvent.find(static_cast<uint32_t>(n));
        // HCTP_VICTORY_TEST=cycle: each preview plays the next unreferenced event (order below); the log names it.
        static const int32_t kOrphans[] = {6202, 6208, 6220, 6223, 6226, 6236, 6240, 6243, 6258, 6261, 6262, 6263, 6264,
                                           6265, 6266, 6267, 6268, 6269, 6270, 6271, 6272, 6273, 6274, 6275, 6284, 6285,
                                           6286, 6287, 6288, 6289, 6290, 6291, 6292, 6293, 6294, 6295, 6296, 6297, 6298,
                                           6299, 6300, 6301, 6314, 6315, 6316, 6317, 6318, 6319, 6320, 6321, 6322, 6323,
                                           6328, 6329, 6331, 6332, 6333, 6334, 6335, 6336, 6337, 6338, 6339, 6340, 6341,
                                           6342, 6344, 6345, 6346, 6347, 6348, 6349, 6350};
        static size_t cycle = 0;
        const bool cycling = test && std::strcmp(test, "cycle") == 0;
        int32_t ev = cycling ? kOrphans[cycle++ % (sizeof(kOrphans) / sizeof(kOrphans[0]))]
                     : test ? std::atoi(test) : it != g_victoryEvent.end() ? static_cast<int32_t>(it->second) : -1;
        if (ev < 0)
            return;
        std::memcpy(rdram + p, &ev, 4);
        std::fprintf(stderr, "[victory] list %d -> event %d%c", n, ev, 10);
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
            if ((!g_victoryEvent.empty() || std::getenv("HCTP_VICTORY_TEST")) &&
                (g_origConvertDoramock = runtime.lookupFunction(0x39dba0u)) != nullptr)
                runtime.replaceFunction(0x39dba0u, convertDoramockFromMoves);
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
        if (!g_attireEntrance.empty() && (g_origGetEntranceEventNo = runtime.lookupFunction(0x2ad1b0u)) != nullptr)
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
    PS2Runtime::RecompiledFunction g_mvOrigPlayAfs, g_mvOrigPlayName, g_mvOrigStop, g_mvOrigPause, g_mvOrigContinue,
        g_mvOrigGetStat, g_mvOrigLoadImage;

    uint32_t guestCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, std::initializer_list<uint32_t> args)
    {
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
        optW32(rdram, sfdInfo(ch) + 0xb4u, 2u);
        *optP(rdram, sfdInfo(ch) + 0x1e8u) = 0u;
    }
    bool nativePlayAfs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ch, uint32_t part, uint32_t fid, uint32_t loop)
    {
        static const char *files[] = {"MOVIE/NMV000.AFS", "MOVIE/NMV001.AFS", "MOVIE/NMV002.AFS", "MOVIE/NMV003.AFS", "MOVIE/LOGOS.AFS"};
        if (ch >= 4u || part < 5u || part > 9u)
            return false;
        hctpMovieStop(static_cast<int>(ch));
        makePath3Tag(rdram, ctx, runtime, ch);
        if (!hctpMovieOpen(static_cast<int>(ch), files[part - 5u], static_cast<int>(fid), loop != 0u))
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
        const uint32_t w = optR32(rdram, info + 8u), h = optR32(rdram, info + 0xcu), buf = optR32(rdram, info + 0xccu);
        const uint32_t mode = optR32(rdram, info + 0xb8u), gsWord = optR32(rdram, info + 0x84u);
        const uint32_t pkt = optR32(rdram, info + 0xbcu), usr = optR32(rdram, info + 0xc4u);
        const bool fresh = w && h && buf && hctpMovieFrame(static_cast<int>(ch), optP(rdram, buf), static_cast<int>(w), static_cast<int>(h));
        if (!fresh)
        {
            if (mode == 1u && *optP(rdram, info + 0x1e8u))
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
            *optP(rdram, info + 0x1e8u) = 1u;
        }
        else if (gsWord && pkt)
        {
            *optP(rdram, info + 0x1e8u) = 1u;
            guestCall(rdram, ctx, runtime, 0x159d70u, {buf, w, h, pkt, gsWord}); // LoadImagePath3
            const uint32_t dmaCh = guestCall(rdram, ctx, runtime, 0x102df0u, {2u}); // sceDmaGetChan(GIF)
            guestCall(rdram, ctx, runtime, 0x101770u, {0u, 0u});                  // sceGsSyncPath
            guestCall(rdram, ctx, runtime, 0x103120u, {dmaCh, pkt});              // sceDmaSend
        }
    }
    void installNativeMovies(PS2Runtime &runtime)
    {
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
        std::cerr << "[nmovie] native movies (FFmpeg) " << (ok ? "on" : "FAILED") << std::endl;
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

    void applyHctp(PS2Runtime &runtime)
    {
        installOptionRestore(runtime);
        if (std::getenv("HCTP_DTXLOG") && (g_dtxOrigExec = runtime.lookupFunction(0x3ca4f8u)) != nullptr)
            runtime.replaceFunction(0x3ca4f8u, dtxExecLogged);
        // CSR reads that test only FIELD (bit 13): yGsSyncV (vsync wait loop) and crisys_VSyncCallBack. Fenced, each
        // waited for the whole chain worker (7 + 4 s per 260 s demo run). Same exemption as Vendetta's 0x1A16C0.
        runtime.chain().allowUnfencedCsrRead(0x14c664u);
        runtime.chain().allowUnfencedCsrRead(0x15881cu);
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
        if (moviesMode == "native")
            installNativeMovies(runtime);
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
        if (!std::getenv("HCTP_NO_CUT_ROSTER"))
        {
            g_origGetCharaGameProp = runtime.lookupFunction(0x12cec0u);
            const int slots = scanCutRosterModels();
            const bool ok = slots == 0 || (g_origGetCharaGameProp &&
                                           runtime.replaceFunction(0x12cec0u, getCharaGamePropCutRoster));
            std::cerr << "[hctp] cut roster restore: " << slots << " slots with models"
                      << (ok ? "" : " (hook FAILED)") << std::endl;
            if (slots > 0)
                installSelectList(runtime);
            if (!g_cutEntranceCodes.empty() && (g_origGetEntranceCastNo = runtime.lookupFunction(0x2af480u)) != nullptr &&
                runtime.replaceFunction(0x2af480u, getEntranceCastNoCutRoster))
                std::cerr << "[hctp] cut roster entrance attires: " << g_cutEntranceCodes.size() << std::endl;
            if ((g_origInitEventSystem = runtime.lookupFunction(0x2ad340u)) != nullptr)
                runtime.replaceFunction(0x2ad340u, initEventSystemShirtTear);
        }
        if (!std::getenv("HCTP_INTERLACE_OFFSET"))
        {
            runtime.replaceFunction(0x102660u, gsSetHalfOffsetEven);
            runtime.replaceFunction(0x1028c8u, gsSetHalfOffsetEven);
            std::cerr << "[hctp] no-interlace half offset on" << std::endl;
        }
        installImportedMedia(runtime);
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



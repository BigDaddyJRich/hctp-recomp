// Included by hctp_overrides.cpp. NATIVE CSD5_SfxFileList::GetSfxInfo (Nov 13; HCTP_SFXLIST_NATIVE=0 = game's own).
// Crash it fixes: SvR six-man, tag exchange -> CMotionData::SetKey -> CAttribute::SetAttr -> PlaySE_FromAttribute
// 0x3ada80 -> play_msin 0x3ad8b0 -> GetSfxInfo 0x1ac6d0 read 0x061F01E1 (logs\shadowprobe_1112_jordan3_sfxcrash.err;
// llvm-symbolizer on the Nov 12 22:18 exe: +0x13d1 = runner line 254 = 0x1ac77c 'lh v0,0(v1)', the list-row walk).
// Code facts (runner MIPS, notes\SFX_CRASH_SIXMAN_20261112.md):
//  - SeNo2BankNo 0x1ac3f0 returns the u8 bank byte of the /SFX/LIST FE row; HCTP's own LIST has banks 1-8 AND 23/24/25
//    (135 ids: 0x258.., 0x2bc.., 0x3e8.., 0x44c..). GetSfxInfo only rejects bank <0, 5, 8, then reads the bank map
//    s32[10] at list+0x40+bank*4 -> bank 23..25 read +0x9C..+0xA4, past the 0x80-byte object (new(0x80) in CSound ctor
//    0x1ae2b8) = heap garbage as an FF-table index.
//  - The ctor 0x1ac1a0 calls SetScene(-1,-1), which only writes bankmap[0] = -1 (sltiu 0xF fails -> jr ra); SetScene
//    scene 8 (match, case 0x1ac56c) writes only banks 2/3/6/8, so banks 1/4/7/9 keep whatever an earlier scene (or the
//    uninitialised heap) left; bank 7 (entrance SEs 0xCB-0xD5) is only set by scenes 4/7 (entrances) -> entrances OFF
//    leaves it unset.
//  - A garbage index >= the FF table size (list+0x38 = 0x18, ReadEndProcessCdata 0x1ac270) reads a garbage {count,ptr}
//    and the row walk at 0x1ac77c faults.
// Fix (no cap): same function 1:1, but the bank map covers every u8 bank: banks 0..9 read the game's table, banks
// 10..255 read a host table (unmapped = -1, ready for added banks, NATIVE_SFX_HANDOFF plan step 3), and an index outside
// the game's own FF table size (+0x38) counts as unmapped (= what the -1 slot means) instead of a wild read.
namespace sfxlist
{
    PS2Runtime::RecompiledFunction g_orig = nullptr;
    int32_t g_hostBankMap[256];
    std::mutex g_logLock;
    std::set<uint64_t> g_logged;

    inline bool inRam(uint32_t a, uint32_t n) { return a >= 0x100000u && a < PS2_RAM_COMMITTED_TOP && n <= PS2_RAM_COMMITTED_TOP - a; }
    inline uint32_t r32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & PS2_RAM_COMMITTED_MASK), 4); return v; }
    inline int16_t r16(const uint8_t *rdram, uint32_t a) { int16_t v; std::memcpy(&v, rdram + (a & PS2_RAM_COMMITTED_MASK), 2); return v; }
    inline uint8_t r8(const uint8_t *rdram, uint32_t a) { return rdram[a & PS2_RAM_COMMITTED_MASK]; }

    void logOnce(uint64_t key, const char *what, int se, int bank, int32_t idx, uint32_t a, uint32_t b)
    {
        std::lock_guard<std::mutex> lock(g_logLock);
        if (!g_logged.insert(key).second)
            return;
        std::fprintf(stderr, "[sfxlist] %s: se %x bank %d idx %d (%08x %08x) -> no sound (was a wild read)%c", what, se, bank,
                     idx, a, b, 10);
    }

    // SeNo2BankNo 0x1ac3f0, 1:1.
    int seNo2BankNo(const uint8_t *rdram, uint32_t list, int se)
    {
        if (r8(rdram, list + 0x78u) == 0u)
            return 0;
        int bank = -1;
        const uint32_t fe = r32(rdram, list + 0x30u);
        const int32_t n = static_cast<int32_t>(r32(rdram, list + 0x34u));
        for (int32_t i = 0; i < n; ++i)
            if (r16(rdram, fe + static_cast<uint32_t>(i) * 4u) == se)
            {
                bank = r8(rdram, fe + static_cast<uint32_t>(i) * 4u + 2u);
                break;
            }
        if (static_cast<int32_t>(r32(rdram, 0x411A40u)) == 6) // GetNowSoundEnv 0x1ae9c0
            return (bank == 1 || bank == 6 || bank == 8) ? bank : 4;
        return bank;
    }

    // GetSfxInfo(list a0, se a1, variant a2, tagSFX_MSIN_INFO *a3) 0x1ac6d0
    void getSfxInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t list = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        const int se = static_cast<int32_t>(_mm_cvtsi128_si32(ctx->r[5]));
        const int variant = static_cast<int32_t>(_mm_cvtsi128_si32(ctx->r[6]));
        const uint32_t info = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[7]));
        int32_t ret = 0;
        do
        {
            if (!inRam(list, 0x80u) || r8(rdram, list + 0x78u) == 0u)
                break;
            const int bank = seNo2BankNo(rdram, list, se);
            if (bank < 0 || bank == 5 || bank == 8)
                break;
            const int32_t idx = bank < 10 ? static_cast<int32_t>(r32(rdram, list + 0x40u + static_cast<uint32_t>(bank) * 4u))
                                          : g_hostBankMap[bank & 0xFF];
            if (idx < 0)
                break;
            const int32_t cap = static_cast<int32_t>(r32(rdram, list + 0x38u));
            if (idx >= cap)
            {
                logOnce((uint64_t(bank) << 32) | uint32_t(idx), "unset bank-map slot", se, bank, idx, uint32_t(cap), 0u);
                break;
            }
            const uint32_t entry = r32(rdram, list + 0x3Cu) + static_cast<uint32_t>(idx) * 8u;
            const int32_t count = static_cast<int32_t>(r32(rdram, entry));
            const uint32_t tbl = r32(rdram, entry + 4u);
            if (count > 0 && !inRam(tbl, static_cast<uint32_t>(count) * 8u))
            {
                logOnce((uint64_t(0x100u + bank) << 32) | uint32_t(idx), "bad FF row table", se, bank, idx, uint32_t(count), tbl);
                break;
            }
            int32_t found = -1;
            for (int32_t i = count - 1; i >= 0; --i)
            {
                const uint32_t row = tbl + static_cast<uint32_t>(i) * 8u;
                if (r16(rdram, row) != se)
                    continue;
                const uint8_t v = r8(rdram, row + 2u);
                if (v == 0u || static_cast<int>(v) == variant)
                {
                    found = i;
                    break;
                }
            }
            if (found < 0 || !inRam(info, 8u))
                break;
            std::memcpy(rdram + (info & PS2_RAM_COMMITTED_MASK), rdram + ((tbl + static_cast<uint32_t>(found) * 8u) & PS2_RAM_COMMITTED_MASK), 8);
            rdram[(info + 7u) & PS2_RAM_COMMITTED_MASK] = static_cast<uint8_t>(bank);
            ret = 1;
        } while (false);
        SET_GPR_S32(ctx, 2, ret);
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("HCTP_SFXLIST_NATIVE"); e && e[0] == '0')
            return;
        for (int32_t &v : g_hostBankMap)
            v = -1;
        g_orig = runtime.lookupFunction(0x1ac6d0u);
        if (!g_orig || !runtime.replaceFunction(0x1ac6d0u, getSfxInfo))
            std::fprintf(stderr, "[sfxlist] GetSfxInfo hook FAILED%c", 10);
    }
}

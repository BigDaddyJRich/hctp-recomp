// Included by hctp_overrides.cpp. NATIVE SOUND KIT (HCTP_NATIVE_SK=1): the Sony Standard Kit's sound-effect path -
// skmsin (IOP) -> modhsyn (IOP) -> libsd -> SPU2 - as host C++. Everything here follows the game's own IOP code as
// Ghidra decompiles it (notes\ghidra\modhsyn_voice_path.txt, skmsin_all.txt, libsd note->pitch tables), not a
// generic synth: the bank walk Prog -> tone -> Sset -> Smpl -> Vagi, the volume / pan / pitch / ADSR maths and the
// SPU2 register writes of modhsyn's key-on (FUN_00052970). The registers go to the IOP's SPU2 through
// ps2x::iop::spu2QueueWrite; the IOP's own modhsyn no longer gets the effect banks' commands.
// Effect banks only (CSkCtrl::BindMsinBank); MIDI-sequence banks (BindMidiBank) stay on the IOP until the sequencer
// is ported. Not yet: the LFOs (vibrato / tremolo, FUN_00057e34 - their depth is 0 in the effect banks seen so far),
// portamento, sustain pedal, noise voices, the user velocity tables (the HD's 5th chunk, absent in these banks).
#include <ps2x/iop/iop_native_hooks.h>

namespace hctpsk
{
    // ---- libsd sceSdNote2Pitch (IOP 0x4ad90) and its tables (notes\ghidra\libsd_note2pitch_tables.txt)
    const uint16_t kSemitone[12] = {0x8000, 0x879c, 0x8fac, 0x9837, 0xa145, 0xaadc, 0xb504, 0xbfc8, 0xcb2f, 0xd744, 0xe411, 0xf1a1};
    uint16_t g_fine[128];
    bool g_fineInit = false;

    uint32_t note2Pitch(int centerNote, uint32_t centerFine, int note, int16_t fine)
    {
        // a literal port of the decompile (libsd 0x4ad90)
        int iVar7 = static_cast<int>(fine) + static_cast<int>(centerFine & 0xffffu);
        int iVar4 = iVar7 < 0 ? iVar7 + 0x7f : iVar7;
        iVar4 >>= 7;
        int iVar5 = static_cast<int16_t>((note + iVar4) - centerNote);
        iVar7 = iVar7 + iVar4 * -0x80;
        const int16_t sVar2 = static_cast<int16_t>(iVar5 / 12);
        int16_t sVar8 = static_cast<int16_t>(sVar2 - 2);
        iVar5 = iVar5 % 12;
        int iVar3 = static_cast<int16_t>(iVar5);
        bool direct = false;
        if (iVar3 < 0)
        {
            iVar5 += 12;
            sVar8 = static_cast<int16_t>(sVar2 - 3);
        }
        else if (iVar3 == 0)
        {
            if (iVar7 >= 0)
            {
                iVar3 = iVar5 * 0x10000;
                direct = true;
            }
            else
            {
                iVar5 += 12;
                sVar8 = static_cast<int16_t>(sVar2 - 3);
            }
        }
        if (!direct)
        {
            iVar3 = iVar5 << 16;
            if (iVar7 < 0)
            {
                iVar7 = iVar7 + (iVar4 + 1) * 0x80;
                iVar3 = (iVar5 - 1 + iVar4) * 0x10000;
            }
        }
        const int semi = iVar3 >> 16;
        if (semi < 0 || semi > 11 || iVar7 < 0 || iVar7 > 127)
            return 0; // outside the tables (the IOP would read neighbouring data)
        uint32_t v = (static_cast<uint32_t>(kSemitone[semi]) * g_fine[iVar7]) >> 16;
        if (sVar8 < 0)
        {
            const uint32_t sh = static_cast<uint32_t>(-sVar8);
            v = (v + (1u << (sh - 1u))) >> sh;
        }
        return v & 0xffffu;
    }

    // ---- a captured bank (overrides' TransToIop capture): Sony HD chunks, as modhsyn's bind checks them
    struct Bank
    {
        std::vector<uint8_t> hd;
        uint32_t spuAddr = 0; // bytes
        uint32_t prog = 0, sset = 0, smpl = 0, vagi = 0;
        bool valid = false;
        uint8_t u8(uint32_t o) const { return o < hd.size() ? hd[o] : 0; }
        int8_t s8(uint32_t o) const { return static_cast<int8_t>(u8(o)); }
        uint16_t u16(uint32_t o) const { return static_cast<uint16_t>(u8(o) | (u8(o + 1) << 8)); }
        uint32_t u32(uint32_t o) const { return u16(o) | (static_cast<uint32_t>(u16(o + 2)) << 16); }
        // entry idx of a chunk at chunkOff: (max index at +0xc, offsets at +0x10, -1 = none) -> offset or 0
        uint32_t entry(uint32_t chunk, uint32_t idx) const
        {
            if (!chunk || idx > u32(chunk + 0xc))
                return 0;
            const uint32_t off = u32(chunk + 0x10 + idx * 4);
            return off == 0xffffffffu ? 0 : chunk + off;
        }
        void parse()
        {
            valid = hd.size() > 0x40 && std::memcmp(hd.data(), "IECSsreV", 8) == 0 && std::memcmp(hd.data() + 0x10, "IECSdaeH", 8) == 0;
            if (!valid)
                return;
            prog = u32(0x24);
            sset = u32(0x10 + 0x18);
            smpl = u32(0x10 + 0x1c);
            vagi = u32(0x10 + 0x20);
            auto chk = [&](uint32_t &c, const char *tag) { if (c == 0xffffffffu || c + 16 > hd.size() || std::memcmp(hd.data() + c, tag, 8) != 0) c = 0; };
            chk(prog, "IECSgorP");
            chk(sset, "IECStesS");
            chk(smpl, "IECSlpmS");
            chk(vagi, "IECSigaV");
            valid = prog && sset && smpl && vagi;
        }
    };

    struct Channel
    {
        uint8_t bankSlot = 0, program = 0;
        uint8_t volume = 0x80, expression = 0x80; // CC7 / CC11 (x128/127)
        int8_t pan = 0;                          // CC10 - 0x40
        uint8_t panOverride = 0xff, volOverride = 0xff; // F9 01 / F9 00
        int16_t bend = 0;
    };

    struct Voice
    {
        bool active = false, released = false;
        int track = -1, channel = 0;
        uint8_t key = 0, vel = 0, id = 0xff;
        uint64_t serial = 0;
        uint16_t envSeen = 0;
        int core = 0, index = 0;
        // for later controls (bend / expression / pan by id)
        uint32_t prog = 0, tone = 0, smpl = 0, vagi = 0;
        // volume inputs (FUN_0005326c / 53064): v16 (bank x velocity), channel volume +0x2b, +0x54, pan gains
        uint32_t v16 = 0, gl = 0x80, gr = 0x80;
        uint8_t chVolume = 0x80, vol54 = 0x80;
        // pitch inputs (FUN_00052e08): note / fine before the bend, center note, rate scale, bend ranges, +0x34
        int note = 0, fine = 0, bend34 = 0;
        uint8_t center = 0;
        uint32_t scale = 0;
        uint16_t rangeDown = 0, rangeUp = 0;
    };

    struct Track
    {
        int bank = -1; // index into g_banks
        Channel ch[16];
    };

    std::mutex g_lock;
    std::vector<Bank> g_banks;
    std::vector<Track> g_tracks;
    Voice g_voices[2][24];
    uint64_t g_serial = 0;
    uint32_t g_mix[4][2] = {}; // VMIXL, VMIXEL, VMIXR, VMIXER per core (24-bit masks)
    constexpr int32_t kHandleBase = 0x5000; // native handles (banks / tracks / msin ports) start here

    void wr(uint32_t off, uint16_t v) { ps2x::iop::spu2QueueWrite(off, v); }
    void writeMask(int core, int which, uint32_t mask)
    {
        static const uint32_t reg[4] = {0x188u, 0x18Cu, 0x190u, 0x194u};
        const uint32_t base = static_cast<uint32_t>(core) * 0x400u + reg[which];
        wr(base, static_cast<uint16_t>(mask & 0xffffu));
        wr(base + 2u, static_cast<uint16_t>((mask >> 16) & 0xffu));
    }

    uint32_t voiceBase(const Voice &v) { return static_cast<uint32_t>(v.core) * 0x400u + static_cast<uint32_t>(v.index) * 0x10u; }

    // FUN_0005326c + FUN_00053064: VOLL / VOLR (the track master is 0x100 = unity, the LFO terms are 0)
    void writeVolume(const Voice &v)
    {
        const uint32_t v18 = (static_cast<uint32_t>(v.chVolume) * v.vol54 * v.v16) >> 14;
        const uint32_t l = std::min<uint32_t>((v18 * v.gl) >> 7, 0x3fff), r = std::min<uint32_t>((v18 * v.gr) >> 7, 0x3fff);
        wr(voiceBase(v) + 0x0u, static_cast<uint16_t>(l));
        wr(voiceBase(v) + 0x2u, static_cast<uint16_t>(r));
    }

    uint32_t note2Pitch(int centerNote, uint32_t centerFine, int note, int16_t fine);

    // FUN_00052e08: PITCH from the note / fine, + the bend term +0x34, scaled by the sample rate / 48 kHz
    void writePitch(const Voice &v)
    {
        int note = v.note, fine = v.fine + v.bend34;
        if (fine < 0)
        {
            const int carry = (fine + 0x7f) >> 7;
            note += carry;
            fine -= carry * 0x80;
            if (fine != 0)
            {
                fine += 0x80;
                note -= 1;
            }
        }
        const uint32_t pitch = std::min<uint32_t>((note2Pitch(v.center, 0, note, static_cast<int16_t>(fine)) * v.scale) >> 16, 0x3fff);
        wr(voiceBase(v) + 0x4u, static_cast<uint16_t>(pitch));
    }

    // FUN_00053010: +0x34 = bend * range >> 13 (the tone's +6 below center, +8 above)
    void setBend(Voice &v, int bend)
    {
        const int range = bend < 0 ? v.rangeDown : v.rangeUp;
        int t = range * bend;
        if (t < 0)
            t += 0x1fff;
        v.bend34 = t >> 13;
    }

    // ---- velocity curve (FUN_00057a64; table index from the Sset's first byte, curves FUN_00057984..57a3c)
    uint8_t sq(uint8_t v)
    {
        const uint32_t u2 = static_cast<uint32_t>(v) * v, u1 = u2 / 0x7f;
        const uint32_t r = (u1 + ((u2 - u1) >> 1)) >> 6;
        return r ? static_cast<uint8_t>(r) : 1u;
    }
    uint8_t velCurve(uint8_t vel, uint8_t mode)
    {
        if (vel == 0)
            vel = 1;
        uint32_t f = mode & 0xf;
        if (f > 10)
            f = 0;
        const uint8_t inv = static_cast<uint8_t>(-0x80 - static_cast<int8_t>(vel));
        switch (f % 6) // the table repeats 0..5 at 6..9
        {
        case 0: return vel;
        case 1: return inv;
        case 2: return sq(vel);
        case 3: return static_cast<uint8_t>(-0x80 - static_cast<int8_t>(sq(vel)));
        case 4: return static_cast<uint8_t>(-0x80 - static_cast<int8_t>(sq(inv)));
        default: return sq(inv);
        }
    }

    // FUN_0005240c: a bit field of an ADSR word moved by (follow * rate / 12), clamped to the field
    uint16_t adsrFollow(uint32_t word, int8_t rate, int follow, uint32_t mask, uint32_t shift)
    {
        const int d = (follow * rate) / 12;
        if (d == 0)
            return static_cast<uint16_t>(word);
        int v = static_cast<int>((static_cast<int>(word & 0xffff) >> shift) & mask) + d;
        v = std::clamp(v, 0, static_cast<int>(mask));
        return static_cast<uint16_t>((word & ~(mask << shift)) | (static_cast<uint32_t>(v) << shift));
    }

    void initTables()
    {
        if (g_fineInit)
            return;
        g_fineInit = true;
        static const uint16_t fine[128] = { // libsd 0x4c688, copied from the IOP RAM dump
            0x8000, 0x800e, 0x801d, 0x802c, 0x803b, 0x804a, 0x8058, 0x8067, 0x8076, 0x8085, 0x8094, 0x80a3, 0x80b1, 0x80c0, 0x80cf, 0x80de,
            0x80ed, 0x80fc, 0x810b, 0x811a, 0x8129, 0x8138, 0x8146, 0x8155, 0x8164, 0x8173, 0x8182, 0x8191, 0x81a0, 0x81af, 0x81be, 0x81cd,
            0x81dc, 0x81eb, 0x81fa, 0x8209, 0x8218, 0x8227, 0x8236, 0x8245, 0x8254, 0x8263, 0x8272, 0x8282, 0x8291, 0x82a0, 0x82af, 0x82be,
            0x82cd, 0x82dc, 0x82eb, 0x82fa, 0x830a, 0x8319, 0x8328, 0x8337, 0x8346, 0x8355, 0x8364, 0x8374, 0x8383, 0x8392, 0x83a1, 0x83b0,
            0x83c0, 0x83cf, 0x83de, 0x83ed, 0x83fd, 0x840c, 0x841b, 0x842a, 0x843a, 0x8449, 0x8458, 0x8468, 0x8477, 0x8486, 0x8495, 0x84a5,
            0x84b4, 0x84c3, 0x84d3, 0x84e2, 0x84f1, 0x8501, 0x8510, 0x8520, 0x852f, 0x853e, 0x854e, 0x855d, 0x856d, 0x857c, 0x858b, 0x859b,
            0x85aa, 0x85ba, 0x85c9, 0x85d9, 0x85e8, 0x85f8, 0x8607, 0x8617, 0x8626, 0x8636, 0x8645, 0x8655, 0x8664, 0x8674, 0x8683, 0x8693,
            0x86a2, 0x86b2, 0x86c1, 0x86d1, 0x86e0, 0x86f0, 0x8700, 0x870f, 0x871f, 0x872e, 0x873e, 0x874e, 0x875d, 0x876d, 0x877d, 0x878c};
        std::memcpy(g_fine, fine, sizeof(fine));
    }

    Track *track(int handle)
    {
        const int i = handle - kHandleBase;
        return (i >= 0 && i < static_cast<int>(g_tracks.size())) ? &g_tracks[static_cast<size_t>(i)] : nullptr;
    }

    // voice allocation (FUN_0005558c: core from Smpl +0x29 bits 0x30; else the core with more free voices), then
    // the oldest released / oldest voice of that core (modhsyn's steal order, FUN_00055630 / 55834, simplified)
    Voice *allocVoice(uint8_t flags29, int &coreOut)
    {
        int freeCount[2] = {0, 0};
        for (int c = 0; c < 2; ++c)
            for (auto &v : g_voices[c])
                freeCount[c] += !v.active;
        int core = (flags29 & 0x30) == 0x10 ? 0 : (flags29 & 0x30) == 0x20 ? 1 : (freeCount[0] < freeCount[1] ? 1 : 0);
        Voice *best = nullptr;
        for (auto &v : g_voices[core])
            if (!v.active)
            {
                best = &v;
                break;
            }
        if (!best)
            for (auto &v : g_voices[core])
                if (!best || (v.released && !best->released) || (v.released == best->released && v.serial < best->serial))
                    best = &v;
        coreOut = core;
        return best;
    }

    // modhsyn note-on (FUN_00054fdc) + voice setup (FUN_000525b4) + key-on (FUN_00052970)
    void noteOn(int handle, int ch, uint8_t key, uint8_t vel, uint8_t id)
    {
        Track *t = track(handle);
        if (!t || t->bank < 0 || vel == 0)
            return;
        const Bank &b = g_banks[static_cast<size_t>(t->bank)];
        Channel &c = t->ch[ch & 15];
        const uint32_t prog = b.entry(b.prog, c.program);
        if (!prog || b.u8(prog + 4) == 0)
            return;
        const uint32_t toneTable = prog + b.u32(prog);
        const uint32_t tones = b.u8(prog + 4);
        uint32_t keyOnMask[2] = {0, 0};
        for (uint32_t ti = 0; ti < tones; ++ti)
        {
            const uint32_t tone = toneTable + ti * 20u;
            if (!((b.u8(tone + 2) & 0x7f) <= key && key <= (b.u8(tone + 4) & 0x7f)))
                continue;
            const uint32_t sset = b.entry(b.sset, b.u16(tone));
            if (!sset || !(b.u8(sset + 1) <= vel && vel <= b.u8(sset + 2)) || b.u8(sset + 3) == 0)
                continue;
            for (uint32_t si = 0; si < b.u8(sset + 3); ++si)
            {
                const uint32_t smpl = b.entry(b.smpl, b.u16(sset + 4 + si * 2));
                if (!smpl || !((b.u8(smpl + 2) & 0x7f) <= vel && vel <= (b.u8(smpl + 4) & 0x7f)))
                    continue;
                const uint32_t vagi = b.entry(b.vagi, b.u16(smpl));
                if (!vagi || b.u32(vagi) == 0xffffffffu)
                    continue; // (noise voices: not yet)
                int core = 0;
                Voice *v = allocVoice(b.u8(smpl + 0x29), core);
                if (!v)
                    continue;
                const int vi = static_cast<int>(v - g_voices[core]);
                const uint32_t vbase = static_cast<uint32_t>(core) * 0x400u + static_cast<uint32_t>(vi) * 0x10u;
                if (v->active) // stolen: key off first
                    wr(static_cast<uint32_t>(core) * 0x400u + (vi < 16 ? 0x1A4u : 0x1A6u), static_cast<uint16_t>(1u << (vi & 15)));
                *v = Voice{};
                v->active = true;
                v->track = handle;
                v->channel = ch & 15;
                v->key = key;
                v->vel = vel;
                v->id = id;
                v->serial = ++g_serial;
                v->core = core;
                v->index = vi;
                v->prog = prog;
                v->tone = tone;
                v->smpl = smpl;
                v->vagi = vagi;

                // volume (FUN_000525b4 + FUN_0005326c + FUN_00053064): prog[6] * tone[0x10] * smpl[0x10] * velocity
                const uint32_t curve = velCurve(vel, b.u8(sset));
                const uint32_t v16 = ((static_cast<uint32_t>(b.u8(prog + 6)) * b.u8(tone + 0x10) * b.u8(smpl + 0x10) * curve) >> 14);
                // pan (FUN_00053460 / FUN_00053534): |prog[7]| - 0x80 + |tone[0x11]| + |smpl[0xd]| + key follow, + pan
                const int follow = ((key - b.u8(tone + 0xf)) * b.s8(tone + 0xe) + (key - b.u8(prog + 0xb)) * b.s8(prog + 0xa)) / 12;
                int base = std::abs(static_cast<int>(b.s8(prog + 7))) - 0x80 + std::abs(static_cast<int>(b.s8(tone + 0x11))) +
                           std::abs(static_cast<int>(b.s8(smpl + 0xd))) + follow;
                base = std::clamp(base, 0, 0x7f);
                int p = (c.panOverride & 0x80) == 0 ? static_cast<int>(c.panOverride) - 0x40 : c.pan;
                p = std::clamp(p, -0x3f, 0x3f);
                const int pos = p + base - 0x40;
                uint32_t gl = 0x80, gr = 0x80;
                if (pos > 0)
                    gl = pos < 0x3f ? static_cast<uint32_t>(((0x3f - pos) * 0x80) / 0x3f) : 0u;
                else if (pos < 0)
                    gr = pos < -0x3e ? 0u : static_cast<uint32_t>(((p + base - 1) * 0x80) / 0x3f);
                v->v16 = v16;
                v->chVolume = c.volume;
                v->vol54 = c.volOverride != 0xff ? c.volOverride : c.expression;
                v->gl = gl;
                v->gr = gr;
                writeVolume(*v);

                // pitch (FUN_00052e08 with libsd Note2Pitch): bend * range >> 13 (tone +6 / +8), the tone / program
                // transposes and fine tunes, scaled by the sample rate / 48 kHz
                v->rangeDown = b.u16(tone + 6);
                v->rangeUp = b.u16(tone + 8);
                setBend(*v, c.bend);
                v->note = key + b.s8(prog + 8) + b.s8(tone + 0x12);
                v->fine = b.s8(prog + 9) + b.s8(smpl + 0xc) + b.s8(tone + 0x13);
                v->center = b.u8(smpl + 0xb);
                v->scale = static_cast<uint32_t>((static_cast<uint64_t>(b.u16(vagi + 4)) * 0x10000u) / 48000u);
                writePitch(*v);

                // ADSR (FUN_00052494 / FUN_00052538): the Smpl's words with key follow
                uint16_t a1 = b.u16(smpl + 0x12), a2 = b.u16(smpl + 0x14);
                if (b.s8(smpl + 0x16)) a1 = adsrFollow(a1, b.s8(smpl + 0x16), key - b.u8(smpl + 0x17), 0x7f, 8);
                if (b.s8(smpl + 0x18)) a1 = adsrFollow(a1, b.s8(smpl + 0x18), key - b.u8(smpl + 0x19), 0xf, 4);
                if (b.s8(smpl + 0x1e)) a1 = adsrFollow(a1, b.s8(smpl + 0x1e), key - b.u8(smpl + 0x1f), 0xf, 0);
                if (b.s8(smpl + 0x1a)) a2 = adsrFollow(a2, b.s8(smpl + 0x1a), key - b.u8(smpl + 0x1b), 0x7f, 6);
                if (b.s8(smpl + 0x1c)) a2 = adsrFollow(a2, b.s8(smpl + 0x1c), key - b.u8(smpl + 0x1d), 0x1f, 0);
                wr(vbase + 0x6u, a1);
                wr(vbase + 0x8u, a2);

                // start address (sceSdSetAddr 0x2040: bytes -> halfword address, hi 6 bits / lo 16)
                const uint32_t ssa = (b.spuAddr + b.u32(vagi)) >> 1;
                const uint32_t abase = static_cast<uint32_t>(core) * 0x400u + 0x1C0u + static_cast<uint32_t>(vi) * 12u;
                wr(abase + 0u, static_cast<uint16_t>((ssa >> 16) & 0x3f));
                wr(abase + 2u, static_cast<uint16_t>(ssa & 0xffff));

                // dry / wet routing (Smpl +0x29: 1 dry L, 2 dry R, 4 wet L, 8 wet R)
                const uint8_t f = b.u8(smpl + 0x29);
                const uint32_t bit = 1u << vi;
                const uint32_t want[4] = {(f & 1u) ? bit : 0u, (f & 4u) ? bit : 0u, (f & 2u) ? bit : 0u, (f & 8u) ? bit : 0u};
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t m = (g_mix[k][core] & ~bit) | want[k];
                    if (m != g_mix[k][core])
                    {
                        g_mix[k][core] = m;
                        writeMask(core, k, m);
                    }
                }
                keyOnMask[core] |= bit;
            }
        }
        for (int core = 0; core < 2; ++core)
            if (keyOnMask[core])
            {
                wr(static_cast<uint32_t>(core) * 0x400u + 0x1A0u, static_cast<uint16_t>(keyOnMask[core] & 0xffff));
                wr(static_cast<uint32_t>(core) * 0x400u + 0x1A2u, static_cast<uint16_t>(keyOnMask[core] >> 16));
            }
    }

    // note-off by key (and id, 0xff = any) (FUN_000546d4: KOFF)
    void noteOff(int handle, uint8_t key, uint8_t id, bool all)
    {
        uint32_t off[2] = {0, 0};
        for (int core = 0; core < 2; ++core)
            for (auto &v : g_voices[core])
                if (v.active && !v.released && v.track == handle && (all || (v.key == key && (id == 0xff || v.id == id))))
                {
                    v.released = true;
                    off[core] |= 1u << v.index;
                }
        for (int core = 0; core < 2; ++core)
            if (off[core])
            {
                wr(static_cast<uint32_t>(core) * 0x400u + 0x1A4u, static_cast<uint16_t>(off[core] & 0xffff));
                wr(static_cast<uint32_t>(core) * 0x400u + 0x1A6u, static_cast<uint16_t>(off[core] >> 16));
            }
    }

    // voices that ended (ENVX 0 after the key-off / the sample end) are free again
    void reap()
    {
        const ps2x::iop::Spu2Snapshot snap = ps2x::iop::spu2Snapshot();
        static uint64_t s_serial = 0;
        if (snap.serial == s_serial)
            return;
        s_serial = snap.serial;
        for (int core = 0; core < 2; ++core)
            for (auto &v : g_voices[core])
                if (v.active && snap.envx[core][v.index] == 0 && (v.released || (snap.endx[core] & (1u << v.index))))
                    v.active = false;
    }

    bool idBusy(int handle, uint8_t id)
    {
        reap();
        for (int core = 0; core < 2; ++core)
            for (auto &v : g_voices[core])
                if (v.active && !v.released && v.track == handle && v.id == id)
                    return true;
        return false;
    }

    // skmsin's controls on a native track (the MIDI it would send to modhsyn, applied directly)
    uint32_t msinControl(int handle, uint32_t msg, const uint32_t *a)
    {
        static int s_dbg = 0; // TEMP diag: the first controls
        if (s_dbg < 60 && std::getenv("HCTP_SKDBG"))
        {
            ++s_dbg;
            std::fprintf(stderr, "[skdbg] h=0x%x msg=0x%x a=%x %x %x %x %x %x %x%c", handle, msg, a[0], a[1], a[2], a[3], a[4], a[5], a[6], 10);
        }
        Track *t = track(handle);
        if (!t)
            return 0xffffff9bu;
        reap();
        switch (msg)
        {
        case 0x10: case 0x11: case 0x12: case 0x13:
        {
            // 0x11/0x13 carry an id first: (id, bank, prog, ch, key, vel, pan); 0x10/0x12 without
            const bool withId = msg == 0x11 || msg == 0x13;
            const uint8_t id = withId ? static_cast<uint8_t>(a[0]) : 0xff;
            const uint32_t *p = withId ? a + 1 : a;
            const int ch = static_cast<int>(p[2] & 0xf);
            Channel &c = t->ch[ch];
            if (msg == 0x10 || msg == 0x11)
            {
                c.bankSlot = static_cast<uint8_t>(p[0] & 0x7f);
                c.program = static_cast<uint8_t>(p[1] & 0x7f);
                c.panOverride = static_cast<uint8_t>(p[5] & 0x7f);
                noteOn(handle, ch, static_cast<uint8_t>(p[3] & 0x7f), static_cast<uint8_t>(p[4] & 0x7f), id);
            }
            else
                noteOff(handle, static_cast<uint8_t>(p[3] & 0x7f), id, false);
            return id;
        }
        case 0x40: case 0x41: case 0x42:
        {
            // FD 01 / 02 / 00 ch id key value: per id (0x7f = all) and key (bit 7 = any). 0x42 expression
            // (FUN_0004f890: +0x54 = v*128/127, volume again), 0x41 bend (FUN_0004f9ac: 14-bit, pitch again);
            // 0x40 (FUN_0004f774, per-id pan): TODO
            const uint8_t id = static_cast<uint8_t>(a[0]), key = static_cast<uint8_t>(a[2]);
            for (int core = 0; core < 2; ++core)
                for (auto &v : g_voices[core])
                {
                    if (!v.active || v.track != handle || !(id == 0x7f || v.id == id) || !((key & 0x80) || v.key == key))
                        continue;
                    if (msg == 0x42)
                    {
                        v.vol54 = static_cast<uint8_t>(((a[3] & 0xffu) * 0x80u) / 0x7fu);
                        writeVolume(v);
                    }
                    else if (msg == 0x41)
                    {
                        setBend(v, static_cast<int>(a[3] & 0x3fff) - 0x2000);
                        writePitch(v);
                    }
                }
            return a[0] & 0xff;
        }
        default:
            return 0xfffffecau;
        }
    }
}

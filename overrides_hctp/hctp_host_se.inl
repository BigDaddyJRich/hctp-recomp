// Included by hctp_overrides.cpp. HOST PYRO SE (Nov 11, HCTP_HOST_SE=0 off; HCTP_HOST_SE_VOL=<float> gain, default 1).
// Ported SvR04 entrances fire their bank-7 pyro SEs (C9, CB-CF, D3-D8, DA) through the event C0 op ->
// PlaySE_FromAttribute 0x3ada80 -> SeNo2BankNo / GetSfxInfo on HCTP's /SFX/LIST -> HCTP's own BK7 programs (wrong
// sounds, quiet) or nothing (D7: no HCTP LIST row). Measured (PART7 note): the SvR04 BK7 samples (380,928 B, all 13 used)
// do not fit SPU RAM in the CAM sound env (ySpu2MemAllocate 0x2a5360 is a bump allocator, HCTP banks end at 0x172DD0 of
// 0x1C0000 -> ~316 KB free), so no extra SK bank: while a ported event of that source game plays, those ids are
// played HERE from the source game's own bank (MODS\<GAME>\SE\bk7.hd/.bd + se_map.txt, tools\stage_svr_pyro_se_1111.py)
// on one host stream. Bank-7 SEs are not positional in HCTP either (play_msin keeps volume 0x7F for every bank but 2).
// The HD walk (Prog -> tone -> Sset -> Smpl -> Vagi) and volume / pitch fields follow hctp_native_sk.inl (modhsyn).
namespace hostse
{
    struct Sample
    {
        std::vector<int16_t> pcm;
        uint32_t loopStart = 0;
        bool loops = false;
        float step = 1.0f; // source samples per 48 kHz output sample (rate x pitch / 48000)
        float gain = 1.0f;
    };
    struct GameBank
    {
        bool loaded = false, ok = false;
        std::map<int, Sample> se; // SE id -> decoded sample
    };
    struct Voice
    {
        const Sample *s = nullptr;
        double pos = 0.0;
    };
    std::mutex g_m;
    std::map<int, GameBank> g_games; // 4 = SvR04, 6 = SvR06, 7 = SYM
    std::vector<Voice> g_voices;
    int g_game = 0;
    AudioStream g_stream{};
    bool g_streamOpen = false;
    PS2Runtime::RecompiledFunction g_origPlaySe = nullptr;

    const char *gameDir(int game) { return game == 4 ? "MODS/SVR04/SE" : game == 6 ? "MODS/SVR06/SE" : game == 7 ? "MODS/SYM/SE" : nullptr; }

    // Sony PS-ADPCM (VAG) block decode; flags: 4 = loop start, 1 = end, 1|2 = end + jump to loop start
    void decodeVag(const std::vector<uint8_t> &bd, uint32_t off, Sample &out)
    {
        static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
        int s1 = 0, s2 = 0;
        for (uint32_t b = off; b + 16 <= bd.size(); b += 16)
        {
            const int filter = std::min(bd[b] >> 4, 4), shift = bd[b] & 15, flags = bd[b + 1];
            if (flags & 4)
                out.loopStart = static_cast<uint32_t>(out.pcm.size());
            for (int i = 0; i < 28; ++i)
            {
                int nib = (bd[b + 2 + i / 2] >> ((i & 1) * 4)) & 15;
                int v = static_cast<int16_t>(static_cast<uint16_t>(nib << 12)) >> shift;
                v += (s1 * f0[filter] + s2 * f1[filter] + 32) >> 6;
                v = std::clamp(v, -32768, 32767);
                s2 = s1;
                s1 = v;
                out.pcm.push_back(static_cast<int16_t>(v));
            }
            if (flags & 1)
            {
                out.loops = (flags & 2) != 0;
                break;
            }
        }
    }

    void loadGame(int game, GameBank &g)
    {
        g.loaded = true;
        const char *dir = gameDir(game);
        if (!dir)
            return;
        auto readAll = [](const std::string &p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        const std::vector<uint8_t> hd = readAll(std::string(dir) + "/bk7.hd"), bd = readAll(std::string(dir) + "/bk7.bd");
        std::ifstream map(std::string(dir) + "/se_map.txt");
        if (hd.size() < 0x40 || bd.empty() || !map || std::memcmp(hd.data(), "IECSsreV", 8) != 0)
            return;
        auto u8 = [&](uint32_t o) -> uint32_t { return o < hd.size() ? hd[o] : 0u; };
        auto s8 = [&](uint32_t o) -> int { return static_cast<int8_t>(u8(o)); };
        auto u16 = [&](uint32_t o) { return u8(o) | (u8(o + 1) << 8); };
        auto u32 = [&](uint32_t o) { return u16(o) | (u16(o + 2) << 16); };
        auto entry = [&](uint32_t chunk, uint32_t idx) -> uint32_t {
            if (!chunk || chunk >= hd.size() || idx > u32(chunk + 0xc))
                return 0;
            const uint32_t o = u32(chunk + 0x10 + idx * 4);
            return o == 0xffffffffu ? 0 : chunk + o;
        };
        const uint32_t prog = u32(0x24), sset = u32(0x28), smpl = u32(0x2c), vagi = u32(0x30);
        std::string line;
        while (std::getline(map, line))
        {
            unsigned se = 0;
            int pr = -1;
            if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%x %d", &se, &pr) != 2)
                continue;
            const uint32_t p = entry(prog, static_cast<uint32_t>(pr));
            if (!p || u8(p + 4) == 0)
                continue;
            const uint32_t tone = p + u32(p); // first tone (the effect banks have one tone / one sample per program)
            const uint32_t ss = entry(sset, u16(tone));
            const uint32_t sm = ss && u8(ss + 3) ? entry(smpl, u16(ss + 4)) : 0;
            const uint32_t vg = sm ? entry(vagi, u16(sm)) : 0;
            if (!vg || u32(vg) == 0xffffffffu)
                continue;
            Sample s;
            decodeVag(bd, u32(vg), s);
            if (s.pcm.empty())
                continue;
            // msin rows play key 0x3C velocity 0x77 (LIST row bytes 0c 77 3c); pitch = note - smpl center (+fine/128)
            const int note = 0x3C + s8(p + 8) + s8(tone + 0x12);
            const double fine = (s8(p + 9) + s8(sm + 0xc) + s8(tone + 0x13)) / 128.0;
            const double semis = note - static_cast<int>(u8(sm + 0xb)) + fine;
            s.step = static_cast<float>(u16(vg + 4) * std::pow(2.0, semis / 12.0) / 48000.0);
            // volume as modhsyn: prog[6] x tone[0x10] x smpl[0x10] (x128 each) x velocity 0x77
            s.gain = static_cast<float>(u8(p + 6) * u8(tone + 0x10) * u8(sm + 0x10)) / (128.0f * 128.0f * 128.0f) * (0x77 / 127.0f);
            std::fprintf(stderr, "[hostse] game %d se %x prog %d: %zu smp @%u Hz step %.3f gain %.2f%s%c", game, se, pr,
                         s.pcm.size(), u16(vg + 4), s.step, s.gain, s.loops ? " LOOP" : "", 10);
            g.se[static_cast<int>(se)] = std::move(s);
        }
        g.ok = !g.se.empty();
    }

    void fill(void *buffer, unsigned int frames)
    {
        static const float vol = [] { const char *e = std::getenv("HCTP_HOST_SE_VOL"); return e ? static_cast<float>(std::atof(e)) : 1.0f; }();
        int16_t *out = static_cast<int16_t *>(buffer);
        std::lock_guard<std::mutex> lock(g_m);
        for (unsigned int f = 0; f < frames; ++f)
        {
            float mix = 0.0f;
            for (Voice &v : g_voices)
            {
                if (!v.s)
                    continue;
                const auto &pcm = v.s->pcm;
                size_t i = static_cast<size_t>(v.pos);
                if (i + 1 >= pcm.size())
                {
                    if (v.s->loops && v.s->loopStart + 1 < pcm.size())
                        v.pos = v.s->loopStart + (v.pos - static_cast<double>(i)), i = static_cast<size_t>(v.pos);
                    else
                    {
                        v.s = nullptr;
                        continue;
                    }
                }
                const float t = static_cast<float>(v.pos - static_cast<double>(i));
                mix += (pcm[i] * (1.0f - t) + pcm[i + 1] * t) * v.s->gain;
                v.pos += v.s->step;
            }
            const int16_t o = static_cast<int16_t>(std::clamp(mix * vol, -32768.0f, 32767.0f));
            out[f * 2] = o;
            out[f * 2 + 1] = o;
        }
        g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [](const Voice &v) { return !v.s; }), g_voices.end());
    }

    // event start (game 4/6/7) or end (0): looping voices of the previous event stop (PS2: the event's SE stop / StopAll)
    void setGame(int game)
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (game != g_game || game == 0)
            g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [](const Voice &v) { return v.s && v.s->loops; }), g_voices.end());
        g_game = game;
    }

    // PlaySE_FromAttribute(obj, id, a2) 0x3ada80
    void playSeFromAttribute(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int id = static_cast<int>(static_cast<int32_t>(_mm_cvtsi128_si32(ctx->r[5])));
        const Sample *hit = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (g_game)
            {
                GameBank &g = g_games[g_game];
                if (!g.loaded)
                    loadGame(g_game, g);
                if (auto it = g.se.find(id); it != g.se.end())
                    hit = &it->second;
            }
            if (hit && IsAudioDeviceReady())
            {
                if (!g_streamOpen)
                {
                    g_stream = LoadAudioStream(48000, 16, 2);
                    SetAudioStreamCallback(g_stream, fill);
                    PlayAudioStream(g_stream);
                    g_streamOpen = true;
                }
                if (g_voices.size() < 24)
                    g_voices.push_back(Voice{hit, 0.0});
            }
        }
        if (!hit)
        {
            g_origPlaySe(rdram, ctx, runtime);
            return;
        }
        std::fprintf(stderr, "[hostse] play game %d se %x%c", g_game, id, 10);
        ctx->r[2] = _mm_setzero_si128();
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }

    void install(PS2Runtime &runtime)
    {
        if (std::getenv("HCTP_HOST_SE") && std::getenv("HCTP_HOST_SE")[0] == '0')
            return;
        g_origPlaySe = runtime.lookupFunction(0x3ada80u);
        if (!g_origPlaySe || !runtime.replaceFunction(0x3ada80u, playSeFromAttribute))
            std::fprintf(stderr, "[hostse] PlaySE_FromAttribute hook FAILED%c", 10);
    }
}

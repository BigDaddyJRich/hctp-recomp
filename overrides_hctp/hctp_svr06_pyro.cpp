// HCTP_SVR06_PYRO=1 (Nov 9 2026): SvR06 entrance pyro (prtcls.pac PCEN/<id> + gm.pac GMEV/<id>) simulated on the host and
// drawn with GS::submitHostTris after the frame. Port of SvR06 Particle.lib (SLUS_212.86 + ELF.MAP via tools\svr06_dis.py);
// every rule below is from notes\SVR06_PYRO_COMPAT_20261109.md (2c / 2d / BUILD SESSION 3 prep). First user: CAM entrance
// row 3944 = Brock 3044 + PCEN 4044. Switches: HCTP_PYRO_SIZE (quad half-size per scale unit, 1.0), HCTP_PYRO_START
// (sim frames from arm to GMEV frame 310, 310), HCTP_PYRO_HZ (30), HCTP_PYRO_EULER=zyx, HCTP_PYRO_LOG=1,
// HCTP_PYRO_ATEST=0 (drop the SvR06 TEST 0x53201), HCTP_PYRO_ZGUARD=0 (drop the 0xFFFFFF homogeneous-z cull).
#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_types.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern std::atomic<void (*)()> g_hctpPrePresentHook;

bool hctpGetCharProj(float out[4][4]); // hctp_native_vu1.cpp (Nov 11 pyro depth)
namespace
{
using Bytes = std::vector<uint8_t>;

bool pyroLog()
{
    static const bool on = std::getenv("HCTP_PYRO_LOG") != nullptr;
    return on;
}
float envF(const char *n, float d)
{
    const char *v = std::getenv(n);
    return v ? static_cast<float>(std::atof(v)) : d;
}
uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
float rdf(const uint8_t *p) { float f; std::memcpy(&f, p, 4); return f; }

// ---- SvR "BPE " (Mod Maker svrfmt.bpe_decode, 1:1) ----
bool bpeDecode(const Bytes &src, Bytes &out)
{
    if (src.size() < 16 || std::memcmp(src.data(), "BPE ", 4) != 0)
    {
        out = src;
        return true;
    }
    const uint32_t packed = rd32(&src[8]), unpacked = rd32(&src[12]);
    size_t pos = 16;
    const size_t end = std::min<size_t>(16 + packed, src.size());
    out.clear();
    out.reserve(unpacked);
    while (pos < end)
    {
        uint8_t left[256], right[256];
        for (int i = 0; i < 256; ++i) left[i] = static_cast<uint8_t>(i), right[i] = 0;
        int c = 0;
        int count = src[pos++];
        for (;;)
        {
            if (count > 127) { c += count - 127; count = 0; }
            if (c == 256) break;
            for (int k = 0; k < count + 1; ++k)
            {
                left[c] = src[pos++];
                if (c != left[c]) right[c] = src[pos++];
                ++c;
            }
            if (c == 256) break;
            count = src[pos++];
        }
        const uint32_t size = src[pos] | (src[pos + 1] << 8);
        pos += 2;
        std::vector<uint8_t> stack;
        for (uint32_t i = 0; i < size && pos + i < src.size(); ++i)
        {
            stack.push_back(src[pos + i]);
            while (!stack.empty())
            {
                const uint8_t s = stack.back();
                stack.pop_back();
                if (left[s] == s) out.push_back(s);
                else { stack.push_back(right[s]); stack.push_back(left[s]); }
            }
        }
        pos += size;
    }
    return out.size() == unpacked;
}

// 'PAC ' = u32 n, n x {u16 id, u24 off, u24 size} (off from end of table), each entry may be BPE
std::map<uint16_t, Bytes> parsePac(const Bytes &b)
{
    std::map<uint16_t, Bytes> out;
    if (b.size() < 8 || std::memcmp(b.data(), "PAC ", 4) != 0) return out;
    const uint32_t n = rd32(&b[4]);
    const size_t base = 8 + static_cast<size_t>(n) * 8;
    for (uint32_t i = 0; i < n && 16 + i * 8 <= b.size(); ++i)
    {
        const uint8_t *e = &b[8 + i * 8];
        const uint16_t id = rd16(e);
        const uint32_t off = e[2] | (e[3] << 8) | (e[4] << 16), sz = e[5] | (e[6] << 8) | (e[7] << 16);
        if (base + off + sz > b.size()) continue;
        Bytes raw(b.begin() + base + off, b.begin() + base + off + sz), dec;
        bpeDecode(raw, dec);
        out[id] = std::move(dec);
    }
    return out;
}

bool readAt(std::ifstream &f, size_t off, size_t n, Bytes &out)
{
    out.resize(n);
    f.seekg(static_cast<std::streamoff>(off));
    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
}

// prtcls.pac: 41 entries at 0x808 {id[4], u16 sec, u16 size/0x100}; data 0x4000 + sec*0x800
// Nov 11 SvR04 PYRO: the same engine plays SvR04 entrance pyro. SvR04 prtcls.pac PCEN/<ev> = SvR06 layout (POF0 prtc +
// textures per file); SvR04 gm.pac group '0001' holds <ev> entries with the SvR06 GMEV layout (0 list, 1 GMPD, 2 params,
// 3 tag PAC 10000). g_src 4 = MODS/SVR04/{prtcls,gm}.pac (generic DPAC name lookup), 6 = MODS/SVR06 (original code).
int g_src = 6;
bool dpacFind(const char *path, const char *id, Bytes &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    Bytes h;
    if (!readAt(f, 0, 0x4000, h) || std::memcmp(h.data(), "DPAC", 4) != 0) return false;
    for (size_t off = 0x800; off + 8 <= 0x4000 && std::memcmp(&h[off], "\0\0\0\0", 4) != 0;)
    {
        uint16_t n = rd16(&h[off + 4]), start = rd16(&h[off + 6]);
        if (n & 0x8000u)
        {
            n &= 0x7FFFu;
            off += 8 + 4u * n; // id-form groups (hex ids) never hold PCEN/GMEV names
            (void)start;
            continue;
        }
        uint32_t cnt = 0;
        for (; cnt < n && off + 16 + 8 * cnt <= 0x4000 && std::memcmp(&h[off + 8 + 8 * cnt], "\0\0\0\0", 4) != 0; ++cnt)
        {
            const uint8_t *e = &h[off + 8 + 8 * cnt];
            if (std::memcmp(e, id, 4) == 0)
            {
                Bytes raw;
                if (!readAt(f, (8u + rd16(e + 4)) * 0x800u, rd16(e + 6) * 0x100u, raw)) return false;
                return bpeDecode(raw, out);
            }
        }
        off += 8 + 8 * cnt;
    }
    return false;
}

bool loadPcen(const char *id, Bytes &out)
{
    if (g_src == 4) return dpacFind("MODS/SVR04/prtcls.pac", id, out);
    std::ifstream f("MODS/SVR06/prtcls.pac", std::ios::binary);
    if (!f) return false;
    Bytes dir;
    if (!readAt(f, 0x808, 41 * 8, dir)) return false;
    for (int i = 0; i < 41; ++i)
        if (std::memcmp(&dir[i * 8], id, 4) == 0)
        {
            Bytes raw;
            if (!readAt(f, 0x4000 + rd16(&dir[i * 8 + 4]) * 0x800u, rd16(&dir[i * 8 + 6]) * 0x100u, raw)) return false;
            return bpeDecode(raw, out);
        }
    return false;
}

// gm.pac: group headers at 0x800 {4cc, u16 n*2, u16 start}, 8 B entries after them
bool loadGmev(const char *id, Bytes &out)
{
    if (g_src == 4) return dpacFind("MODS/SVR04/gm.pac", id, out);
    std::ifstream f("MODS/SVR06/gm.pac", std::ios::binary);
    if (!f) return false;
    Bytes hdr;
    if (!readAt(f, 0x800, 0x400, hdr)) return false;
    size_t o = 0;
    struct G { char t[4]; uint16_t n, st; };
    std::vector<G> gs;
    while (o + 8 <= hdr.size() && std::isalpha(hdr[o]) && std::isalpha(hdr[o + 1]) && std::isalpha(hdr[o + 2]) && std::isalpha(hdr[o + 3]))
    {
        G g;
        std::memcpy(g.t, &hdr[o], 4);
        g.n = rd16(&hdr[o + 4]) / 2;
        g.st = rd16(&hdr[o + 6]);
        gs.push_back(g);
        o += 8;
    }
    const size_t ent = 0x800 + o;
    for (const G &g : gs)
    {
        if (std::memcmp(g.t, "GMEV", 4) != 0) continue;
        Bytes e;
        for (uint32_t i = g.st; i < static_cast<uint32_t>(g.st) + g.n; ++i)
        {
            if (!readAt(f, ent + i * 8u, 8, e)) return false;
            if (std::memcmp(e.data(), id, 4) == 0)
            {
                Bytes raw;
                if (!readAt(f, 0x4000 + rd16(&e[4]) * 0x800u, rd16(&e[6]) * 0x100u, raw)) return false;
                return bpeDecode(raw, out);
            }
        }
    }
    return false;
}

// ---- TIM2 8-bit / 4-bit with 32/16-bit CLUT -> RGBA (raw PS2 alpha) ----
bool decodeTim2(const Bytes &b, uint32_t &w, uint32_t &h, std::vector<uint32_t> &px)
{
    if (b.size() < 0x40 || std::memcmp(b.data(), "TIM2", 4) != 0) return false;
    const uint8_t *p = &b[0x10];
    const uint32_t clutSize = rd32(p + 4), imgSize = rd32(p + 8);
    const uint16_t hdrSize = rd16(p + 12), clutColors = rd16(p + 14);
    const uint8_t clutType = p[18], imgType = p[19];
    w = rd16(p + 20);
    h = rd16(p + 22);
    const size_t img = 0x10 + hdrSize, clut = img + imgSize;
    if (clut + clutSize > b.size() || w == 0 || h == 0) return false;
    std::vector<uint32_t> pal(std::max<uint32_t>(clutColors, 256u), 0);
    const bool c32 = (clutType & 0x3F) == 3, c16 = (clutType & 0x3F) == 1 || (clutType & 0x3F) == 2;
    for (uint32_t i = 0; i < clutColors; ++i)
    {
        uint32_t c = 0;
        if (c32) c = rd32(&b[clut + i * 4]);
        else if (c16)
        {
            const uint16_t v = rd16(&b[clut + i * 2]);
            c = ((v & 31u) << 3) | (((v >> 5) & 31u) << 11) | (((v >> 10) & 31u) << 19) | ((v & 0x8000u) ? 0x80000000u : 0u);
        }
        pal[i] = c;
    }
    if (imgType == 5 && !(clutType & 0x80) && clutColors >= 256) // CSM1: swap index bits 3 and 4
    {
        std::vector<uint32_t> t = pal;
        for (uint32_t i = 0; i < 256; ++i) pal[i] = t[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)];
    }
    px.assign(static_cast<size_t>(w) * h, 0);
    for (uint32_t i = 0; i < w * h; ++i)
    {
        uint32_t idx;
        if (imgType == 5) idx = b[img + i];
        else if (imgType == 4) idx = (b[img + i / 2] >> ((i & 1) * 4)) & 15;
        else return false;
        px[i] = pal[idx];
    }
    return true;
}

float rand2() { return (static_cast<float>(std::rand() & 0xFFFF) - 32767.0f) / 32768.0f; } // RandomFloat2 0x27a090
constexpr float kDeg = 3.14159265f / 180.0f;

struct M3 { float m[3][3]; };
M3 ident() { M3 r{}; r.m[0][0] = r.m[1][1] = r.m[2][2] = 1; return r; }
M3 mul(const M3 &a, const M3 &b)
{
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
// row-vector rotations (objMatrix::RotX/Y/Z): v' = v * R
M3 rotX(float a) { M3 r = ident(); float c = std::cos(a), s = std::sin(a); r.m[1][1] = c; r.m[1][2] = s; r.m[2][1] = -s; r.m[2][2] = c; return r; }
M3 rotY(float a) { M3 r = ident(); float c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][2] = -s; r.m[2][0] = s; r.m[2][2] = c; return r; }
M3 rotZ(float a) { M3 r = ident(); float c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][1] = s; r.m[1][0] = -s; r.m[1][1] = c; return r; }
void vmul(const float v[3], const M3 &m, float o[3])
{
    float t[3];
    for (int j = 0; j < 3; ++j) t[j] = v[0] * m.m[0][j] + v[1] * m.m[1][j] + v[2] * m.m[2][j];
    o[0] = t[0]; o[1] = t[1]; o[2] = t[2];
}
M3 euler(float x, float y, float z)
{
    static const bool zyx = [] { const char *e = std::getenv("HCTP_PYRO_EULER"); return e && std::strcmp(e, "zyx") == 0; }();
    return zyx ? mul(mul(rotZ(z), rotY(y)), rotX(x)) : mul(mul(rotX(x), rotY(y)), rotZ(z));
}

enum Act { Birth = 0, Gravity = 1, Fade = 2, BirthRange = 3, Roll = 5, TexAnim = 6, Move = 7, AddSpeed = 8, Scaling = 13, Link = 16 };
struct Action { uint8_t id; uint32_t start, end; float p[8]; };
struct Particle
{
    bool live = false;
    float pos[3]{}, vel[3]{};
    // SvR06 data+0xB0 (GetBlurVector 0x272eb0 = +0xB0 - pos): ParticleBase::BlurUpDate 0x272750 copies pos at the START of
    // every ParticleBase::Action 0x271d70 (before the actions + the move); SetBirthPos 0x275914 = birth pos; spark polygons
    // reset it to the moved pos at age 1 (ParticleData::Action 0x27297c).
    float prev[3]{};
    int life = 0, age = 0;
    float scale = 1, angle = 0, aspeed = 0, sTarget = 0, sRate = 0;
    float aspect = 1;  // data+0xCC (Reset 1.0; PrtActScaling 0x2787a0: type 2 only = p5 / 100)
    bool spark = false; // data+0xDC (PrtActRoll::RollSpark 0x277a70: type 2 with int(p5) == 1)
    M3 base = ident();  // data+0x00 (type 2): SetBirthVec 0x2755f0 = M60 * W; SparkRotate 0x272d50 = RZ(-pitch) * RY(yaw)
    int tex = 0;
    int alpha = 255;
};
struct Emitter
{
    uint8_t drawType = 0, blend = 0;
    uint16_t maxP = 0;
    uint32_t stBorn = 0, stLinkKill = 0, stNoTex = 0, stDraw = 0, stCull = 0; // HCTP_PYRO_EMSTATS (Nov 11 smoke hunt)
    uint16_t copies = 1; // rec+0x5A -> packet 0x3F1.y (VU inner loop count: copies along the blur vector)
    float blurK = 0;     // rec+0x5C -> packet 0x3F1.z (VU vf22.z: blur step scale)
    std::vector<Action> acts;
    std::vector<Particle> ps;
    float lpos[3]{}, lrot[3]{};
    M3 lrotM = ident(); // ParticleBase+0x60 (AddRot accumulates M60 = M60 * RZ*RY*RX)
    std::vector<int> texIds; // TexGroup index -> host texture id
};
struct File { std::vector<Emitter> em; };
struct Instance
{
    File f;
    float wpos[3]{};
    M3 wrot = ident();
    int startTick = 0, frame = 0, evScene = -1, evFrame = 0; // GMEV start window (SvR06 scene/frame)
    // Nov 11 cue windows (SvR06 gmParticleEmitter::MyAction 0x4de210, game.bin): params {no, scene0, frame0, scene1, frame1,
    // mode}; start when scene > scene0 or (== and GetMotionCameraCurrentFrame >= frame0); at the end window mode 0 ->
    // DeathParticle (ParticleBase+0x108: no more emitter births, live particles finish), else VanishParticle (+0x104 too).
    int endScene = -1, endFrame = 0, endMode = 0, src = 6; // src = g_src at arm (caller resets g_src to 6 after arming SvR04)
    bool started = false, dead = false, vanished = false;
};

struct TexInfo { uint16_t id; uint32_t w, h; };
// Nov 11 PART9: textures are PER FILE (retail CParticleGroup::SetTexture per group, SvR04 proto 0x1f5430): the same name is a
// different image per PCEN entry (SvR04 3047 has 3 'kemuri', 3003 two 'kemuri' + two 'zimen01'; SvR06 4044 also has 'hbn_red'
// / 'kemuri'). The old global name map (first wins, never cleared) drew the wrong smoke/ground/spark textures.
using TexMap = std::map<std::string, TexInfo>;
TexMap g_tex;                          // id -> dims lookup only (every registered texture, keyed by name#hash)
std::map<uint64_t, TexInfo> g_texHash; // content hash -> host texture (no re-register on re-arm)
TexMap g_packTex;                      // this arm: first texture of each name in the pack (fallback for files without sub 1)
std::vector<Instance> g_inst;
std::mutex g_mx;
PS2Runtime *g_rt = nullptr;
int g_tick = 0;
uint32_t g_common = 0; // CAM common: CEventSystem at +0x28 (scene u16 +0x18, frame u16 +0x1A)
int g_lastEvScene = -1, g_lastEvFrame = -1;
// Build 10: event clock handed in by the CEventSystem::Action 0x166e00 hook (hctp_overrides.cpp evsActionSvr).
std::atomic<uint32_t> g_evPacked{0xFFFFFFFFu}; // scene << 16 | frame (30 Hz event frames)
std::atomic<int> g_winScene{-1}, g_winFrame{0};
std::atomic<bool> g_seFired{false}, g_armedA{false};
bool g_active = false;
std::chrono::steady_clock::time_point g_last;
double g_acc = 0;

std::string baseName(const char *s, size_t max)
{
    std::string r;
    for (size_t i = 0; i < max && s[i]; ++i) r += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    const size_t dot = r.find('.');
    return dot == std::string::npos ? r : r.substr(0, dot);
}

void loadTextures(const Bytes &blob, TexMap &local)
{
    if (blob.size() < 0x10) return;
    const uint32_t n = rd32(&blob[0]), tbl = rd32(&blob[0xC]);
    static uint16_t nextId = 0x5000;
    for (uint32_t i = 0; i < n && tbl + i * 0x20 + 0x20 <= blob.size(); ++i)
    {
        const uint8_t *e = &blob[tbl + i * 0x20];
        const std::string nm = baseName(reinterpret_cast<const char *>(e), 16);
        const uint32_t sz = rd32(e + 0x14), off = rd32(e + 0x18);
        if (off + sz > blob.size() || local.count(nm)) continue;
        uint64_t hsh = 1469598103934665603ull;
        for (uint32_t q = 0; q < sz; ++q) hsh = (hsh ^ blob[off + q]) * 1099511628211ull;
        if (auto hit = g_texHash.find(hsh); hit != g_texHash.end())
        {
            local[nm] = hit->second;
            if (!g_packTex.count(nm)) g_packTex[nm] = hit->second;
            continue;
        }
        Bytes t(blob.begin() + off, blob.begin() + off + sz);
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> px;
        if (!decodeTim2(t, w, h, px)) { if (pyroLog()) std::fprintf(stderr, "[pyro] tim2 %s decode failed\n", nm.c_str()); continue; }
        const uint16_t id = nextId++;
        hctpRegisterHostTexture(id, w, h, px.data());
        char key[32];
        std::snprintf(key, sizeof(key), "#%016llx", static_cast<unsigned long long>(hsh));
        g_tex[nm + key] = {id, w, h};
        g_texHash[hsh] = {id, w, h};
        local[nm] = {id, w, h};
        if (!g_packTex.count(nm)) g_packTex[nm] = {id, w, h};
        if (pyroLog()) std::fprintf(stderr, "[pyro] tex %s %ux%u id %x\n", nm.c_str(), w, h, id);
    }
}

bool parsePrtc(const Bytes &fileBytes, File &out, const TexMap &local)
{
    if (fileBytes.size() < 0x20) return false;
    const uint8_t *b = fileBytes.data() + 8; // 8 B POF0 header
    const size_t bs = fileBytes.size() - 8;
    if (std::memcmp(b, "PRTC", 4) != 0) return false;
    const uint16_t n = rd16(b + 0xC);
    const uint32_t eo = rd32(b + 0x10), to = rd32(b + 0x14);
    std::vector<int> groupTex;
    if (to + 0x18 <= bs)
    {
        const uint32_t ntex = rd32(b + to), namesOff = rd32(b + to + 0x14);
        for (uint32_t i = 0; i < ntex && namesOff + i * 16 + 16 <= bs; ++i)
        {
            const std::string nm = baseName(reinterpret_cast<const char *>(b + namesOff + i * 16), 16);
            auto it = local.find(nm);
            if (it != local.end()) { groupTex.push_back(it->second.id); continue; }
            auto pk = g_packTex.find(nm);
            groupTex.push_back(pk == g_packTex.end() ? 0 : pk->second.id);
        }
    }
    for (uint16_t i = 0; i < n; ++i)
    {
        const size_t e = eo + i * 0x68u;
        if (e + 0x68 > bs) return false;
        Emitter em;
        em.drawType = b[e + 0xA] == 3 ? 0 : b[e + 0xA];
        em.blend = b[e + 0xB];
        em.maxP = std::min<uint16_t>(rd16(b + e + 0x58), 512);
        em.ps.resize(em.maxP);
        em.copies = std::clamp<uint16_t>(rd16(b + e + 0x5A), 1, 16);
        em.blurK = rdf(b + e + 0x5C);
        em.texIds = groupTex;
        const uint16_t nfb = rd16(b + e + 0x60);
        const uint32_t fbo = rd32(b + e + 0x64);
        for (uint16_t j = 0; j < nfb && fbo + j * 8u + 8 <= bs; ++j)
        {
            const uint16_t fr = rd16(b + fbo + j * 8), nc = rd16(b + fbo + j * 8 + 2);
            uint32_t p = rd32(b + fbo + j * 8 + 4);
            for (uint16_t k = 0; k < nc && p + 4 <= bs; ++k)
            {
                Action a{};
                a.id = b[p];
                const uint8_t sz = b[p + 1];
                const uint16_t par = rd16(b + p + 2);
                a.start = fr;
                a.end = par == 0xFFFF ? 0xFFFFFFFFu : par;
                for (int q = 0; q < 8 && q * 4 + 4 <= sz; ++q) a.p[q] = rdf(b + p + 4 + q * 4);
                em.acts.push_back(a);
                p += 4u + sz;
            }
        }
        out.em.push_back(std::move(em));
    }
    return true;
}

// emitter rotation = local (Move) * world (tag)
// SvR06 SetBirthVec 0x2752c0: dir = (1,0,0) * W(+0xC0, RotZYX of tag) * M60(+0x60) * cone  [MulMatrix(a,b): a = b*a]
M3 emRot(const Instance &in, const Emitter &em) { return mul(in.wrot, em.lrotM); }

void stepInstance(Instance &in)
{
    // ParticleBase::Action 0x271d70 -> BlurUpDate 0x272750 first: data+0xB0 = data+0x80 for every particle
    for (Emitter &em : in.f.em)
        for (Particle &pt : em.ps)
            if (pt.live) std::memcpy(pt.prev, pt.pos, sizeof(pt.prev));
    for (int pass = 0; pass < 3; ++pass)
        for (Emitter &em : in.f.em)
            for (const Action &a : em.acts)
            {
                if (!(a.start <= static_cast<uint32_t>(in.frame) && static_cast<uint32_t>(in.frame) < a.end)) continue;
                const bool isBirth = a.id == Birth, isLink = a.id == Link;
                if ((pass == 0) != isBirth || (pass == 2) != isLink) continue;
                if (isBirth && in.dead) continue; // ParticleBase::Action 0x271dc0: +0x108 skips the emitter actions
                const float *p = a.p;
                const M3 R = emRot(in, em);
                switch (a.id)
                {
                case Birth:
                {
                    int cnt = static_cast<int>(p[0]) + static_cast<int>(p[1] * rand2());
                    for (Particle &pt : em.ps)
                    {
                        if (cnt <= 0) break;
                        if (pt.live) continue;
                        --cnt;
                        pt = Particle{};
                        pt.live = true;
                        ++em.stBorn;
                        pt.life = std::max(1, static_cast<int>(p[2] + p[3] * rand2()));
                        const float speed = p[4] + p[5] * rand2();
                        // cone: p6/2 deg, axes gated by int(p7) bits (0 -> 15): 1=Z 2=Y 4=X; acc = RX*RY*RZ
                        M3 cone = ident();
                        const float c = p[6] * 0.5f * kDeg;
                        if (c != 0.0f)
                        {
                            int fl = static_cast<int>(p[7]);
                            if (fl == 0) fl = 15;
                            if (fl & 1) cone = mul(rotZ(rand2() * c), cone);
                            if (fl & 2) cone = mul(rotY(rand2() * c), cone);
                            if (fl & 4) cone = mul(rotX(rand2() * c), cone);
                        }
                        const float d0[3] = {1, 0, 0};
                        float d[3];
                        vmul(d0, mul(R, cone), d);
                        for (int k = 0; k < 3; ++k) pt.vel[k] = d[k] * speed;
                        float lp[3];
                        vmul(em.lpos, in.wrot, lp);
                        for (int k = 0; k < 3; ++k) pt.pos[k] = in.wpos[k] + lp[k];
                        std::memcpy(pt.prev, pt.pos, sizeof(pt.prev)); // SetBirthPos 0x275914: +0xB0 = birth pos
                        if (em.drawType == 2) pt.base = mul(em.lrotM, in.wrot); // SetBirthVec 0x275534..0x2755fc: M00 = M60 * W
                    }
                    break;
                }
                case Move:
                {
                    const int N = static_cast<int>(p[0]);
                    const int rel = in.frame - static_cast<int>(a.start);
                    if (!(static_cast<int>(p[7]) != 0 || (rel >= 0 && rel <= N))) break;
                    const float k = 1.0f / static_cast<float>(N + 1);
                    em.lpos[0] += p[1] * k; em.lpos[1] += -p[2] * k; em.lpos[2] += p[3] * k;
                    em.lrot[0] += p[4] * kDeg * k; em.lrot[1] += p[5] * kDeg * k; em.lrot[2] += p[6] * kDeg * k;
                    { // PrtActMove -> ParticleBase::AddRot 0x272440: M60 = M60 * (RZ*RY*RX), zero axes skipped
                        M3 acc = ident();
                        if (p[4] != 0.0f) acc = mul(rotX(p[4] * kDeg * k), acc);
                        if (p[5] != 0.0f) acc = mul(rotY(p[5] * kDeg * k), acc);
                        if (p[6] != 0.0f) acc = mul(rotZ(p[6] * kDeg * k), acc);
                        em.lrotM = mul(em.lrotM, acc);
                    }
                    break;
                }
                case Link:
                {
                    const int si = static_cast<int>(p[0]);
                    if (si < 0 || si >= static_cast<int>(in.f.em.size()) || &in.f.em[si] == &em) break;
                    const Emitter &src = in.f.em[si];
                    if (src.ps.empty()) break;
                    for (size_t i = 0; i < em.ps.size(); ++i)
                    {
                        Particle &pt = em.ps[i];
                        if (!pt.live) continue;
                        const Particle &sp = src.ps[i % src.ps.size()];
                        if (!sp.live && pt.age == 0) { pt.live = false; ++em.stLinkKill; continue; }
                        if (sp.live && pt.age >= static_cast<int>(p[2]) && pt.age <= static_cast<int>(p[3]))
                            for (int k = 0; k < 3; ++k) pt.pos[k] = sp.pos[k] - sp.vel[k] * p[1];
                    }
                    break;
                }
                default:
                    for (Particle &pt : em.ps)
                    {
                        if (!pt.live) continue;
                        switch (a.id)
                        {
                        case Gravity:
                        {
                            float g[3] = {p[0], -p[1], p[2]};
                            if (static_cast<int>(p[3]) == 0) vmul(g, R, g);
                            for (int k = 0; k < 3; ++k) pt.vel[k] += g[k];
                            break;
                        }
                        case Fade:
                        {
                            const int in_ = static_cast<int>(p[0]), out_ = static_cast<int>(p[1]);
                            if (pt.age < in_) pt.alpha = 255 * pt.age / in_;
                            else if (pt.life - pt.age < out_) pt.alpha = 255 * (pt.life - pt.age) / out_;
                            else pt.alpha = 255;
                            if (pt.alpha <= 0 && pt.age >= in_) pt.live = false; // fade-in starts at 0: only fade-out kills
                            break;
                        }
                        case Roll:
                            if (em.drawType == 1) break;
                            // PrtActRoll::Action 0x277960: type 2 + int(p5) == 1 -> RollSpark (data+0xDC = 1), no roll
                            if (em.drawType == 2 && static_cast<int>(p[5]) == 1) { pt.spark = true; break; }
                            if (pt.age == 0) pt.aspeed = p[0] * kDeg + rand2() * p[1] * kDeg;
                            else pt.aspeed += p[2] * kDeg + rand2() * p[3] * kDeg;
                            break;
                        case TexAnim:
                            if (pt.age == static_cast<int>(p[0])) pt.tex = static_cast<int>(p[1]);
                            break;
                        case AddSpeed:
                            if (pt.age >= static_cast<int>(p[0]) && pt.age < static_cast<int>(p[1]))
                                for (int k = 0; k < 3; ++k) pt.vel[k] *= p[2 + k] / 100.0f + rand2() * p[5 + k];
                            break;
                        case Scaling:
                        {
                            const int a0 = static_cast<int>(p[3]), a1 = static_cast<int>(p[4]);
                            if (pt.age == a0)
                            {
                                pt.scale = p[0];
                                pt.sTarget = std::max(0.0f, p[1] + p[2] * rand2());
                                pt.sRate = a1 > a0 ? (pt.sTarget - p[0]) / static_cast<float>(a1 - a0) : 0.0f;
                            }
                            else if (pt.age > a0 && pt.age <= a1)
                                pt.scale = p[0] + static_cast<float>(pt.age - a0) * pt.sRate;
                            if (em.drawType == 2) pt.aspect = p[5] / 100.0f; // 0x27878c: action+0x30 / 100.0 -> data+0xCC
                            break;
                        }
                        default:
                            break;
                        }
                    }
                    break;
                }
            }
    ++in.frame;
    for (Emitter &em : in.f.em)
        for (Particle &pt : em.ps)
        {
            if (!pt.live) continue;
            if (pt.age < pt.life) ++pt.age;
            else { pt.live = false; continue; }
            for (int k = 0; k < 3; ++k) pt.pos[k] += pt.vel[k];
            // ParticleData::Action 0x2728f0: type 2 (a2 = 0) spark -> SparkRotate + (age 1) +0xB0 = pos; else roll the angle
            if (em.drawType == 2 && pt.spark)
            {
                // SparkRotate 0x272d50: yaw = DirX = atan2(-vz, vx); pitch = atan2(-vy, |v.xz|); M00 = RZ(-pitch) * RY(yaw)
                const float yaw = std::atan2(-pt.vel[2], pt.vel[0]);
                const float pitch = std::atan2(-pt.vel[1], std::sqrt(pt.vel[0] * pt.vel[0] + pt.vel[2] * pt.vel[2]));
                pt.base = mul(rotZ(-pitch), rotY(yaw));
                if (pt.age == 1) std::memcpy(pt.prev, pt.pos, sizeof(pt.prev));
            }
            else
                pt.angle += pt.aspeed;
        }
}

struct Cam { float M[4][4]; float E[4][4]{}; bool ok = false; }; // E = world->view (cam+0xE0), M = E*A
// Matrix order variants (2b said v * (E*A); first CAM test projected the posts off-screen / behind -> pick at runtime):
// 0 v*(E*A), 1 v*(A*E), 2 (E*A)*v, 3 (A*E)*v. Chosen = the one that puts the ring centre near screen centre with w > 0.
int g_variant = -1;
int g_gateTick = -1, g_startBase = 0;
Cam buildCam(const float E[4][4], const float A[4][4], int var)
{
    Cam c;
    const float(*L)[4] = (var & 1) ? A : E;
    const float(*R)[4] = (var & 1) ? E : A;
    float P[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            P[i][j] = L[i][0] * R[0][j] + L[i][1] * R[1][j] + L[i][2] * R[2][j] + L[i][3] * R[3][j];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            c.M[i][j] = (var & 2) ? P[j][i] : P[i][j];
    std::memcpy(c.E, E, sizeof(c.E));
    c.ok = true;
    return c;
}
// Nov 11 PYRO DEPTH: HCTP's character view->clip matrix (hctp_native_vu1.cpp); z = clip.z / clip.w of view = v * E,
// the same value the wrestlers get (FTOI4 -> zMul 16), so particles behind a wrestler are hidden. HCTP_PYRO_HCTPZ=0 -> old z.
float g_pm[4][4];
bool g_pmOk = false;
bool project(const Cam &c, const float v[3], float o[4])
{
    float r[4];
    for (int j = 0; j < 4; ++j) r[j] = v[0] * c.M[0][j] + v[1] * c.M[1][j] + v[2] * c.M[2][j] + c.M[3][j];
    if (r[3] <= 0.01f) return false;
    o[0] = r[0] / r[3]; o[1] = r[1] / r[3]; o[2] = r[2] / r[3]; o[3] = r[3];
    if (g_pmOk)
    {
        float vw[4];
        for (int j = 0; j < 3; ++j) vw[j] = v[0] * c.E[0][j] + v[1] * c.E[1][j] + v[2] * c.E[2][j] + c.E[3][j];
        vw[3] = 1.0f;
        float cz = 0, cw = 0;
        for (int i = 0; i < 4; ++i) { cz += vw[i] * g_pm[i][2]; cw += vw[i] * g_pm[i][3]; }
        if (std::fabs(cw) > 1e-6f) o[2] = cz / cw;
    }
    return std::isfinite(o[0]) && std::isfinite(o[1]);
}
Cam readCam(uint8_t *rdram)
{
    Cam c;
    uint32_t cam;
    std::memcpy(&cam, rdram + 0x526E70u, 4);
    cam &= 0x1FFFFFFu;
    if (cam == 0 || cam + 0x120 > 0x2000000u) return c;
    float E[4][4], A[4][4];
    std::memcpy(E, rdram + cam + 0xE0, 64);
    std::memcpy(A, rdram + cam + 0xA0, 64);
    static const int forced = std::getenv("HCTP_PYRO_PROJ") ? std::atoi(std::getenv("HCTP_PYRO_PROJ")) : -1;
    if (forced >= 0) return buildCam(E, A, forced & 3);
    g_variant = 0; // Perspect 0x157b20 (VU0 decoded): M row i = E[i].x*A0 + E[i].y*A1 + E[i].z*A2 + E[i].w*A3
    return buildCam(E, A, g_variant);
}

// SvR06 SeikikaRadian 0x111a40: while a > pi: a -= 2pi; while a < -pi: a += 2pi
float seikikaRadian(float a)
{
    for (int i = 0; i < 64 && a > 3.14159274f; ++i) a -= 6.28318548f;
    for (int i = 0; i < 64 && a < -3.14159274f; ++i) a += 6.28318548f;
    return a;
}
// VU ESIN = 5-term odd series (PCSX2 VUops.cpp _vuESIN constants)
float vuEsin(float p)
{
    const float p2 = p * p;
    return p * (1.0f + p2 * (-0.166666567325592f + p2 * (0.008333025500178f + p2 * (-0.000198074136279f + p2 * 0.000002601886990f))));
}

void prePresent()
{
    std::lock_guard<std::mutex> lock(g_mx);
    if (!g_active || !g_rt) return;
    static const float hz = std::max(1.0f, envF("HCTP_PYRO_HZ", 30.0f));
    const auto now = std::chrono::steady_clock::now();
    g_acc += std::chrono::duration<double>(now - g_last).count();
    g_last = now;
    g_acc = std::min(g_acc, 0.25);
    int steps = 0;
    while (g_acc >= 1.0 / hz) { g_acc -= 1.0 / hz; ++steps; }
    size_t live = 0;
    // Start gate (Nov 9 test 2): SvR06 frame 310 != Brock's HCTP 3044 timing (camera looked away from the ring) ->
    // start when the ring centre is on screen, not before HCTP_PYRO_MIN ticks (default 60 = 2 s).
    static const int minTick = static_cast<int>(envF("HCTP_PYRO_MIN", 60.0f));
    // Build 9: the CAM common pointer (0x529400) is still null when the row is armed (test 8: event clock never ran,
    // old ring-on-screen gate fired at tick 966) -> fetch it here every tick until it exists.
    if (std::getenv("HCTP_PYRO_EVCLOCK") && std::getenv("HCTP_PYRO_EVCLOCK")[0] == '0' && g_gateTick < 0 && g_tick >= minTick)
    {
        const Cam gc = readCam(g_rt->memory().getRDRAM());
        float o[4] = {0, 0, 0, 0};
        const float centre[3] = {0, -10, 0};
        if (gc.ok && project(gc, centre, o) && std::fabs(o[0] - 2048.0f) < 300.0f && std::fabs(o[1] - 2048.0f) < 220.0f)
        {
            g_gateTick = g_tick;
            for (Instance &in : g_inst) in.startTick += g_tick - g_startBase;
            if (pyroLog()) std::fprintf(stderr, "[pyro] ring on screen at tick %d (gs %.0f %.0f) -> start%c", g_tick, o[0], o[1], 10);
        }
    }
    // Build 8: run on the reader event's OWN clock (CEventSystem GetCurrentScene 0x167350 / GetCurrentFrame 0x167380):
    // start at the GMEV window (scene 15 = bank segment x 15, or scene index*5), one sim step per event frame.
    bool evClock = false;
    const uint32_t evPk = g_evPacked.load(std::memory_order_relaxed);
    if (evPk != 0xFFFFFFFFu && !(std::getenv("HCTP_PYRO_EVCLOCK") && std::getenv("HCTP_PYRO_EVCLOCK")[0] == '0'))
    {
        {
            const uint16_t sc = static_cast<uint16_t>(evPk >> 16), fr = static_cast<uint16_t>(evPk & 0xFFFFu);
            evClock = true;
            steps = 0;
            int adv = 0;
            if (g_lastEvScene < 0) adv = 0;
            else if (sc == g_lastEvScene) adv = std::clamp(static_cast<int>(fr) - g_lastEvFrame, 0, 4);
            else adv = 1;
            if (pyroLog() && sc != g_lastEvScene) std::fprintf(stderr, "[pyro] event scene %u frame %u (tick %d)\n", sc, fr, g_tick);
            g_lastEvScene = sc, g_lastEvFrame = fr;
            for (Instance &in : g_inst)
            {
                // SvR04 (g_src 4): GMEV scene = event scene index, passed scenes count (MyAction 0x4de210 'scene > s0').
                // SvR06 keeps the proven Nov 10 rule (== scene or scene*5; Brock 4044 timing Jordan-confirmed).
                auto reached = [&](int s0, int f0) {
                    if (s0 < 0) return false;
                    // Nov 11: sc = RAW camera scene (caller), same rule for SvR04 + SvR06 (HCTP 0x1a9810, SvR04 0x458f60, SvR06 0x4de210)
                    return static_cast<int>(sc) > s0 || (static_cast<int>(sc) == s0 && static_cast<int>(fr) >= f0);
                };
                if (in.started && !in.dead && reached(in.endScene, in.endFrame))
                {
                    in.dead = true;
                    in.vanished = in.endMode != 0;
                    if (pyroLog()) std::fprintf(stderr, "[pyro] %s at event scene %u frame %u (end %d/%d)%c", in.vanished ? "vanish" : "death", sc, fr, in.endScene, in.endFrame, 10);
                }
                if (!in.started && reached(in.evScene, in.evFrame))
                {
                    in.started = true;
                    if (pyroLog()) std::fprintf(stderr, "[pyro] start at event scene %u frame %u (window %d/%d)\n", sc, fr, in.evScene, in.evFrame);
                    continue;
                }
                for (int a = 0; a < adv; ++a)
                    if (in.started && !in.vanished && in.frame < 2000) stepInstance(in);
            }
            g_tick += adv;
        }
    }
    for (int s = 0; s < steps; ++s)
    {
        ++g_tick;
        if (g_gateTick < 0) continue;
        for (Instance &in : g_inst)
        {
            if (!in.started && g_tick >= in.startTick)
            {
                in.started = true;
                if (pyroLog()) std::fprintf(stderr, "[pyro] start instance at tick %d pos %.1f %.1f %.1f\n", g_tick, in.wpos[0], in.wpos[1], in.wpos[2]);
            }
            if (in.started && in.frame < 2000) stepInstance(in);
        }
    }
    bool anyStarted = false;
    for (const Instance &in : g_inst) anyStarted |= in.started;
    if (!anyStarted) return;
    uint8_t *rdram = g_rt->memory().getRDRAM();
    const Cam cam = readCam(rdram);
    if (!cam.ok) return;
    static const float K = envF("HCTP_PYRO_SIZE", 1.0f); // Nov 10: SvR06 CalcOffest 0x1bb110 corners = unit right/up -> half-size = scale (6.0 compensated the old broken axes)
    static const float zMul = envF("HCTP_PYRO_ZMUL", 1.0f); // Nov 11: GS z = (FTOI4(P.z) >> 4) = P.z (XYZF2 packed field;
    // gs_d3d11_backend.cpp GPU char shader zi). Was 16 -> pyro 16x nearer than the wrestlers (drawn over them). [pyroz] log
    // proved pm*V == P and pyro z == HCTP z (both ~16.7M/w).
    static const bool useHctpZ = !(std::getenv("HCTP_PYRO_HCTPZ") && std::getenv("HCTP_PYRO_HCTPZ")[0] == '0');
    g_pmOk = useHctpZ && hctpGetCharProj(g_pm);
    // group by (texture, blend)
    std::map<uint32_t, std::vector<GS::HostTri>> groups;
    std::map<uint32_t, std::pair<uint16_t, uint16_t>> dims;
    // SvR06 ParticleManager::Draw 0x23d320: CalcOffest(camera+0x1d0); CalcCameraInfo 0x1aafe0 sets +0x1d0 =
    // (0,0,1,0) * inverse(cam+0xE0) = view +z in world (= column 2 of E). CalcOffest 0x1bb110: R = norm(d x (0,1,0)),
    // U = R x d, A = U x d; VU gets c0 = -A-U = R-U (0x3EF, vf9) and c3 = A+U = U-R (0x3F0, vf10) through
    // TranslateVUMemCommonData 0x23d5c0.
    float cnr0[3], cnr3[3];
    {
        float d[3] = {cam.E[0][2], cam.E[1][2], cam.E[2][2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl <= 0) return;
        for (float &x : d) x /= dl;
        float R[3] = {-d[2], 0.0f, d[0]}; // d x (0,1,0)
        const float rl = std::sqrt(R[0] * R[0] + R[2] * R[2]);
        if (rl <= 0) return;
        R[0] /= rl, R[2] /= rl;
        const float U[3] = {R[1] * d[2] - R[2] * d[1], R[2] * d[0] - R[0] * d[2], R[0] * d[1] - R[1] * d[0]};
        for (int k = 0; k < 3; ++k) { cnr0[k] = R[k] - U[k]; cnr3[k] = U[k] - R[k]; }
    }
    // (4) culls with HCTP's real values. SvR06 TranslateVUMemCommonData 0x23d5c0: halfW/H = CRender+0x5a8/+0x5ac >> 1;
    // HCTP CRender = *(0x524eb0): width +0x1058c (GetScreenWidth 0x21d9d0), height +0x10590 (read next to it by
    // TelopString/csnTitleDraw). Z guard: VU CLIPw vs 0xFFFFFF on HOMOGENEOUS z in CCalcPerspectiveMatrix space; HCTP has the
    // same class (Updata 0x1a89f0 = CRender+0x10390 * CRender+0x10350), instance = *(0x5262f8) + 0x40
    // (GetCalcPerspectiveMatrix 0x163d40) -> z_hom from that matrix (cam.M fallback).
    float halfW = 320.0f, halfH = 224.0f;
    float PM[4][4];
    bool pmOk = false;
    {
        uint32_t rnd = 0, cdm = 0;
        std::memcpy(&rnd, rdram + 0x524EB0u, 4);
        rnd &= 0x1FFFFFFu;
        if (rnd != 0 && rnd + 0x10600u < 0x2000000u)
        {
            int32_t sw = 0, sh = 0;
            std::memcpy(&sw, rdram + rnd + 0x1058Cu, 4);
            std::memcpy(&sh, rdram + rnd + 0x10590u, 4);
            if (sw >= 256 && sw <= 1024 && sh >= 192 && sh <= 1024) { halfW = static_cast<float>(sw >> 1); halfH = static_cast<float>(sh >> 1); }
        }
        std::memcpy(&cdm, rdram + 0x5262F8u, 4);
        cdm &= 0x1FFFFFFu;
        if (cdm != 0 && cdm + 0x80u < 0x2000000u)
        {
            std::memcpy(PM, rdram + cdm + 0x40u, sizeof(PM));
            pmOk = std::isfinite(PM[3][3]) && (PM[0][0] != 0.0f || PM[1][1] != 0.0f);
        }
        static bool s_logged = false;
        if (pyroLog() && !s_logged)
        {
            s_logged = true;
            float dmax = 0;
            if (pmOk)
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j) dmax = std::max(dmax, std::fabs(PM[i][j] - cam.M[i][j]));
            std::fprintf(stderr, "[pyrocull] CRender %08x half %.0fx%.0f persp %08x ok %d max|PM-camM| %g\n", rnd, halfW, halfH, cdm, pmOk ? 1 : 0, dmax);
        }
    }
    static const bool zGuardOn = !(std::getenv("HCTP_PYRO_ZGUARD") && std::getenv("HCTP_PYRO_ZGUARD")[0] == '0');
    auto zHom = [&](const float v[3]) -> float {
        const float(*Mz)[4] = pmOk ? PM : cam.M;
        return v[0] * Mz[0][2] + v[1] * Mz[1][2] + v[2] * Mz[2][2] + Mz[3][2];
    };
    auto zFail = [&](const float v[3]) { return zGuardOn && zHom(v) > 16777215.0f; }; // CLIPw +z vs |0xFFFFFF|
    // fully outside = SvR06 FCAND 0x042108 (min.x > halfW | max.x < -halfW | min.y > halfH | max.y < -halfH, around 2048)
    auto offScreen = [&](const float X[4], const float Y[4]) {
        const float mnx = std::min(std::min(X[0], X[1]), std::min(X[2], X[3])), mxx = std::max(std::max(X[0], X[1]), std::max(X[2], X[3]));
        const float mny = std::min(std::min(Y[0], Y[1]), std::min(Y[2], Y[3])), mxy = std::max(std::max(Y[0], Y[1]), std::max(Y[2], Y[3]));
        return mnx - 2048.0f > halfW || mxx - 2048.0f < -halfW || mny - 2048.0f > halfH || mxy - 2048.0f < -halfH;
    };
    for (Instance &in : g_inst)
    {
        if (!in.started || in.vanished) continue;
        for (Emitter &em : in.f.em)
            for (Particle &pt : em.ps)
            {
                if (!pt.live) continue;
                ++live;
                if (pt.tex < 0 || pt.tex >= static_cast<int>(em.texIds.size()) || em.texIds[pt.tex] == 0) { ++em.stNoTex; continue; }
                const uint16_t tid = static_cast<uint16_t>(em.texIds[pt.tex]);
                // Nov 10 DRAW PORT: SvR06 VU1 microcode 1:1 (My_dma_Particle = type 1 SPRITE, My_dma_ParticleRot = type 0
                // TRISTRIP, My_dma_ParticlePolygon = type 2 FAN; decoded with tools\vu1dis2.py, notes SVR06_PYRO_COMPAT "DRAW PORT").
                // Colour: Reset 0x272890 sets data+0xA0 = (FF,FF,FF,FF); Updata sends each byte >> 1 (0x2739d0) -> rgb 0x7F.
                const uint8_t a8 = static_cast<uint8_t>(std::clamp(pt.alpha, 0, 255) >> 1);
                if (a8 == 0) continue; // ILW.w vi15, 0(vi5) + IBLEZ: rgba.w <= 0 -> particle skipped
                const uint32_t key = (static_cast<uint32_t>(tid) << 8) | em.blend;
                auto &vec = groups[key];
                dims[key] = {0, 0};
                auto pushQuad = [&](const float X[4], const float Y[4], const float Z[4], const float us[4], const float vs[4], const int (&idx)[2][3]) {
                    ++em.stDraw;
                    for (const auto &tr : idx)
                    {
                        GS::HostTri t{};
                        for (int j = 0; j < 3; ++j)
                        {
                            t.x[j] = X[tr[j]];
                            t.y[j] = Y[tr[j]];
                            t.z[j] = std::clamp<double>(static_cast<double>(Z[tr[j]]) * zMul, 0.0, 16777215.0);
                            t.s[j] = us[tr[j]];
                            t.t[j] = vs[tr[j]];
                        }
                        t.rgba[0] = t.rgba[1] = t.rgba[2] = 0x7F; // colour bytes >> 1 (white 0xFF)
                        t.rgba[3] = a8;
                        vec.push_back(t);
                    }
                };
                if (em.drawType == 2)
                {
                    // POLYGON (My_dma_ParticlePolygon, VU 0x23c8..0x2a10). CParticlePolygonPS2::SetPacketParam 0x27adb0 fills
                    // 0x3F3-0x3F6 ST (0,0)(u,0)(u,v)(0,v) Q=1 and 0x3F7-0x3FA local corners (-1,-1,0,1)(1,-1,0,1)(1,1,0,1)(-1,1,0,1);
                    // CParticlePolygon::Updata 0x2738f0 streams the per-particle matrix data+0x40 (CalcLocalPos 0x272b80:
                    // M40 = S(scale*aspect, scale, scale) * RZ(angle) * M00, row 3 = pos) and raw GetBlurVector (no blur scale:
                    // vf31.z unused here). Per copy: world_i = corner_i * M40, screen_i = world_i * persp / w; z guard = MAX of the
                    // 4 homogeneous z; then corners -= blur (AFTER the first projection, unlike BB/ROT); FAN 0-1-2-3, own z per vertex.
                    // The guard-band clipper (FCAND 0x081204 -> 0x2c90/0x2a48/0x2bb0) only re-cuts quads crossing the screen edge:
                    // a visual no-op for the host renderer (scissor clips) -> not ported.
                    const float s = pt.scale * K;
                    M3 SR = mul(rotZ(pt.angle), pt.base);
                    for (int k = 0; k < 3; ++k) { SR.m[0][k] *= s * pt.aspect; SR.m[1][k] *= s; SR.m[2][k] *= s; }
                    float L[4][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
                    const float bl[3] = {pt.prev[0] - pt.pos[0], pt.prev[1] - pt.pos[1], pt.prev[2] - pt.pos[2]};
                    static const float us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
                    static const int fan[2][3] = {{0, 1, 2}, {0, 2, 3}};
                    for (int cp = 0; cp < em.copies; ++cp)
                    {
                        float X[4], Y[4], Z[4];
                        bool zbad = false, behind = false;
                        for (int c = 0; c < 4; ++c)
                        {
                            float w3[3], v[4];
                            vmul(L[c], SR, w3);
                            for (int k = 0; k < 3; ++k) w3[k] += pt.pos[k];
                            zbad |= zFail(w3);
                            if (!project(cam, w3, v)) { behind = true; continue; }
                            X[c] = v[0], Y[c] = v[1], Z[c] = v[2];
                        }
                        if (zbad) break;                               // IBNE vi1 -> 0x2a18: next particle
                        for (auto &c : L) for (int k = 0; k < 3; ++k) c[k] -= bl[k]; // SUB vf6..9, vf1
                        if (behind || offScreen(X, Y)) continue;      // FCAND 0x042108 -> 0x29f0: next copy
                        pushQuad(X, Y, Z, us, vs, fan);
                    }
                    continue;
                }
                float p[3] = {pt.pos[0], pt.pos[1], pt.pos[2]};
                // vf14 = GetBlurVector (data+0xB0 - pos) * vf22.z; every copy first does vf1 -= vf14
                const float bl[3] = {(pt.prev[0] - pt.pos[0]) * em.blurK, (pt.prev[1] - pt.pos[1]) * em.blurK,
                                     (pt.prev[2] - pt.pos[2]) * em.blurK};
                const bool rot = em.drawType == 0;
                for (int cp = 0; cp < em.copies; ++cp)
                {
                    for (int k = 0; k < 3; ++k) p[k] -= bl[k];
                    // BB: corners pos + vf9*scale / pos + vf10*scale (MULx vf20/vf21 by vf31.x); ROT: unscaled corners
                    const float sc = rot ? 1.0f : pt.scale * K;
                    float q0[3], q3[3], v0[4], v3[4];
                    for (int k = 0; k < 3; ++k) { q0[k] = p[k] + cnr0[k] * sc; q3[k] = p[k] + cnr3[k] * sc; }
                    // corner 0 failing CLIPw +z vs 0xFFFFFF (homogeneous z) branches to the next particle; w <= 0 = host guard
                    if (zFail(q0) || !project(cam, q0, v0)) break;
                    if (!project(cam, q3, v3)) continue;
                    float X[4], Y[4], Z[4];
                    if (!rot)
                    {
                        // SPRITE: v0 = corner0 ST(0,0), v1 = corner3 ST(u,v); GS sprite takes Z from the 2nd vertex
                        X[0] = v0[0], X[1] = v3[0], X[2] = v0[0], X[3] = v3[0];
                        Y[0] = v0[1], Y[1] = v0[1], Y[2] = v3[1], Y[3] = v3[1];
                        Z[0] = Z[1] = Z[2] = Z[3] = v3[2];
                    }
                    else
                    {
                        // ROT: centre = (v0+v3)*0.5; o0 = (v0-centre)*s, o1 = ((v3.x,v0.y)-centre)*s (MULx.xy by vf31.x)
                        const float s2 = pt.scale * K;
                        const float cx = (v0[0] + v3[0]) * 0.5f, cy = (v0[1] + v3[1]) * 0.5f;
                        const float o0x = (v0[0] - cx) * s2, o0y = (v0[1] - cy) * s2;
                        const float o1x = (v3[0] - cx) * s2, o1y = (v0[1] - cy) * s2;
                        // vf9 = (cos,-sin), vf10 = (sin,cos) from ESIN(SeikikaRadian(a)) / ESIN(a + pi/2) (PCSX2 _vuESIN series)
                        const float an = seikikaRadian(pt.angle);
                        const float sn = vuEsin(an), cs = vuEsin(an + 1.57079637f);
                        const float r0x = o0x * cs - o0y * sn, r0y = o0x * sn + o0y * cs;
                        const float r1x = o1x * cs - o1y * sn, r1y = o1x * sn + o1y * cs;
                        X[0] = cx + r0x, X[1] = cx + r1x, X[2] = cx - r1x, X[3] = cx - r0x;
                        Y[0] = cy + r0y, Y[1] = cy + r1y, Y[2] = cy - r1y, Y[3] = cy - r0y;
                        Z[0] = Z[1] = Z[2] = v0[2]; // vf6/vf7 are copies of vf5 (z kept), vf8 keeps its own z
                        Z[3] = v3[2];
                    }
                    // off-screen cull: SvR06 bbox - (2048,2048) vs HCTP screen half size (fully outside -> skip this copy)
                    if (offScreen(X, Y)) continue;
                    static const float us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
                    static const int idx[2][3] = {{0, 1, 2}, {1, 3, 2}};
                    pushQuad(X, Y, Z, us, vs, idx);
                }
            }
    }
    for (auto &g : groups)
    {
        if (g.second.empty()) continue; // Nov 11: every quad of this group culled (crash fix, log path read g.second[0])
        const uint16_t tid = static_cast<uint16_t>(g.first >> 8);
        uint16_t w = 64, h = 64;
        for (const auto &t : g_tex)
            if (t.second.id == tid) { w = static_cast<uint16_t>(t.second.w); h = static_cast<uint16_t>(t.second.h); }
        // (3) SvR06 ParticleManager::Draw 0x23d320 -> SetAlphaTest(1,1) 0x23d480: TEST_1 = 0x53201 (ATE, ATST NEVER, AREF 0x20,
        // AFAIL RGB_ONLY, ZTE, ZTST GEQUAL) = colour only, no Z / FB-alpha write. HCTP_PYRO_ATEST=0 -> old default TEST.
        static const uint64_t atest = (std::getenv("HCTP_PYRO_ATEST") && std::getenv("HCTP_PYRO_ATEST")[0] == '0') ? 0ull : 0x53201ull;
        g_rt->gs().submitHostTris(g.second.data(), g.second.size(), tid, w, h, (g.first & 0xFF) == 1, atest);
    }
    // HCTP_PYRO_EMSTATS=1: every 60 ticks, per started instance + emitter: born / killed by Link / no texture / quads drawn
    static const bool emStats = std::getenv("HCTP_PYRO_EMSTATS") != nullptr;
    static int s_lastStatTick = -1000;
    if (emStats && g_tick - s_lastStatTick >= 60)
    {
        s_lastStatTick = g_tick;
        for (size_t ii = 0; ii < g_inst.size(); ++ii)
        {
            const Instance &in = g_inst[ii];
            if (!in.started) continue;
            for (size_t ei = 0; ei < in.f.em.size(); ++ei)
            {
                const Emitter &em = in.f.em[ei];
                if (!em.stBorn) continue;
                size_t lv = 0;
                for (const Particle &pt : em.ps) lv += pt.live ? 1u : 0u;
                std::fprintf(stderr, "[pyroem] tick %d inst %zu em %zu draw %u blend %u live %zu born %u linkkill %u notex %u quads %u%c",
                             g_tick, ii, ei, em.drawType, em.blend, lv, em.stBorn, em.stLinkKill, em.stNoTex, em.stDraw, 10);
            }
        }
    }
    static int s_logLeft = 40;
    static int s_lastLogTick = -100;
    if (pyroLog() && s_logLeft > 0 && live > 0 && g_tick - s_lastLogTick >= 15)
    {
        s_lastLogTick = g_tick;
        --s_logLeft;
        float c[4] = {0, 0, 0, 0};
        project(cam, g_inst.empty() ? c : g_inst[0].wpos, c);
        std::fprintf(stderr, "[pyro] tick %d live %zu groups %zu post0 -> gs %.0f %.0f z %.0f w %.2f\n", g_tick, live, groups.size(), c[0], c[1], c[2], c[3]);
        for (size_t ii = 0; ii < g_inst.size(); ++ii)
        {
            // Nov 10 back posts: per post screen pos + half size of a K-unit offset along old (M) vs new (E) axes
            float cc[4], o1[4];
            if (!project(cam, g_inst[ii].wpos, cc)) { std::fprintf(stderr, "[pyropost] %zu behind camera\n", ii); continue; }
            float hOld[2] = {0, 0}, hNew[2] = {0, 0};
            for (int ax_ = 0; ax_ < 2; ++ax_)
                for (int m = 0; m < 2; ++m)
                {
                    const float(*AX)[4] = m ? cam.E : cam.M;
                    const float d[3] = {AX[0][ax_], AX[1][ax_], AX[2][ax_]};
                    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (l <= 0) continue;
                    float q[3];
                    for (int k = 0; k < 3; ++k) q[k] = g_inst[ii].wpos[k] + d[k] / l * K;
                    if (!project(cam, q, o1)) continue;
                    (m ? hNew : hOld)[ax_] = std::fabs(o1[ax_] - cc[ax_]);
                }
            std::fprintf(stderr, "[pyropost] %zu world %.1f %.1f %.1f -> gs %.0f %.0f z %.0f w %.2f half old %.1fx%.1f new %.1fx%.1f\n", ii,
                         g_inst[ii].wpos[0], g_inst[ii].wpos[1], g_inst[ii].wpos[2], cc[0], cc[1], cc[2], cc[3], hOld[0], hOld[1], hNew[0], hNew[1]);
        }
        for (auto &g : groups)
        {
            if (g.second.empty()) continue;
            const GS::HostTri &t0 = g.second[0];
            std::fprintf(stderr, "[pyro]   group tex %u blend %u tris %zu quad0 x %.1f..%.1f y %.1f..%.1f z %.0f a %u\n", g.first >> 8, g.first & 0xFF,
                         g.second.size(), t0.x[0], t0.x[1], t0.y[0], t0.y[2], t0.z[0], (unsigned)t0.rgba[3]);
        }
    }
    bool any = false;
    for (const Instance &in : g_inst)
        if (!in.started || in.frame < 1500) any = true;
    if (!any) g_active = false;
}
} // namespace

// Called from sd5EventMngCtEvtk when the CAM entrance-list sentinel picks the pyro row. id = PCEN/GMEV id ("4044").
void hctpPyroSetSource(int game) { g_src = game == 4 ? 4 : 6; }

void hctpPyroArm(PS2Runtime *runtime, const char *id, uint32_t common)
{
    std::lock_guard<std::mutex> lock(g_mx);
    g_rt = runtime;
    g_common = common & 0x1FFFFFFu;
    g_lastEvScene = g_lastEvFrame = -1;
    g_evPacked.store(0xFFFFFFFFu);
    g_seFired.store(false);
    g_armedA.store(false);
    g_winScene.store(-1);
    g_inst.clear();
    g_active = false;
    Bytes pack, gm;
    if (!loadPcen(id, pack) || !loadGmev(id, gm))
    {
        std::fprintf(stderr, "[pyro] MODS\\SVR06\\prtcls.pac PCEN/%s or gm.pac GMEV/%s missing (copy the SvR06 files there)\n", id, id);
        return;
    }
    auto top = parsePac(pack);
    std::map<uint16_t, File> files;
    g_packTex.clear();
    std::map<uint16_t, TexMap> fileTex;
    for (auto &kv : top) // textures first: each entry's sub-entry 1 = that file's own texture set
        if (kv.first < 16000)
        {
            auto sub = parsePac(kv.second);
            if (sub.count(1)) loadTextures(sub[1], fileTex[kv.first]);
        }
    for (auto &kv : top)
        if (kv.first < 16000)
        {
            auto sub = parsePac(kv.second);
            File f;
            if (sub.count(0) && parsePrtc(sub[0], f, fileTex[kv.first])) files[kv.first] = std::move(f);
        }
    auto G = parsePac(gm);
    if (!G.count(0) || !G.count(2) || !G.count(3)) { std::fprintf(stderr, "[pyro] GMEV/%s incomplete\n", id); return; }
    auto T = parsePac(G[3]);
    std::map<uint32_t, std::pair<std::array<float, 3>, std::array<float, 3>>> tags;
    if (T.count(10000))
    {
        const Bytes &tb = T[10000];
        for (size_t o = 16; o + 32 <= tb.size(); o += 32)
            tags[rd32(&tb[o + 12])] = {{rdf(&tb[o]), rdf(&tb[o + 4]), rdf(&tb[o + 8])}, {rdf(&tb[o + 16]), rdf(&tb[o + 20]), rdf(&tb[o + 24])}};
    }
    const int startBase = static_cast<int>(envF("HCTP_PYRO_START", 310.0f));
    const Bytes &g0 = G[0], &g2 = G[2];
    for (size_t o = 0; o + 8 <= g0.size(); o += 8)
    {
        const uint16_t tag = rd16(&g0[o]), type = rd16(&g0[o + 2]), pc = rd16(&g0[o + 4]), po = rd16(&g0[o + 6]);
        if ((type != 1 && type != 5) || pc < 4 || (po + 4u) * 4u > g2.size()) continue;
        const int no = static_cast<int>(rdf(&g2[(po + 1) * 4])), fr = static_cast<int>(rdf(&g2[(po + 3) * 4]));
        const int scn = static_cast<int>(rdf(&g2[(po + 2) * 4]));
        if (!files.count(static_cast<uint16_t>(no)) || !tags.count(tag)) continue;
        Instance in;
        in.f = files[static_cast<uint16_t>(no)];
        const auto &tg = tags[tag];
        for (int k = 0; k < 3; ++k) in.wpos[k] = tg.first[k];
        in.wrot = euler(tg.second[0], tg.second[1], tg.second[2]);
        in.startTick = startBase + (fr - 310);
        static const int fdiv = std::max(1, static_cast<int>(envF("HCTP_PYRO_FRAMEDIV", 1.0f)));
        in.evScene = scn, in.evFrame = fr / fdiv;
        in.src = g_src;
        if (pc >= 7) // params[4..6] = end scene, end frame, mode (gmParticleEmitter::MyAction 0x4de210)
        {
            in.endScene = static_cast<int>(rdf(&g2[(po + 4) * 4]));
            in.endFrame = static_cast<int>(rdf(&g2[(po + 5) * 4])) / fdiv;
            in.endMode = static_cast<int>(rdf(&g2[(po + 6) * 4]));
        }
        if (pyroLog()) std::fprintf(stderr, "[pyro] inst tag %u file %d start %d/%d end %d/%d mode %d%c", tag, no, in.evScene, in.evFrame, in.endScene, in.endFrame, in.endMode, 10);
        if (g_winScene.load() < 0 || in.evFrame < g_winFrame.load()) g_winScene.store(scn), g_winFrame.store(in.evFrame);
        g_startBase = startBase;
        g_inst.push_back(std::move(in));
    }
    g_tick = 0;
    g_variant = -1;
    g_gateTick = -1;
    g_acc = 0;
    g_last = std::chrono::steady_clock::now();
    g_active = !g_inst.empty();
    g_armedA.store(g_active);
    g_hctpPrePresentHook.store(&prePresent, std::memory_order_release);
    std::fprintf(stderr, "[pyro] armed %s: %zu files, %zu textures, %zu instances (start tick %d)\n", id, files.size(), g_tex.size(), g_inst.size(), startBase);
}

// Nov 10 STEP 2: SvR06 event id has a pyro pack (prtcls.pac PCEN/<id> dir entry + gm.pac GMEV/<id>); cached per id.
bool hctpPyroHas(const char *id)
{
    static std::map<std::string, bool> cache;
    std::lock_guard<std::mutex> lock(g_mx);
    const std::string key = std::to_string(g_src) + ":" + id;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    bool has = false;
    if (g_src == 4)
    {
        Bytes pc, gm;
        has = dpacFind("MODS/SVR04/prtcls.pac", id, pc) && dpacFind("MODS/SVR04/gm.pac", id, gm);
        cache[key] = has;
        std::fprintf(stderr, "[pyro] SvR04 %s pyro pack: %s\n", id, has ? "yes" : "no");
        return has;
    }
    {
        std::ifstream f("MODS/SVR06/prtcls.pac", std::ios::binary);
        Bytes dir;
        if (f && readAt(f, 0x808, 41 * 8, dir))
            for (int i = 0; i < 41 && !has; ++i)
                has = std::memcmp(&dir[i * 8], id, 4) == 0;
    }
    Bytes gm;
    if (has) has = loadGmev(id, gm);
    cache[key] = has;
    std::fprintf(stderr, "[pyro] SvR06 %s pyro pack: %s\n", id, has ? "yes" : "no");
    return has;
}

void hctpPyroDisarm()
{
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_active && pyroLog()) std::fprintf(stderr, "[pyro] disarmed\n");
    g_active = false;
    g_armedA.store(false);
    g_evPacked.store(0xFFFFFFFFu);
    g_inst.clear();
}

// Build 10: called every CEventSystem::Action of a playing SvR06 reader event (scene u16 +0x18, frame u16 +0x1A,
// frame max +0x1E). Returns true ONCE when the first pyro window is reached (caller plays the pyro SE).
bool hctpPyroEventTick(uint32_t scene, uint32_t frame, uint32_t frameMax)
{
    if (!g_armedA.load(std::memory_order_relaxed)) return false;
    static uint32_t lastSc = 0xFFFFu;
    if (scene != lastSc)
    {
        lastSc = scene;
        std::fprintf(stderr, "[pyro] build10 event scene %u frame %u max %u (window %d/%d)%c", scene, frame, frameMax,
                     g_winScene.load(), g_winFrame.load(), 10);
    }
    g_evPacked.store(((scene & 0xFFFFu) << 16) | (frame & 0xFFFFu), std::memory_order_relaxed);
    const int ws = g_winScene.load();
    if (ws >= 0 && (static_cast<int>(scene) > ws || (static_cast<int>(scene) == ws && static_cast<int>(frame) >= g_winFrame.load())) &&
        !g_seFired.exchange(true))
        return true;
    return false;
}

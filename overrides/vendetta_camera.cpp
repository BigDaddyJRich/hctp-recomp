// Def Jam Vendetta (USA, SLUS-20639) - the fight camera's styles (VENDETTA_CAMERA; settings.toml graphics.camera).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The match camera FUN_001037a0 runs every match frame. In normal play (no camera mode from a fighter, no scripted
// camera) it calls the default fight camera FUN_001039e0, then the screen shake and the projection FUN_001a1768(the
// field of view 0x22B468). FUN_001039e0 is a side-on camera: it never turns, it looks along -z from the +z side.
//   1. The fighters it follows: every fighter in a 1-on-1 or a 2-on-2 (match kind 0x2361CE & 0xC == 0); in a 3- or
//      4-fighter match the ones still in it (+0x5BB bit 4 clear; with fewer than 3 left, a fighter followed once,
//      +0x5B4 bit 15, stays followed).
//   2. Their box: each fighter's point (+0xCC.. while +0x40 is 0-2, the hips +0x15C.. otherwise; +0x40 = 8 mixes the
//      two) widened 10 up and down.
//   3. The tilt: 12 degrees down, 35 when a fighter in the ring (+0x5AC bit 4) is near the front or back ropes
//      (z < -41 or z > 20.5); eased 5% a frame (0x22B474).
//   4. The aim (0x22B478): the box's middle, moved towards the front by its depth x tilt / 90 x 1.7; eased half way
//      (x, z) and 15% (y) a frame.
//   5. The range (0x22B470): the box's half-size, between 20 (14 with two fighters) and 75; eased 20% a frame when it
//      moves 0.25 or more. The camera stands range / tan(fov / 2) back from the aim, tilted.
//   6. The arena: the bottom of the view meets the floor at most at z 100, the sides at most x +-75 (aspect 1.333),
//      the camera at most 140 high; the aim and the camera move together to keep them.
//   7. The camera (0x22FD18 position, 0x22FD24 direction): eased 75% (x, z) and 50% (y) a frame, or straight there
//      when the direction's height hasn't changed; straight there after a cut (0x22B484 bits 0-2, then cleared).
// Controls: the game's directions belong to the arena, not the camera (up = away from its front view): the pad read
// FUN_00132a50 turns the left stick into direction bits (fighter +0x668: 0x1 up, 0x2 right, 0x4 down, 0x8 left) by its
// angle alone, and 0x258084 (the camera's angle) is only used for drawing. So with a camera from a corner the pad read
// is followed by a turn of those bits and the left stick (+0x674 X, +0x678 Y up) by the camera's angle, while the fight
// camera is the one in use: up on the pad is up on the screen. Fighters the computer plays skip the pad read (+0x5BC bit
// 0), so they are never turned.
//
// "vendetta" (the default) keeps the game's own function. The other styles run this file's port of it with their own
// settings: their own tilt and closeness, and which way the camera faces the ring - from the front as the game's own,
// or from a corner (the ring a diamond). None of them goes round the fighters (only the scripted close-ups of moves
// do, and those stay the game's): the camera slides with the action, facing the same way all match. The work is done
// in a frame turned to the camera's direction (the fighters turned in, the result turned back), so the game's own
// steps, its arena limits included, hold for any direction. VENDETTA_CAMERA_VERIFY=1 runs the
// port with the game's own settings next to the original and reports the largest difference (the game keeps the
// original's result). Scripted cameras (intros, DEMO scenes, camera modes 1-5 and 99) stay the game's.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <atomic>
#include <chrono>
#include <string>

namespace vendetta_camera
{
    constexpr uint32_t kFightCamera = 0x1039E0u;
    constexpr uint32_t kFighters = 0x22FE08u;
    constexpr uint32_t kFighterStride = 0xCD8u;
    constexpr uint32_t kMatchKind = 0x2361CEu;  // byte: & 1 two fighters, & 0xC three or four
    constexpr uint32_t kFov = 0x22B468u;        // degrees
    constexpr uint32_t kRoll = 0x22B46Cu;
    constexpr uint32_t kRange = 0x22B470u;
    constexpr uint32_t kTilt = 0x22B474u;
    constexpr uint32_t kAim = 0x22B478u;        // x, y, z
    constexpr uint32_t kFlags = 0x22B484u;      // u16: bits 0-2 = a cut (no easing this frame)
    constexpr uint32_t kCamera = 0x22FD18u;     // position x, y, z; direction x, y, z
    constexpr uint32_t kCameraRoll = 0x22FD50u;
    constexpr uint32_t kEasedPos = 0x22FD30u;   // x, y, z
    constexpr uint32_t kEasedDir = 0x22FD3Cu;   // x, y, z
    constexpr float kPi = 3.14159274f;

    /// A camera style: the game's numbers by default.
    struct Rig
    {
        const char *name = "vendetta";
        float tilt = -12.0f;          // degrees, negative = looking down
        float tiltRopes = -35.0f;     // a fighter near the front or back ropes
        float ropesBack = -41.0f;
        float ropesFront = 20.5f;
        float tiltEase = 0.05f;
        float aimForward = 1.7f;      // how far the aim moves to the front with the tilt
        float aimEaseXZ = 0.5f;
        float aimEaseY = 0.15f;
        float rangeMin = 20.0f;
        float rangeMinTwo = 14.0f;    // with two fighters
        float rangeMax = 75.0f;
        float rangeEase = 0.2f;
        float rangeDeadband = 0.25f;
        float distance = 1.0f;        // x the framing distance
        float floorZ = 100.0f;        // the bottom of the view meets the floor at most here
        float sideX = 75.0f;          // the view's sides at most here
        float aspect = 1.333f;
        float maxY = 140.0f;
        float cameraEaseXZ = 0.75f;
        float cameraEaseY = 0.5f;
        float facing = 0.0f;          // degrees round the ring from the game's own front view (45 = from a corner)
    };

    Rig g_rig;
    constexpr uint32_t kPadRead = 0x132A50u;       // FUN_00132a50(fighter): the fighter's pad, once a frame
    constexpr uint32_t kMatchPadFlags = 0x22FDE4u; // u16: bit 6 = no pad reading
    PS2Runtime::RecompiledFunction g_originalPadRead = nullptr;
    std::atomic<int64_t> g_fightCameraAt{0};       // when the fight camera last ran (steady clock, ms)
    bool g_verify = false;
    PS2Runtime::RecompiledFunction g_original = nullptr;

    float readF(const uint8_t *rdram, uint32_t address)
    {
        const uint32_t bits = Ps2FastRead32(rdram, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void writeF(uint8_t *rdram, uint32_t address, float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        Ps2FastWrite32(rdram, address, bits);
    }

    float degTan(float degrees) { return std::tan(degrees / 180.0f * kPi); }

    struct V3
    {
        float x, y, z;
    };

    float length(V3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

    /// A point turned about the vertical by an angle (sin, cos): into the camera's frame with -angle, back with +angle.
    V3 turned(V3 v, float sinA, float cosA) { return {v.x * cosA + v.z * sinA, v.y, -v.x * sinA + v.z * cosA}; }

    /// The port of FUN_001039e0 with a rig's numbers (the comment at the top, step by step).
    void fightCamera(uint8_t *rdram, const Rig &rig)
    {
        const float facing = rig.facing / 180.0f * kPi;
        const float faceSin = std::sin(facing), faceCos = std::cos(facing);
        const auto toFrame = [&](V3 v) { return rig.facing == 0.0f ? v : turned(v, -faceSin, faceCos); };
        const auto toWorld = [&](V3 v) { return rig.facing == 0.0f ? v : turned(v, faceSin, faceCos); };
        const uint16_t flags = Ps2FastRead16(rdram, kFlags);
        const bool eased = (flags & 7u) == 0u;
        const uint8_t kind = Ps2FastRead8(rdram, kMatchKind);

        // 1. the fighters followed
        int stillIn = 0;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const uint32_t f = kFighters + i * kFighterStride;
            if (static_cast<int32_t>(Ps2FastRead32(rdram, f + 0x30u)) != -1 && (Ps2FastRead8(rdram, f + 0x5BBu) & 0x10u) == 0u)
                ++stillIn;
        }
        bool followed[4] = {};
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const uint32_t f = kFighters + i * kFighterStride;
            if (static_cast<int32_t>(Ps2FastRead32(rdram, f + 0x30u)) == -1)
                continue;
            const bool out = (Ps2FastRead8(rdram, f + 0x5BBu) & 0x10u) != 0u;
            if ((kind & 0xCu) == 0u)
                followed[i] = true;
            else if (stillIn < 3)
            {
                const uint16_t marks = Ps2FastRead16(rdram, f + 0x5B4u);
                if (!out)
                {
                    followed[i] = true;
                    Ps2FastWrite16(rdram, f + 0x5B4u, static_cast<uint16_t>(marks | 0x8000u));
                }
                else if ((marks & 0x8000u) != 0u)
                    followed[i] = true;
            }
            else if (!out)
                followed[i] = true;
        }

        // 2. their box (and 3. who is near the ropes)
        V3 lo{1000.0f, 1000.0f, 1000.0f};
        V3 hi{-1000.0f, -1000.0f, -1000.0f};
        bool nearRopes = false;
        V3 points[4] = {};
        int count = 0;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            if (!followed[i])
                continue;
            const uint32_t f = kFighters + i * kFighterStride;
            const float z0 = readF(rdram, f + 0xD4u);
            if ((Ps2FastRead16(rdram, f + 0x5ACu) & 0x10u) != 0u && (z0 < rig.ropesBack || z0 > rig.ropesFront))
                nearRopes = true;
            const int32_t state = static_cast<int32_t>(Ps2FastRead32(rdram, f + 0x40u));
            V3 p;
            if (state >= 0 && state < 3)
                p = {readF(rdram, f + 0xCCu), readF(rdram, f + 0xD0u) + 10.0f, z0};
            else if (state == 8)
                p = {readF(rdram, f + 0x15Cu), readF(rdram, f + 0xD0u) + 10.0f, readF(rdram, f + 0x164u)};
            else
                p = {readF(rdram, f + 0x15Cu), readF(rdram, f + 0x160u), readF(rdram, f + 0x164u)};
            p = toFrame(p);
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y - 10.0f), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y + 10.0f), std::max(hi.z, p.z)};
            if (count < 4)
                points[count++] = p;
        }

        // 3. the tilt
        float tilt = nearRopes ? rig.tiltRopes : rig.tilt;
        if (eased)
        {
            const float before = readF(rdram, kTilt);
            tilt = before + (tilt - before) * rig.tiltEase;
        }
        writeF(rdram, kTilt, tilt);
        const float tiltAbs = std::fabs(tilt);

        // 4. the aim
        V3 aim{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, hi.z - (hi.z - lo.z) * 0.5f * (tiltAbs / 90.0f) * rig.aimForward};
        if (eased)
        {
            const V3 before = toFrame({readF(rdram, kAim), readF(rdram, kAim + 4u), readF(rdram, kAim + 8u)});
            aim = {before.x + (aim.x - before.x) * rig.aimEaseXZ, before.y + (aim.y - before.y) * rig.aimEaseY,
                   before.z + (aim.z - before.z) * rig.aimEaseXZ};
        }

        // 5. the range and the distance
        const float reachHi = length({hi.x - aim.x, hi.y - aim.y, hi.z - aim.z});
        const float reachLo = length({lo.x - aim.x, lo.y - aim.y, lo.z - aim.z});
        float reach = std::min(reachHi, reachLo);
        reach = std::min(reach, rig.rangeMax);
        reach = std::max(reach, (kind & 1u) != 0u ? rig.rangeMinTwo : rig.rangeMin);
        float range = readF(rdram, kRange);
        if (std::fabs(reach - range) >= rig.rangeDeadband)
            range += (reach - range) * rig.rangeEase;
        writeF(rdram, kRange, range);
        const float fov = readF(rdram, kFov);
        const float distance = range / degTan(fov * 0.5f) * rig.distance;
        // the offset (0, 0, distance) tilted
        V3 pos{aim.x, aim.y - distance * std::sin(tilt / 180.0f * kPi), aim.z + distance * std::cos(tilt / 180.0f * kPi)};

        // 6. the arena
        const float groundAhead = (pos.y - lo.y) / degTan(tiltAbs + fov * 0.5f);
        float floorZ = pos.z - groundAhead;
        if (floorZ > rig.floorZ)
        {
            const float shift = floorZ - rig.floorZ;
            aim.z -= shift;
            pos.z -= shift;
            floorZ = rig.floorZ;
        }
        const float halfWidth = (pos.z - floorZ) * degTan(fov * 0.5f) * rig.aspect;
        if (pos.x + halfWidth > rig.sideX)
        {
            const float shift = rig.sideX - (pos.x + halfWidth);
            aim.x += shift;
            pos.x += shift;
        }
        if (pos.x - halfWidth < -rig.sideX)
        {
            const float shift = -rig.sideX - (pos.x - halfWidth);
            aim.x += shift;
            pos.x += shift;
        }
        // back from the camera's frame
        aim = toWorld(aim);
        pos = toWorld(pos);
        writeF(rdram, kAim, aim.x);
        writeF(rdram, kAim + 4u, aim.y);
        writeF(rdram, kAim + 8u, aim.z);
        pos.y = std::min(pos.y, rig.maxY);

        // 7. the camera
        V3 cam = pos;
        if (eased)
        {
            V3 dir{aim.x - pos.x, aim.y - pos.y, aim.z - pos.z};
            const float n = length(dir);
            dir = {dir.x / n, dir.y / n, dir.z / n};
            if (std::fabs(readF(rdram, kEasedDir + 4u) - dir.y) >= 1e-5f)
            {
                const V3 before{readF(rdram, kEasedPos), readF(rdram, kEasedPos + 4u), readF(rdram, kEasedPos + 8u)};
                cam = {before.x + (pos.x - before.x) * rig.cameraEaseXZ, before.y + (pos.y - before.y) * rig.cameraEaseY,
                       before.z + (pos.z - before.z) * rig.cameraEaseXZ};
            }
        }
        writeF(rdram, kEasedPos, cam.x);
        writeF(rdram, kEasedPos + 4u, cam.y);
        writeF(rdram, kEasedPos + 8u, cam.z);
        V3 dir{aim.x - cam.x, aim.y - cam.y, aim.z - cam.z};
        const float n = length(dir);
        dir = {dir.x / n, dir.y / n, dir.z / n};
        writeF(rdram, kEasedDir, dir.x);
        writeF(rdram, kEasedDir + 4u, dir.y);
        writeF(rdram, kEasedDir + 8u, dir.z);
        writeF(rdram, kCamera, cam.x);
        writeF(rdram, kCamera + 4u, cam.y);
        writeF(rdram, kCamera + 8u, cam.z);
        writeF(rdram, kCamera + 12u, dir.x);
        writeF(rdram, kCamera + 16u, dir.y);
        writeF(rdram, kCamera + 20u, dir.z);
        Ps2FastWrite32(rdram, kCameraRoll, Ps2FastRead32(rdram, kRoll));
        Ps2FastWrite16(rdram, kFlags, static_cast<uint16_t>(flags & 0xFFF8u));
    }

    // --- VENDETTA_CAMERA_VERIFY: the port with the game's numbers next to the original ---

    struct Snapshot
    {
        uint8_t low[0x70];                 // 0x22B468 .. 0x22B4D7 (the camera's state)
        uint8_t high[0x40];                // 0x22FD18 .. 0x22FD57 (the camera)
        uint16_t marks[4];                 // each fighter's +0x5B4
    };

    void take(const uint8_t *rdram, Snapshot &s)
    {
        std::memcpy(s.low, rdram + 0x22B468u, sizeof(s.low));
        std::memcpy(s.high, rdram + 0x22FD18u, sizeof(s.high));
        for (uint32_t i = 0; i < 4u; ++i)
            s.marks[i] = Ps2FastRead16(rdram, kFighters + i * kFighterStride + 0x5B4u);
    }

    void put(uint8_t *rdram, const Snapshot &s)
    {
        std::memcpy(rdram + 0x22B468u, s.low, sizeof(s.low));
        std::memcpy(rdram + 0x22FD18u, s.high, sizeof(s.high));
        for (uint32_t i = 0; i < 4u; ++i)
            Ps2FastWrite16(rdram, kFighters + i * kFighterStride + 0x5B4u, s.marks[i]);
    }

    float largestDifference(const Snapshot &a, const Snapshot &b)
    {
        float worst = 0.0f;
        const auto floats = [&](const uint8_t *x, const uint8_t *y, size_t bytes) {
            for (size_t i = 0; i + 4 <= bytes; i += 4)
            {
                float fx, fy;
                std::memcpy(&fx, x + i, 4);
                std::memcpy(&fy, y + i, 4);
                if (std::isfinite(fx) && std::isfinite(fy))
                    worst = std::max(worst, std::fabs(fx - fy));
            }
        };
        floats(a.low, b.low, sizeof(a.low));
        floats(a.high, b.high, sizeof(a.high));
        for (int i = 0; i < 4; ++i)
            if (a.marks[i] != b.marks[i])
                worst = std::max(worst, 1000.0f);
        return worst;
    }

    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /// The pad read, then (camera from a corner, the fight camera in use, a fighter read from a pad) its directions turned
    /// to the screen: a direction on the pad (right, up) becomes the arena's (right, up) seen from the camera's angle.
    void padRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fighter = getRegU32(ctx, 4);
        const bool reads = (Ps2FastRead16(rdram, kMatchPadFlags) & 0x40u) == 0u && (Ps2FastRead8(rdram, fighter + 0x5BCu) & 1u) == 0u;
        g_originalPadRead(rdram, ctx, runtime);
        if (!reads || nowMs() - g_fightCameraAt.load() > 150)
            return;
        const float angle = g_rig.facing / 180.0f * kPi;
        const float c = std::cos(angle), sn = std::sin(angle);
        // the direction bits
        uint32_t held = Ps2FastRead32(rdram, fighter + 0x668u);
        const uint32_t dirs = held & 0xFu;
        if (dirs != 0u)
        {
            const float right = ((dirs & 0x2u) ? 1.0f : 0.0f) - ((dirs & 0x8u) ? 1.0f : 0.0f);
            const float up = ((dirs & 0x1u) ? 1.0f : 0.0f) - ((dirs & 0x4u) ? 1.0f : 0.0f);
            const float r = right * c - up * sn;
            const float u = right * sn + up * c;
            const float n = std::sqrt(r * r + u * u);
            uint32_t turned = 0u;
            if (n > 0.0f)
            {
                if (r > 0.38f * n) turned |= 0x2u;
                if (r < -0.38f * n) turned |= 0x8u;
                if (u > 0.38f * n) turned |= 0x1u;
                if (u < -0.38f * n) turned |= 0x4u;
            }
            held = (held & ~0xFu) | turned;
            Ps2FastWrite32(rdram, fighter + 0x668u, held);
            Ps2FastWrite32(rdram, fighter + 0x66Cu, (held ^ Ps2FastRead32(rdram, fighter + 0x670u)) & held);
        }
        // the left stick
        const float x = readF(rdram, fighter + 0x674u);
        const float y = readF(rdram, fighter + 0x678u);
        writeF(rdram, fighter + 0x674u, x * c - y * sn);
        writeF(rdram, fighter + 0x678u, x * sn + y * c);
    }

    void replacement(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_fightCameraAt.store(nowMs());
        if (g_verify)
        {
            static uint64_t frames = 0;
            static float worst = 0.0f;
            Snapshot before, original, port;
            take(rdram, before);
            g_original(rdram, ctx, runtime);
            take(rdram, original);
            put(rdram, before);
            fightCamera(rdram, Rig{});
            take(rdram, port);
            put(rdram, original); // the game goes on with the original's camera
            worst = std::max(worst, largestDifference(original, port));
            if (++frames % 600u == 0u)
            {
                std::cerr << "[camera] verify: " << frames << " frames, the largest difference " << worst << std::endl;
                worst = 0.0f;
            }
            return;
        }
        fightCamera(rdram, g_rig);
        ctx->pc = getRegU32(ctx, 31);
    }

    Rig rigFor(const std::string &style)
    {
        Rig rig;
        if (style == "nomercy")
        {
            // In the style of WWF No Mercy (N64, AKI 2000): in front of the ring like the game's own, sliding with the
            // action, but looking down more (about 25 degrees: much more of the ring's floor), a little further back
            // when the fighters are close together, and the whole ring when they're apart.
            rig.name = "nomercy";
            rig.tilt = -24.0f;
            rig.tiltRopes = -30.0f;
            rig.rangeMin = 22.0f;
            rig.rangeMinTwo = 22.0f;
            rig.rangeMax = 80.0f;
            rig.rangeEase = 0.25f;
        }
        else if (style == "wk2")
        {
            // In the style of Wrestle Kingdom 2 (PS2, Yuke's 2007): No Mercy's camera, watching from a corner of the
            // ring all match (the ring a diamond on screen), with the controls turned to match.
            rig = rigFor("nomercy");
            rig.name = "wk2";
            rig.facing = 45.0f;
        }
        return rig;
    }

    void install(PS2Runtime &runtime)
    {
        const char *style = std::getenv("VENDETTA_CAMERA");
        const char *verify = std::getenv("VENDETTA_CAMERA_VERIFY");
        g_verify = verify && *verify && verify[0] != '0';
        const std::string chosen = style && *style ? style : "vendetta";
        if (chosen == "vendetta" && !g_verify)
            return; // the game's own camera, untouched
        g_original = runtime.lookupFunction(kFightCamera);
        if (!g_original)
        {
            std::cerr << "[camera] the fight camera 0x1039E0 isn't recompiled: the game's own camera" << std::endl;
            return;
        }
        g_rig = rigFor(chosen);
        runtime.replaceFunction(kFightCamera, replacement);
        if (g_rig.facing != 0.0f && !g_verify)
        {
            g_originalPadRead = runtime.lookupFunction(kPadRead);
            if (g_originalPadRead)
                runtime.replaceFunction(kPadRead, padRead);
        }
        std::cerr << "[camera] " << (g_verify ? "verify: the port against the game's own camera" : "style: ")
                  << (g_verify ? "" : g_rig.name) << std::endl;
    }
}

void installVendettaCamera(PS2Runtime &runtime) { vendetta_camera::install(runtime); }

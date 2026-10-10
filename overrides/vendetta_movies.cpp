// Def Jam Vendetta (USA, SLUS-20639) — the movie player's picture flips, watched (VENDETTA_MOVIELOG=1).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// EA's movie run loop 0x1BB7B0 double-buffers the picture in two 640 x 480 buffers (FBP 0 and 0x96): each pass that
// has a new frame uploads it with 0x1BB2A0 (40 GIF transfers of a 16-pixel column) into the buffer the frame counter
// (gp - 0x1C00) picks — even = FBP 0, odd = 0x96 — then waits in 0x1BB710 for the armed VBlank-start handler 0x1BB698
// to show the *other* buffer, then counts on and re-arms (0x1BB768). So the next upload targets the buffer still on
// screen until the next VBlank: if two frames come within one VBlank, the second is written into the picture shown.
//
// Only entries are hooked: a recompiled routine can stop at a scheduler checkpoint and be resumed mid-way by the
// dispatcher, so code after the original call would not run reliably. The upload ends when 0x1BB710 is entered.
// Per movie (printed when the player is freed, 0x1BB570): frames, VBlanks between uploads, frames on screen for how
// many VBlanks, frames never shown, uploads that started into the shown buffer, VBlanks that came mid-upload and left
// the buffer being written on screen (a torn picture reaching the window — the window presents after the handler).
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace vendetta_movies
{
    constexpr uint32_t kUpload = 0x1BB2A0u;
    constexpr uint32_t kWaitFlip = 0x1BB710u;
    constexpr uint32_t kFlipHandler = 0x1BB698u;
    constexpr uint32_t kFreePlayer = 0x1BB570u;
    constexpr uint32_t kFlipArmed = 0x258144u;     // DAT_00258144: the handler shows a buffer only while set
    constexpr uint32_t kCounterGpOffset = 0x1C00u; // the frame counter at gp - 0x1C00

    PS2Runtime::RecompiledFunction g_originalUpload = nullptr;
    PS2Runtime::RecompiledFunction g_originalWaitFlip = nullptr;
    PS2Runtime::RecompiledFunction g_originalFlipHandler = nullptr;
    PS2Runtime::RecompiledFunction g_originalFreePlayer = nullptr;

    struct Stats
    {
        uint32_t uploads = 0;
        uint32_t vblanks = 0;
        uint32_t intoShown = 0;    // uploads that started into the buffer on screen
        uint32_t vblankMidUpload = 0;
        uint32_t tornShown = 0;    // a VBlank mid-upload left the buffer being written on screen
        uint32_t neverShown = 0;   // frames replaced before any VBlank showed them
        uint32_t gaps[6] = {};     // VBlanks between consecutive uploads: 0..4, 5+
        uint32_t onScreen[6] = {}; // VBlanks a frame stayed on screen: 0..4, 5+
    };
    Stats g_stats;
    bool g_uploading = false;
    int g_target = -1;
    uint32_t g_vblankAtLastUpload = 0;
    int g_bufferFrame[2] = {-1, -1}; // the frame each buffer holds (0 = FBP 0, 1 = FBP 0x96)
    int g_shownFrame = -1;
    uint32_t g_shownSince = 0;

    uint32_t read32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + (address & 0x1FFFFFFu), 4);
        return value;
    }

    int shownBuffer(PS2Runtime *runtime)
    {
        const GSRegisters &gs = runtime->memory().gs();
        const uint64_t dispfb = (gs.pmode & 2u) ? gs.dispfb2 : gs.dispfb1;
        const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        return fbp == 0u ? 0 : fbp == 0x96u ? 1 : -1;
    }

    // VENDETTA_MOVIEDUMP=<folder>: every 30th uploaded picture as a PPM, as the upload reads it from guest memory
    // (the frame: +6 = height, +0x10 = 40 columns of 16 x height RGBA32 pixels)
    void dumpPicture(const uint8_t *rdram, uint32_t frame, uint32_t index)
    {
        static const char *dir = std::getenv("VENDETTA_MOVIEDUMP");
        if (!dir || !*dir || index % 30u != 0u)
            return;
        int16_t height = 0;
        std::memcpy(&height, rdram + ((frame + 6u) & 0x1FFFFFFu), 2);
        if (height <= 0 || height > 512)
            return;
        static uint32_t movie = 0;
        if (index == 0u)
            ++movie;
        const std::string path = std::string(dir) + "/movie" + std::to_string(movie) + "-upload" + std::to_string(index) + ".ppm";
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n640 %d\n255\n", height);
        const uint32_t pixels = (frame + 0x10u) & 0x1FFFFFFu;
        for (int y = 0; y < height; ++y)
            for (uint32_t x = 0; x < 640u; ++x)
                std::fwrite(rdram + pixels + ((x / 16u) * static_cast<uint32_t>(height) * 16u + y * 16u + x % 16u) * 4u, 1, 3, f);
        std::fclose(f);
    }

    void upload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Stats &s = g_stats;
        const uint32_t counter = read32(rdram, getRegU32(ctx, 28) - kCounterGpOffset);
        g_target = static_cast<int>(counter & 1u);
        if (s.uploads > 0u)
            ++s.gaps[std::min<uint32_t>(s.vblanks - g_vblankAtLastUpload, 5u)];
        g_vblankAtLastUpload = s.vblanks;
        if (g_target == shownBuffer(runtime))
            ++s.intoShown;
        const int replaced = g_bufferFrame[g_target];
        if (replaced >= 0 && replaced > g_shownFrame)
            ++s.neverShown;
        g_bufferFrame[g_target] = static_cast<int>(s.uploads);
        dumpPicture(rdram, getRegU32(ctx, 4), s.uploads);
        ++s.uploads;
        g_uploading = true;
        g_originalUpload(rdram, ctx, runtime);
    }

    void waitFlip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_uploading = false;
        g_originalWaitFlip(rdram, ctx, runtime);
    }

    void flipHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Stats &s = g_stats;
        ++s.vblanks;
        if (read32(rdram, kFlipArmed) != 0u)
        {
            // what the handler is about to show: even counter = FBP 0x96, odd = FBP 0
            const uint32_t counter = read32(rdram, getRegU32(ctx, 28) - kCounterGpOffset);
            const int shows = (counter & 1u) == 0u ? 1 : 0;
            if (g_uploading)
            {
                ++s.vblankMidUpload;
                if (shows == g_target)
                    ++s.tornShown;
            }
            const int frame = g_bufferFrame[shows];
            if (frame != g_shownFrame)
            {
                if (g_shownFrame >= 0)
                    ++s.onScreen[std::min<uint32_t>(s.vblanks - g_shownSince, 5u)];
                g_shownFrame = frame;
                g_shownSince = s.vblanks;
            }
        }
        g_originalFlipHandler(rdram, ctx, runtime);
    }

    void freePlayer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Stats &s = g_stats;
        if (s.uploads > 0u)
        {
            std::cerr << "[movie] " << s.uploads << " frames, " << s.vblanks << " vblanks; vblanks between uploads:"
                      << " 0=" << s.gaps[0] << " 1=" << s.gaps[1] << " 2=" << s.gaps[2] << " 3=" << s.gaps[3]
                      << " 4=" << s.gaps[4] << " 5+=" << s.gaps[5] << "; vblanks on screen: 1=" << s.onScreen[1]
                      << " 2=" << s.onScreen[2] << " 3=" << s.onScreen[3] << " 4=" << s.onScreen[4]
                      << " 5+=" << s.onScreen[5] << "; never shown " << s.neverShown << "; started into the shown buffer "
                      << s.intoShown << "; a vblank mid-upload " << s.vblankMidUpload << " (left the half-written buffer shown "
                      << s.tornShown << ")" << std::endl;
        }
        g_stats = Stats{};
        g_uploading = false;
        g_bufferFrame[0] = g_bufferFrame[1] = -1;
        g_shownFrame = -1;
        g_vblankAtLastUpload = g_shownSince = 0;
        g_originalFreePlayer(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        const char *on = std::getenv("VENDETTA_MOVIELOG");
        const char *dump = std::getenv("VENDETTA_MOVIEDUMP");
        if ((!on || !*on || *on == '0') && (!dump || !*dump))
            return;
        g_originalUpload = runtime.lookupFunction(kUpload);
        g_originalWaitFlip = runtime.lookupFunction(kWaitFlip);
        g_originalFlipHandler = runtime.lookupFunction(kFlipHandler);
        g_originalFreePlayer = runtime.lookupFunction(kFreePlayer);
        if (!g_originalUpload || !g_originalWaitFlip || !g_originalFlipHandler || !g_originalFreePlayer)
        {
            std::cerr << "[movie] VENDETTA_MOVIELOG: the movie routines were not found" << std::endl;
            return;
        }
        runtime.replaceFunction(kUpload, upload);
        runtime.replaceFunction(kWaitFlip, waitFlip);
        runtime.replaceFunction(kFlipHandler, flipHandler);
        runtime.replaceFunction(kFreePlayer, freePlayer);
    }
}

void installVendettaMovies(PS2Runtime &runtime) { vendetta_movies::install(runtime); }

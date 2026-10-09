// Def Jam Vendetta (USA, SLUS-20639) — runtime overrides for this game build (PS2Recomp game-override module).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/ps2_host_profile.h"

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

void installVendettaAnimations(PS2Runtime &runtime); // vendetta_animations.cpp: body animations from mods\ (a folder)

namespace
{
    // The game reads the disc by raw sector number through sceCdRead (sector 16 = the ISO-9660 volume descriptor,
    // then its files and DATA.BIN by sector), which the runtime can only answer from a disc image — a folder of
    // extracted files isn't enough. Use disc/SLUS-20639.iso (written by tools/chd_to_iso.py) next to game/.
    // VENDETTA_THREADS=1: every 5 s, print the guest threads (pc / ra / status / what they wait on) and the
    // semaphores to stderr � the debug window's Threads tab, readable from a log.
    void startThreadDump(PS2Runtime &runtime)
    {
        if (!std::getenv("VENDETTA_THREADS"))
        {
            return;
        }
        PS2Runtime *rt = &runtime;
        std::thread([rt]()
                    {
                        for (int n = 1;; ++n)
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(5));
                            const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                            std::cerr << "[threads] t=" << n * 5 << "s running=" << snap.runningThreadId << std::hex << std::endl;
                            for (const EeThreadSnapshot &t : snap.threads)
                            {
                                std::cerr << "[threads]   id=" << std::dec << t.id << std::hex << " entry=0x" << t.entry
                                          << " pc=0x" << t.pc << " ra=0x" << t.ra << " prio=" << std::dec << t.currentPriority
                                          << " status=" << static_cast<int>(t.status) << " wait=" << static_cast<int>(t.waitReason)
                                          << " waitId=" << t.waitId << std::endl;
                            }
                            // the game's own state: timer-1 tick, MEDIA_SYNC's 32 disc-request
                            // slots, the disc thread's flags / bytes left
                            const uint8_t *ram = rt->memory().getRDRAM();
                            auto word = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + a, 4); return v; };
                            std::cerr << "[threads]   tick=" << std::dec << word(0x29a538) << " cdflags=0x" << std::hex << word(0x26ff90)
                                      << " cdleft=0x" << word(0x26ffb4) << " slots:";
                            for (uint32_t i = 0; i < 32; ++i)
                            {
                                if (word(0x1e25f00 + i * 4) != 0u) std::cerr << " " << std::dec << i << "=0x" << std::hex << word(0x1e25f00 + i * 4);
                            }
                            std::cerr << std::endl;
                            // speed: vblanks / frames the game counted (its VSync callback 0x1A1680) and how
                            // much the emulated IOP ran, each as a total and per second over the last 5 s
                            static uint32_t lastVblanks = 0, lastFrames = 0;
                            static uint64_t lastIop = 0;
                            const uint32_t vblanks = word(0x258148), frames = word(0x258134);
                            const uint64_t iop = rt->iopDebugSnapshot().emulatorInstructions;
                            std::cerr << "[threads]   vblanks=" << std::dec << vblanks << " (" << (vblanks - lastVblanks) / 5 << "/s)"
                                      << " frames=" << frames << " (" << (frames - lastFrames) / 5 << "/s)"
                                      << " iop-instr=" << iop << " (" << (iop - lastIop) / 5 << "/s)" << std::endl;
                            // a rough call stack of the main thread: words on its stack that point just after a JAL in
                            // the game's code (return addresses), innermost first
                            for (const EeThreadSnapshot &t : snap.threads)
                            {
                                if (t.id != 1 || t.sp == 0u)
                                    continue;
                                std::cerr << "[threads]   main stack sp=0x" << std::hex << t.sp << ":";
                                int found = 0;
                                for (uint32_t a = t.sp & 0x1FFFFFFCu; a < (t.sp & 0x1FFFFFFCu) + 0x2000u && found < 16; a += 4u)
                                {
                                    if (a + 4u > 0x2000000u)
                                        break;
                                    const uint32_t v = word(a);
                                    if (v < 0x100008u || v >= 0x29A000u || (v & 3u) != 0u)
                                        continue;
                                    const uint32_t call = word(v - 8u); // the JAL before the delay slot
                                    if ((call >> 26) != 3u)
                                        continue;
                                    std::cerr << " 0x" << v;
                                    ++found;
                                }
                                std::cerr << std::dec << std::endl;
                            }
                            // VENDETTA_GSDUMP=<folder>: the two 640-wide 32-bit display buffers the game draws into
                            // (BP 0x0 / 0x12C0, BW 10) as PPM images, straight from GS memory
                            if (const char *gsDir = std::getenv("VENDETTA_GSDUMP"))
                            {
                                uint8_t *vram = rt->memory().getGSVRAM();
                                // movie buffers 640 × 448 (BW 10) + the menus' field buffers 512 × 224 (BW 8, FBP 0 / 0x40)
                                struct Buf { uint32_t bp, bw, w, h; };
                                for (const Buf b : {Buf{0x0u, 10u, 640u, 448u}, Buf{0x12C0u, 10u, 640u, 448u},
                                                    Buf{0x0u, 8u, 512u, 224u}, Buf{0x800u, 8u, 512u, 224u}})
                                {
                                    char name[64];
                                    std::snprintf(name, sizeof(name), "/gs-%03ds-bp%04x-w%u.ppm", n * 5, b.bp, b.w);
                                    if (std::FILE *f = std::fopen((std::string(gsDir) + name).c_str(), "wb"))
                                    {
                                        std::fprintf(f, "P6\n%u %u\n255\n", b.w, b.h);
                                        for (uint32_t y = 0; y < b.h; ++y)
                                            for (uint32_t x = 0; x < b.w; ++x)
                                            {
                                                const uint32_t px = GSMem::ReadCT32(vram, b.bp, b.bw, x, y);
                                                const uint8_t rgb[3] = {static_cast<uint8_t>(px), static_cast<uint8_t>(px >> 8),
                                                                        static_cast<uint8_t>(px >> 16)};
                                                std::fwrite(rgb, 1, 3, f);
                                            }
                                        std::fclose(f);
                                    }
                                }
                            }
                            // ... and the menu font's one-glyph cache (PSMT4 at TBP 0x3FBD, its CLUT CT32 8×2 at
                            // 0x3FBF) as ASCII art + the 16 CLUT colours
                            if (std::getenv("VENDETTA_GSDUMP"))
                            {
                                uint8_t *vram = rt->memory().getGSVRAM();
                                std::cerr << "[gsdump] glyph @0x3fbd (4-bit index, hex):" << std::endl;
                                for (uint32_t y = 0; y < 24u; ++y)
                                {
                                    std::string row;
                                    for (uint32_t x = 0; x < 24u; ++x)
                                        row += "0123456789abcdef"[GSMem::ReadP4(vram, 0x3FBDu, 1u, x, y) & 15u];
                                    std::cerr << "[gsdump]   " << row << std::endl;
                                }
                                std::cerr << "[gsdump] clut @0x3fbf:" << std::hex;
                                for (uint32_t i = 0; i < 16u; ++i)
                                    std::cerr << " " << GSMem::ReadCT32(vram, 0x3FBFu, 1u, i % 8u, i / 8u);
                                std::cerr << std::dec << std::endl;
                            }
                            // the projection (the camera): FOV degrees 0x258130 → 0x1B6D08 builds 0x258040, whose
                            // [0][0] = cot(fov/2) × 0.75 and [1][1] = cot(fov/2) (computed with VU0 macro code)
                            {
                                float fov = 0.0f, m00 = 0.0f, m11 = 0.0f, cosHalf = 0.0f, sinHalf = 0.0f;
                                std::memcpy(&fov, ram + 0x258130u, 4);
                                std::memcpy(&m00, ram + 0x258040u, 4);
                                std::memcpy(&m11, ram + 0x258054u, 4);
                                std::memcpy(&sinHalf, ram + 0x1E25E80u, 4);
                                std::memcpy(&cosHalf, ram + 0x1E25E84u, 4);
                                const double expected = 1.0 / std::tan(fov * 3.14159265358979 / 360.0);
                                std::cerr << "[threads]   projection fov=" << fov << " m00=" << m00 << " m11=" << m11
                                          << " (expected " << expected * 0.75 << " / " << expected << ") vu0 sin=" << sinHalf
                                          << " cos=" << cosHalf << std::endl;
                            }
                            // the memory-card dialog: its state (0x3029B4 → which message) and selection
                            std::cerr << "[threads]   mc-dialog state=0x" << std::hex << word(0x3029b4)
                                      << " sel=" << std::dec << word(0x3029b8) << std::endl;
                            // timer 1 (the tick): a TARGET near 0xFFFF means its handler ran > 1 period late
                            std::cerr << "[threads]   timer1 count=0x" << std::hex << rt->memory().readIORegister(0x10000800)
                                      << " mode=0x" << rt->memory().readIORegister(0x10000810)
                                      << " target=0x" << rt->memory().readIORegister(0x10000820) << std::dec << std::endl;
                            lastVblanks = vblanks;
                            lastFrames = frames;
                            lastIop = iop;
                            for (const EeSemaphoreSnapshot &s : snap.semaphores)
                            {
                                if (s.waiters != 0u)
                                {
                                    std::cerr << "[threads]   sema id=" << std::dec << s.id << " count=" << s.count
                                              << " max=" << s.maxCount << " waiters=" << s.waiters << std::endl;
                                }
                            }
                        }
                    })
            .detach();
    }

    // VENDETTA_PADLOG=1: the game's own pad table, checked every 50 ms, printed when it changes — pads 0 / 1
    // (port 0 / 1, slot 0): the record's first word, held (+4), pressed (+8), the raw scePadRead bytes (+0xC), the
    // key-repeat word (0x1431210 + 4·pad), the per-port RPC answers of 0x1FC1D8 (gp − 0x1C30 / − 0x1C2C).
    void startPadWatch(PS2Runtime &runtime)
    {
        if (!std::getenv("VENDETTA_PADLOG"))
            return;
        PS2Runtime *rt = &runtime;
        std::thread([rt]()
                    {
                        std::string last;
                        for (;;)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            const uint8_t *ram = rt->memory().getRDRAM();
                            auto word = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + a, 4); return v; };
                            char line[320];
                            std::snprintf(line, sizeof(line),
                                          "pad0 w0=%08x held=%08x pressed=%08x raw=%08x rpt=%08x st=%u | pad1 w0=%08x held=%08x pressed=%08x raw=%08x | rpc %08x %08x",
                                          word(0x1431010u), word(0x1431014u), word(0x1431018u), word(0x143101Cu), word(0x1431210u), word(0x1431044u),
                                          word(0x1431110u), word(0x1431114u), word(0x1431118u), word(0x143111Cu),
                                          word(0x29C570u - 0x1C30u), word(0x29C570u - 0x1C2Cu));
                            if (last != line)
                            {
                                last = line;
                                std::cerr << "[padtable] " << line << std::endl;
                            }
                        }
                    })
            .detach();
    }

    // VENDETTA_HOSTPROF=1: every 5 s, the game's frames/s (its frame counter 0x258134), what the game thread
    // was busy with (sampled 1000x/s; runtime/ps2_host_profile.h) and the work counters; [hostprof-gs] and
    // [hostprof-chain] lines for the GS thread and the chain worker. Phases and counters that are 0 are left out.
    // VENDETTA_HOSTPROF=2 also samples the three threads' host code addresses and lists the busiest functions and
    // source lines per thread (named from the exe's PDB; build-fast.bat links with /DEBUG; the runtime and the compiled
    // VU1 code carry line information).
    void startHostProfile(PS2Runtime &runtime)
    {
        const char *mode = std::getenv("VENDETTA_HOSTPROF");
        if (!mode)
            return;
        const bool samplePcs = mode[0] == '2';
        PS2Runtime *rt = &runtime;
        std::thread([rt, samplePcs]()
                    {
                        using namespace ps2_host_profile;
                        uint32_t counts[Count] = {}, gsCounts[Count] = {}, chainCounts[Count] = {};
                        uint32_t samples = 0, lastFrames = 0;
                        // the longest stretch without a new game frame (a pause on screen), in ms
                        uint32_t seenFrames = 0, msSinceFrame = 0, longestGap = 0;
                        // the most game frames / vblanks in any 250 ms (15 at a steady 60): more = running fast-forward
                        constexpr uint32_t kWindow = 250;
                        uint32_t frameRing[kWindow] = {}, vblankRing[kWindow] = {};
                        uint32_t fastestFrames = 0, fastestVblanks = 0;
                        // the stall report: no vblank for 40 ms = the runtime behind real time (a hitch on screen)
                        constexpr size_t kMaxStacks = 64, kStackDepth = 24;
                        uint64_t stacks[kMaxStacks][kStackDepth];
                        size_t stackLens[kMaxStacks] = {};
                        size_t stackSamples = 0;
                        uint32_t stallPhases[TrackCount][Count] = {};
                        uint64_t stallTick = 0;
                        uint32_t stallMs = 0;
                        auto next = std::chrono::steady_clock::now();
                        auto report = next + std::chrono::seconds(5);
                        auto share = [&samples](uint32_t count) { return std::to_string(samples ? (count * 100u + samples / 2u) / samples : 0u) + "%"; };
                        for (;;)
                        {
                            next += std::chrono::milliseconds(1);
                            std::this_thread::sleep_until(next);
                            const uint8_t phase = g_phases[GameTrack].load(std::memory_order_relaxed);
                            if (phase < Count)
                                ++counts[phase];
                            const uint8_t gsPhase = g_phases[GsTrack].load(std::memory_order_relaxed);
                            if (gsPhase < Count)
                                ++gsCounts[gsPhase];
                            const uint8_t chainPhase = g_phases[ChainTrack].load(std::memory_order_relaxed);
                            if (chainPhase < Count)
                                ++chainCounts[chainPhase];
                            ++samples;
                            uint32_t nowFrames;
                            std::memcpy(&nowFrames, rt->memory().getRDRAM() + 0x258134u, 4);
                            if (nowFrames != seenFrames)
                            {
                                seenFrames = nowFrames;
                                longestGap = std::max(longestGap, msSinceFrame);
                                msSinceFrame = 0;
                            }
                            else
                                ++msSinceFrame;
                            {
                                const uint32_t nowVblanks = static_cast<uint32_t>(rt->memory().gs().vsyncTick.load(std::memory_order_acquire));
                                const uint32_t slot = samples % kWindow;
                                if (samples > kWindow)
                                {
                                    fastestFrames = std::max(fastestFrames, nowFrames - frameRing[slot]);
                                    fastestVblanks = std::max(fastestVblanks, nowVblanks - vblankRing[slot]);
                                }
                                frameRing[slot] = nowFrames;
                                vblankRing[slot] = nowVblanks;
                            }
                            {
                                // while no vblank has come for 40 ms, count what the three threads do and take a rough
                                // call stack of the game thread every 5 ms; when the vblanks resume after 60 ms or more,
                                // print it: "[stall] N ms without a vblank" (the runtime fell behind real time)
                                const uint64_t vblankNow = rt->memory().gs().vsyncTick.load(std::memory_order_acquire);
                                if (vblankNow != stallTick)
                                {
                                    if (stallMs >= 60u)
                                    {
                                        std::string text = "[stall] " + std::to_string(stallMs) + " ms without a vblank (after tick " +
                                                           std::to_string(stallTick) + ")";
                                        static const char *const trackNames[TrackCount] = {"game thread", "GS thread", "chain worker"};
                                        for (uint32_t t = 0; t < TrackCount; ++t)
                                        {
                                            uint32_t total = 0;
                                            for (uint8_t i = 0; i < Count; ++i)
                                                total += stallPhases[t][i];
                                            text += std::string(t == 0 ? ": " : "; ") + trackNames[t];
                                            for (uint8_t i = 0; i < Count; ++i)
                                                if (stallPhases[t][i] != 0u)
                                                    text += std::string(" ") + name(i) + "=" + std::to_string((stallPhases[t][i] * 100u + total / 2u) / total) + "%";
                                        }
                                        std::map<std::string, uint32_t> seen;
                                        for (size_t i = 0; i < stackSamples; ++i)
                                            ++seen[describeStack(stacks[i], stackLens[i])];
                                        std::vector<std::pair<uint32_t, std::string>> ranked;
                                        for (auto &[stack, count] : seen)
                                            ranked.emplace_back(count, stack);
                                        std::sort(ranked.rbegin(), ranked.rend());
                                        for (size_t i = 0; i < ranked.size() && i < 3u; ++i)
                                            text += "\n[stall]   " + std::to_string(ranked[i].first) + "/" + std::to_string(stackSamples) +
                                                    " game thread: " + ranked[i].second;
                                        std::fprintf(stderr, "%s\n", text.c_str());
                                    }
                                    stallTick = vblankNow;
                                    stallMs = 0;
                                    stackSamples = 0;
                                    std::memset(stallPhases, 0, sizeof(stallPhases));
                                }
                                else if (++stallMs >= 40u)
                                {
                                    if (phase < Count)
                                        ++stallPhases[GameTrack][phase];
                                    if (gsPhase < Count)
                                        ++stallPhases[GsTrack][gsPhase];
                                    if (chainPhase < Count)
                                        ++stallPhases[ChainTrack][chainPhase];
                                    if (stallMs % 5u == 0u && stackSamples < kMaxStacks)
                                    {
                                        stackLens[stackSamples] = stackScan(GameTrack, stacks[stackSamples], kStackDepth);
                                        ++stackSamples;
                                    }
                                }
                            }
                            if (samplePcs)
                            {
                                // one thread per tick (a sample stops that thread for a moment): ~333 a second each
                                const Track track = static_cast<Track>(samples % TrackCount);
                                samplePc(track, track == GameTrack ? phase : track == GsTrack ? gsPhase : chainPhase);
                            }
                            if (std::chrono::steady_clock::now() < report)
                                continue;
                            report += std::chrono::seconds(5);
                            uint32_t frames;
                            std::memcpy(&frames, rt->memory().getRDRAM() + 0x258134u, 4);
                            std::string line = "[hostprof] frames/s=" + std::to_string((frames - lastFrames) / 5u) +
                                               " longest-frame-gap=" + std::to_string(std::max(longestGap, msSinceFrame)) + "ms" +
                                               " most-in-250ms=" + std::to_string(fastestFrames) + "/" + std::to_string(fastestVblanks) + "vbl";
                            longestGap = 0;
                            fastestFrames = fastestVblanks = 0;
                            for (uint8_t i = 0; i < Count; ++i)
                                if (counts[i] != 0u)
                                    line += std::string(" ") + name(i) + "=" + share(counts[i]);
                            for (uint8_t i = 0; i < CounterCount; ++i)
                            {
                                const uint64_t value = g_counters[i].exchange(0u, std::memory_order_relaxed);
                                if (value != 0u)
                                    line += std::string(" ") + counterName(i) + "/s=" + std::to_string((value + 4u) / 5u);
                            }
                            std::cerr << line << std::endl;
                            // the GS thread and the chain worker (VENDETTA_CHAIN_ASYNC=1), when they ran
                            for (const auto &[label, track] : {std::pair<const char *, const uint32_t *>{"[hostprof-gs]", gsCounts},
                                                               std::pair<const char *, const uint32_t *>{"[hostprof-chain]", chainCounts}})
                            {
                                if (track[Idle] == samples)
                                    continue;
                                std::string trackLine = std::string(label) + " busy=" + share(samples - track[Idle]);
                                for (uint8_t i = 0; i < Count; ++i)
                                    if (track[i] != 0u)
                                        trackLine += std::string(" ") + name(i) + "=" + share(track[i]);
                                std::cerr << trackLine << std::endl;
                            }
                            if (samplePcs)
                                std::cerr << pcReport(30u, 12u) << std::flush;
                            lastFrames = frames;
                            samples = 0;
                            std::fill(std::begin(counts), std::end(counts), 0u);
                            std::fill(std::begin(gsCounts), std::end(gsCounts), 0u);
                            std::fill(std::begin(chainCounts), std::end(chainCounts), 0u);
                        }
                    })
            .detach();
    }

    // Always on: if the game's frame counter (0x258134) stops for 3 s after it has been counting, print once
    // what every guest thread and semaphore is doing (a freeze report in the log, for runs where nothing else logs).
    void startFreezeWatch(PS2Runtime &runtime)
    {
        PS2Runtime *rt = &runtime;
        std::thread([rt]()
                    {
                        uint32_t lastFrames = 0, stalledSeconds = 0;
                        bool counting = false, reported = false;
                        for (;;)
                        {
                            // one second: how much of it the game thread spent waiting (a frozen game waits ~all the time;
                            // a movie keeps it busy although the frame counter stands still)
                            uint32_t waitingSamples = 0;
                            for (int sample = 0; sample < 100; ++sample)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                if (ps2_host_profile::g_phase.load(std::memory_order_relaxed) == ps2_host_profile::Wait)
                                    ++waitingSamples;
                            }
                            const uint8_t *ram = rt->memory().getRDRAM();
                            auto word = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + a, 4); return v; };
                            const uint32_t frames = word(0x258134u);
                            if (frames != lastFrames)
                            {
                                counting = counting || (frames - lastFrames) > 5u;
                                lastFrames = frames;
                                stalledSeconds = 0;
                                reported = false;
                                continue;
                            }
                            if (!counting || reported || ++stalledSeconds < 3u)
                                continue;
                            const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                            if (waitingSamples < 90u) // busy (a movie plays without counting frames): not frozen
                            {
                                stalledSeconds = 0;
                                continue;
                            }
                            reported = true;
                            std::cerr << "[freeze] the game's frame counter stopped at " << frames << "; running thread "
                                      << snap.runningThreadId << std::endl;
                            for (const EeThreadSnapshot &t : snap.threads)
                                std::cerr << "[freeze]   thread " << t.id << " entry=0x" << std::hex << t.entry << " pc=0x" << t.pc
                                          << " ra=0x" << t.ra << " sp=0x" << t.sp << " stack=0x" << t.stack << "+0x" << t.stackSize
                                          << std::dec << " prio=" << t.currentPriority << " status=" << static_cast<int>(t.status)
                                          << " wait=" << static_cast<int>(t.waitReason) << "/" << t.waitId
                                          << " depth=" << t.invocationDepth << std::endl;
                            for (const EeSemaphoreSnapshot &sema : snap.semaphores)
                                std::cerr << "[freeze]   sema " << sema.id << " count=" << sema.count << "/" << sema.maxCount
                                          << " waiters=" << sema.waiters << std::endl;
                            for (const EeThreadSnapshot &t : snap.threads)
                            {
                                if (t.sp == 0u || (t.sp & 0x1FFFFFFFu) + 0x800u > 0x2000000u)
                                    continue;
                                std::cerr << "[freeze]   thread " << t.id << " stack (return addresses):" << std::hex;
                                int found = 0;
                                for (uint32_t a = t.sp & 0x1FFFFFFCu; a < (t.sp & 0x1FFFFFFCu) + 0x800u && found < 12; a += 4u)
                                {
                                    const uint32_t v = word(a);
                                    if (v >= 0x100008u && v < 0x29A000u && (v & 3u) == 0u && (word(v - 8u) >> 26) == 3u)
                                    {
                                        std::cerr << " 0x" << v;
                                        ++found;
                                    }
                                }
                                std::cerr << std::dec << std::endl;
                            }
                        }
                    })
            .detach();
    }

    void applyVendetta(PS2Runtime &runtime)
    {
        // The chain worker (VENDETTA_CHAIN_ASYNC=1): the VSync handler 0x1A1680 reads CSR at 0x1A16C0 (ld s0, 0(v0) of
        // 0x12001000) only to pass it to 0x1A1648, which uses (s0 >> 13) & 1 = FIELD (written by the EE scheduler
        // alone); SIGNAL / FINISH (bits 0-1, from the GS front end on the worker) only reach a dead stack slot. So that
        // read needs no fence (VENDETTA_CHAIN_VERIFY=1 checks it, VENDETTA_CHAIN_CSR_EXEMPT=0 turns it off).
        runtime.chain().allowUnfencedCsrRead(0x1A16C0u);
        startThreadDump(runtime);
        startPadWatch(runtime);
        startHostProfile(runtime);
        startFreezeWatch(runtime);
        installVendettaAnimations(runtime);
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        const std::filesystem::path image = paths.elfDirectory.parent_path() / "disc" / "SLUS-20639.iso";
        if (!std::filesystem::exists(image))
        {
            std::cerr << "[vendetta] disc image not found: " << image.string() << std::endl;
            return;
        }
        paths.cdImage = image;
        PS2Runtime::setIoPaths(paths);
        std::cout << "[vendetta] disc image: " << image.string() << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Def Jam Vendetta (USA)", "SLUS_206.39", 0x100008u, 0u, applyVendetta)

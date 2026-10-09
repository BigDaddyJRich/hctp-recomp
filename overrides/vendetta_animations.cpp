// Def Jam Vendetta (USA, SLUS-20639) - body animations from a mods\ folder.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game keeps every body animation ("clip") in two banks loaded whole at boot: bank 0 (0x1431580, 821 clips, ids
// 10000 + i) and bank 1 (0x1625580, 2,003 clips, ids i). Each bank is "PAK ", a size, a count, then an index of signed
// 32-bit offsets: clip i is at bank + 0xC + 4i + offset[i]. The code that plays clips (FUN_001b33b8, FUN_001b2620,
// FUN_001b2848, FUN_001b3460) builds the bank address from a literal and reads the index without checking the count,
// so a clip can be replaced by pointing its index entry at new bytes anywhere in memory.
//
// Once both banks are in memory - the boot routine starts their disc reads (FUN_001a0780) and lets them finish in the
// background, so bank 1 is still empty when the boot moves on - the first time a fighter starts a clip
// (FUN_00132158, hooked) each mod's "replace" / "animations" entries (mod.json, see vendetta_mods.h) replace those
// game clips (by the names in the tables at 0x25A790 / 0x259AB8): the new bytes go into the free space after the
// clip's bank and its index entry is repointed. A replacement changes the clip everywhere the game uses it; mods apply
// in load order, so a later mod's replacement of the same clip wins.
//
// VENDETTA_MODS=0        the plain game (mods\ ignored)
// VENDETTA_ANIMLOG=1     each clip a fighter starts (FUN_00132158: fighter in a0, clip id in a1), with its name
// VENDETTA_ANIMLIST=1    every clip at boot: id, bank, name, size, where it comes from
// VENDETTA_ANIMDUMP=a,b  those clips' bytes to logs\anim\<name>.bin (to make test mods)
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"
#include "vendetta_mods.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace vendetta_animations
{
    constexpr uint32_t kBank0 = 0x1431580u;    // PAK 2: 821 clips, ids 10000 + i
    constexpr uint32_t kBank1 = 0x1625580u;    // PAK 3: 2,003 clips, ids i
    constexpr uint32_t kBank0Limit = kBank1;   // the free space after bank 0 ends where bank 1 starts
    constexpr uint32_t kBank1Limit = 0x1DDC580u; // ... after bank 1 ends where PAK 1 starts
    constexpr uint32_t kNames0 = 0x259AB8u;    // bank 0's clip names
    constexpr uint32_t kNames1 = 0x25A790u;    // bank 1's clip names
    constexpr uint32_t kWaza = 0x2331C8u;      // the parsed move data (+0x08 = 999 move records)
    constexpr uint32_t kReadSlots = 0x1E25F00u; // the game's 32 disc reads in flight (0 = free)
    constexpr uint32_t kPlayClip = 0x132158u;
    constexpr uint32_t kExpectedCount0 = 821u;
    constexpr uint32_t kExpectedCount1 = 2003u;

    struct Clip
    {
        int bank = 0;
        uint32_t index = 0;
        uint32_t address = 0;
        uint32_t size = 0; // to the next clip (or the bank's end)
        std::string name;
        std::string from = "game";
    };

    uint8_t *g_rdram = nullptr;
    std::vector<Clip> g_clips[2];
    PS2Runtime::RecompiledFunction g_originalPlayClip = nullptr;

    uint32_t read32(uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 4);
        return value;
    }
    uint16_t read16(uint32_t address)
    {
        uint16_t value = 0;
        std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 2);
        return value;
    }
    void write32(uint32_t address, uint32_t value) { std::memcpy(g_rdram + (address & PS2_RAM_MASK), &value, 4); }

    std::string upper(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return text;
    }

    bool envOn(const char *name)
    {
        const char *value = std::getenv(name);
        return value && *value && *value != '0';
    }

    // A guest C string (a clip name), or "" when the pointer doesn't lead to one.
    std::string guestString(uint32_t address)
    {
        if (address < 0x100000u || address >= 0x2000000u)
            return {};
        std::string text;
        for (uint32_t i = 0; i < 32u; ++i)
        {
            const char c = static_cast<char>(g_rdram[(address + i) & PS2_RAM_MASK]);
            if (c == 0)
                return text;
            if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) >= 0x7Fu)
                return {};
            text.push_back(c);
        }
        return {};
    }

    uint32_t bankBase(int bank) { return bank == 0 ? kBank0 : kBank1; }
    uint32_t clipAddress(int bank, uint32_t index)
    {
        const uint32_t entry = bankBase(bank) + 0xCu + 4u * index;
        return entry + read32(entry); // a signed offset: unsigned wrap-around does the subtraction
    }

    // Reads both banks' index and names (the game's own state; called after the boot loads).
    void readClips()
    {
        for (int bank = 0; bank < 2; ++bank)
        {
            const uint32_t base = bankBase(bank);
            const uint32_t count = read32(base + 8u);
            const uint32_t end = base + read32(base + 4u);
            std::vector<Clip> &clips = g_clips[bank];
            clips.assign(count, Clip{});
            for (uint32_t i = 0; i < count; ++i)
            {
                Clip &clip = clips[i];
                clip.bank = bank;
                clip.index = i;
                clip.address = clipAddress(bank, i);
                clip.name = guestString(read32((bank == 0 ? kNames0 : kNames1) + 4u * i));
            }
            // sizes: the distance to the next clip in memory (the last one to the bank's end)
            std::vector<uint32_t> starts;
            for (const Clip &clip : clips)
                starts.push_back(clip.address);
            std::sort(starts.begin(), starts.end());
            for (Clip &clip : clips)
            {
                const auto next = std::upper_bound(starts.begin(), starts.end(), clip.address);
                clip.size = (next != starts.end() ? *next : end) - clip.address;
            }
        }
    }

    // The banks and the move data look like the original disc's (mods are for the original disc only).
    bool looksOriginal(std::string &why)
    {
        if (std::memcmp(g_rdram + kBank0, "PAK ", 4) != 0 || std::memcmp(g_rdram + kBank1, "PAK ", 4) != 0)
            why = "an animation bank is missing";
        else if (read32(kBank0 + 8u) != kExpectedCount0 || read32(kBank1 + 8u) != kExpectedCount1)
            why = "the animation banks hold " + std::to_string(read32(kBank0 + 8u)) + " / " +
                  std::to_string(read32(kBank1 + 8u)) + " clips, not 821 / 2003";
        else if (read32(kWaza + 8u) != 999u)
            why = "the move data holds " + std::to_string(read32(kWaza + 8u)) + " moves, not 999";
        else if (guestString(read32(kNames1)).empty() || guestString(read32(kNames0)).empty())
            why = "the clip name tables are not where the original game keeps them";
        else
            return true;
        return false;
    }

    std::filesystem::path projectRoot() { return PS2Runtime::getIoPaths().elfDirectory.parent_path(); }

    bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    // The checks a clip file must pass (the full layout needs the BODY skeleton: that stays the converter's job).
    bool clipLooksValid(const std::vector<uint8_t> &bytes, std::string &why)
    {
        if (bytes.size() < 3u)
        {
            why = "shorter than a clip header";
            return false;
        }
        const uint32_t lastFrame = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u);
        const uint32_t poseWords = bytes[2];
        if (lastFrame >= 0x7FFu)
        {
            why = "its last frame (" + std::to_string(lastFrame) + ") is past what the game's timing fields hold (2046)";
            return false;
        }
        if (3u + poseWords * 6u > bytes.size())
        {
            why = "its pose words run past the end of the file";
            return false;
        }
        return true;
    }

    // Replaces clips from the mods\ folder: the bytes go into the free space after the clip's bank (placed 8 bytes past
    // a 16-byte boundary, like the game's own clips), and the index entry points at them.
    void applyMods()
    {
        uint32_t freeStart[2];
        for (int bank = 0; bank < 2; ++bank)
        {
            const uint32_t base = bankBase(bank);
            freeStart[bank] = base + read32(base + 4u);
        }
        const uint32_t freeEnd[2] = {kBank0Limit, kBank1Limit};

        std::unordered_map<std::string, std::vector<Clip *>> byName;
        for (std::vector<Clip> &clips : g_clips)
            for (Clip &clip : clips)
                if (!clip.name.empty())
                    byName[upper(clip.name)].push_back(&clip);

        uint32_t replaced = 0, skipped = 0;
        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            if (const vendetta_mods::Json *added = mod.manifest.find("new"))
                if (added->find("animations"))
                    std::cerr << "[mods] " << mod.name << ": new animations come in a later step - not loaded yet" << std::endl;
            const vendetta_mods::Json *replace = mod.manifest.find("replace");
            const vendetta_mods::Json *animations = replace ? replace->find("animations") : nullptr;
            if (!animations)
                continue;
            if (!animations->isObject())
            {
                std::cerr << "[mods] " << mod.name << ": \"replace\": \"animations\" should be { \"<game clip>\": \"<file>\" }"
                          << std::endl;
                continue;
            }
            for (const auto &[gameName, fileValue] : animations->members)
            {
                const auto found = byName.find(upper(gameName));
                if (found == byName.end())
                {
                    std::cerr << "[mods] " << mod.name << ": replace: the game has no animation called \"" << gameName
                              << "\" - skipped" << std::endl;
                    ++skipped;
                    continue;
                }
                if (!fileValue.isString())
                {
                    std::cerr << "[mods] " << mod.name << ": replace " << gameName << ": the file should be a name in quotes"
                              << std::endl;
                    ++skipped;
                    continue;
                }
                const std::filesystem::path file = vendetta_mods::modFile(mod, fileValue.text, "animation");
                std::vector<uint8_t> bytes;
                std::string why;
                if (file.empty())
                {
                    ++skipped;
                    continue;
                }
                if (!readFile(file, bytes) || !clipLooksValid(bytes, why))
                {
                    std::cerr << "[mods] " << mod.name << ": " << fileValue.text << " skipped: "
                              << (why.empty() ? "can't be read" : why) << std::endl;
                    ++skipped;
                    continue;
                }
                const std::string fileName = file.filename().string();
                const std::string &modName = mod.name;
                for (Clip *clip : found->second)
                {
                    const int bank = clip->bank;
                    const uint32_t start = ((freeStart[bank] + 7u) & ~15u) + 8u;
                    const uint32_t end = start + static_cast<uint32_t>(bytes.size());
                    if (end > freeEnd[bank])
                    {
                        std::cerr << "[mods] " << modName << ": " << fileName << " skipped: no room left after bank " << bank
                                  << " (" << (freeEnd[bank] > freeStart[bank] ? freeEnd[bank] - freeStart[bank] : 0u)
                                  << " bytes free; more memory comes in a later step)" << std::endl;
                        ++skipped;
                        continue;
                    }
                    std::memcpy(g_rdram + start, bytes.data(), bytes.size());
                    const uint32_t entry = bankBase(bank) + 0xCu + 4u * clip->index;
                    const uint32_t oldLastFrame = read16(clip->address);
                    write32(entry, start - entry);
                    freeStart[bank] = end;
                    std::cerr << "[mods] " << modName << ": replaced " << clip->name << " (bank " << bank << " clip "
                              << clip->index << ", id " << (bank == 0 ? 10000u + clip->index : clip->index) << ", "
                              << oldLastFrame + 1u << " frames) with " << fileName << " ("
                              << (static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u)) + 1u
                              << " frames, " << bytes.size() << " bytes)" << std::endl;
                    clip->address = start;
                    clip->size = static_cast<uint32_t>(bytes.size());
                    clip->from = modName;
                    ++replaced;
                }
            }
        }
        std::cerr << "[mods] animations: " << replaced << " replaced" << (skipped ? ", " + std::to_string(skipped) + " skipped" : "")
                  << std::endl;
    }

    void dumpClips()
    {
        const char *list = std::getenv("VENDETTA_ANIMDUMP");
        if (!list || !*list)
            return;
        const std::filesystem::path folder = projectRoot() / "logs" / "anim";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::string names = list;
        size_t start = 0;
        while (start <= names.size())
        {
            const size_t comma = names.find(',', start);
            const std::string want = upper(names.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            start = comma == std::string::npos ? names.size() + 1 : comma + 1;
            bool found = false;
            for (const std::vector<Clip> &clips : g_clips)
                for (const Clip &clip : clips)
                    if (!found && upper(clip.name) == want)
                    {
                        found = true;
                        std::ofstream out(folder / (clip.name + ".bin"), std::ios::binary);
                        out.write(reinterpret_cast<const char *>(g_rdram + clip.address), clip.size);
                        std::cerr << "[anim] dumped " << clip.name << " (" << clip.size << " bytes) to logs\\anim\\" << std::endl;
                    }
            if (!found && !want.empty())
                std::cerr << "[anim] VENDETTA_ANIMDUMP: no clip named " << want << std::endl;
        }
    }

    void listClips()
    {
        for (const std::vector<Clip> &clips : g_clips)
            for (const Clip &clip : clips)
                std::cerr << "[anim] id " << (clip.bank == 0 ? 10000u + clip.index : clip.index) << " bank " << clip.bank
                          << " " << (clip.name.empty() ? "?" : clip.name) << " " << clip.size << " bytes, "
                          << read16(clip.address) + 1u << " frames, from " << clip.from << std::endl;
    }

    // Both banks loaded and no disc read of the game's in flight (so neither bank is half-read).
    bool banksReady()
    {
        if (std::memcmp(g_rdram + kBank0, "PAK ", 4) != 0 || std::memcmp(g_rdram + kBank1, "PAK ", 4) != 0)
            return false;
        for (uint32_t slot = 0; slot < 32u; ++slot)
            if (read32(kReadSlots + 4u * slot) != 0u)
                return false;
        return true;
    }

    // Once, when the banks are ready: check the data is the original disc's, then apply the mods.
    void prepare(uint8_t *rdram)
    {
        static bool done = false;
        if (done)
            return;
        g_rdram = rdram;
        if (!banksReady())
            return;
        done = true;
        std::string why;
        if (!looksOriginal(why))
        {
            std::cerr << "[mods] the game data doesn't look like the original disc's (" << why
                      << "); mods are off: they need the original Def Jam Vendetta (USA) disc" << std::endl;
            return;
        }
        readClips();
        dumpClips(); // the game's own bytes, before any mod
        applyMods();
        if (envOn("VENDETTA_ANIMLIST"))
            listClips();
    }

    std::string clipName(uint32_t id)
    {
        const int bank = id >= 10000u ? 0 : 1;
        const uint32_t index = bank == 0 ? id - 10000u : id;
        if (index < g_clips[bank].size())
            return g_clips[bank][index].name;
        return "?";
    }

    // A fighter starting a clip (FUN_00132158; it skips the work when the clip is already playing): the mods go in
    // before the first clip that plays with both banks loaded. VENDETTA_ANIMLOG: each new clip, with its name.
    void playClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        prepare(rdram);
        static const bool log = envOn("VENDETTA_ANIMLOG");
        static const auto start = std::chrono::steady_clock::now();
        const uint32_t fighter = getRegU32(ctx, 4);
        const uint32_t id = getRegU32(ctx, 5);
        if (log && g_rdram && read32(fighter + 0xFCu) != id)
        {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cerr << "[anim] " << std::fixed;
            std::cerr.precision(2);
            std::cerr << seconds << "s fighter 0x" << std::hex << fighter << std::dec << " clip " << id << " "
                      << clipName(id) << " (ra 0x" << std::hex << getRegU32(ctx, 31) << std::dec << ")" << std::endl;
            std::cerr.unsetf(std::ios::fixed);
        }
        g_originalPlayClip(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        g_originalPlayClip = runtime.lookupFunction(kPlayClip);
        if (g_originalPlayClip)
            runtime.replaceFunction(kPlayClip, playClip);
    }
}

void installVendettaAnimations(PS2Runtime &runtime) { vendetta_animations::install(runtime); }

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
// (FUN_00132158, hooked) the mods go in (mod.json, see vendetta_mods.h), their bytes in the store above the game's
// 32 MB (vendetta_store.h):
//  - "new": "animations": clips the game doesn't have, named by their file, become bank 1 clips 2003, 2004, ... The
//    index can't move (the code builds its address from a literal), so it grows in place over the first clips after
//    it: those are copied to the store and repointed first. Each clip also has a 38-byte timing record (FZUI) in the
//    array at *(0x2331C8 + 0x24), stride *(0x2331C8 + 0x1C), read when a clip starts (FUN_00132158 / FUN_00135060 /
//    FUN_00125d58): the array is copied to the store with room for the new clips and the pointer moved. A new clip's
//    record starts empty (a move that uses it sets it). The clip count at bank + 8 is updated (no game code reads it).
//    Ids stay under 4,096: a move record holds its clip in 12 bits (FUN_0011c608).
//  - "replace": "animations": game clips (by the names in the tables at 0x25A790 / 0x259AB8) get new bytes; the index
//    entry points at them. A replacement changes the clip everywhere the game uses it; mods apply in load order, so a
//    later mod's replacement of the same clip wins.
//  - "wrestlers": a game wrestler's moveset slots ({ "84": "Jericho Entrance" }): a new animation in a slot gets a
//    move of its own (a copy of the slot's move with the clip changed); the move records are copied to the store with
//    room for them. FUN_00102938 (play a moveset slot) reads the slot before it starts the clip, so it is hooked too.
//
// VENDETTA_MODS=0        the plain game (mods\ ignored)
// VENDETTA_ANIMLOG=1     each clip a fighter starts (FUN_00132158: fighter in a0, clip id in a1), with its name
// VENDETTA_ANIMLIST=1    every clip at boot: id, bank, name, size, where it comes from
// VENDETTA_ANIMDUMP=a,b  those clips' bytes to logs\anim\<name>.bin (to make test mods)
// VENDETTA_ANIM_POISON=1 fills what the mods made unused (moved / replaced clips' old bytes, the old timing array) with
//                        0xFF: if anything in the game still read them, it would show (a test switch)
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"
#include "vendetta_mods.h"
#include "vendetta_store.h"

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
    constexpr uint32_t kNames0 = 0x259AB8u;    // bank 0's clip names
    constexpr uint32_t kNames1 = 0x25A790u;    // bank 1's clip names
    constexpr uint32_t kWaza = 0x2331C8u;      // the parsed move data (+0x08 = 999 move records)
    constexpr uint32_t kFzuiStride = 0x1Cu;    // ... + 0x1C: bytes per clip timing record (38)
    constexpr uint32_t kFzuiBank1 = 0x24u;     // ... + 0x24: bank 1's timing records (one per clip id)
    constexpr uint32_t kFzuiSize = 38u;
    constexpr uint32_t kMaxClipId = 4095u;     // a move record's clip field is 12 bits (FUN_0011c608)
    constexpr uint32_t kRecords = 0x14u;       // ... + 0x14: the move records (stride + 0x04, count + 0x08)
    constexpr uint32_t kMovesets = 0x7Cu;      // ... + 0x7C + 16 x attire: that attire's moveset (128 u16 slots)
    constexpr uint32_t kCharacters = 0x27E100u; // the character table: one 0x20 row per wrestler attire (+0 / +4 names)
    constexpr uint32_t kAttires = 59u;         // rows with a moveset (59-62 are the empty "Character A-D")
    constexpr uint32_t kSlots = 128u;
    constexpr uint32_t kPlaySlot = 0x102938u;  // play a moveset slot (fighter, wrestler, slot) - reads the slot first
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
    PS2Runtime::RecompiledFunction g_originalPlaySlot = nullptr;

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
        else if (read32(kWaza + kFzuiStride) != kFzuiSize || read32(kWaza + kFzuiBank1) - 0x100000u >= 0x1F00000u)
            why = "the clip timing records aren't laid out as in the original game";
        else if (upper(guestString(read32(kCharacters))) != "BRIGGS" || read32(kWaza + 4u) != 25u)
            why = "the character table or the move records aren't where the original game keeps them";
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

    uint32_t lastFrameOf(const std::vector<uint8_t> &bytes)
    {
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u);
    }

    // A clip file from a mod, read and checked once however many times it's named; its bytes go into the store on
    // first use (placed 8 bytes past a 16-byte boundary, like the game's own clips).
    struct ModClip
    {
        std::string modName;
        std::string fileName;
        std::vector<uint8_t> bytes;
        uint32_t address = 0; // in the store, once placed
    };

    std::map<std::filesystem::path, ModClip> g_modClips;

    ModClip *loadModClip(const vendetta_mods::Mod &mod, const std::string &relative, const std::string &what)
    {
        const std::filesystem::path file = vendetta_mods::modFile(mod, relative, "animation");
        if (file.empty())
            return nullptr;
        const auto known = g_modClips.find(file);
        if (known != g_modClips.end())
            return &known->second;
        ModClip clip;
        clip.modName = mod.name;
        clip.fileName = file.filename().string();
        std::string why;
        if (!readFile(file, clip.bytes) || !clipLooksValid(clip.bytes, why))
        {
            std::cerr << "[mods] " << mod.name << ": " << what << " " << relative << " skipped: "
                      << (why.empty() ? "can't be read" : why) << std::endl;
            return nullptr;
        }
        return &g_modClips.emplace(file, std::move(clip)).first->second;
    }

    bool placeModClip(ModClip &clip)
    {
        if (clip.address == 0u)
        {
            clip.address = vendetta_store::allocate(vendetta_store::Area::Boot, static_cast<uint32_t>(clip.bytes.size()),
                                                    "animations", 16u, 8u);
            if (clip.address != 0u)
                std::memcpy(vendetta_store::host(clip.address), clip.bytes.data(), clip.bytes.size());
        }
        return clip.address != 0u;
    }

    void pointIndexAt(int bank, uint32_t index, uint32_t address)
    {
        const uint32_t entry = bankBase(bank) + 0xCu + 4u * index;
        write32(entry, address - entry); // self-relative; unsigned wrap-around makes it signed
    }

    // Byte ranges no index entry uses any more, for VENDETTA_ANIM_POISON.
    std::vector<std::pair<uint32_t, uint32_t>> g_unused;

    // A new clip from a mod: its name (the file's), the file.
    struct NewClip
    {
        std::string name;
        ModClip *clip = nullptr;
    };

    // The mods' "new" animations, in load order; a name added again by a later mod replaces the earlier one.
    std::vector<NewClip> collectNewClips(const std::unordered_map<std::string, std::vector<Clip *>> &gameNames)
    {
        std::vector<NewClip> added;
        std::unordered_map<std::string, size_t> byName;
        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            const vendetta_mods::Json *section = mod.manifest.find("new");
            const vendetta_mods::Json *list = section ? section->find("animations") : nullptr;
            if (!list)
                continue;
            if (!list->isArray())
            {
                std::cerr << "[mods] " << mod.name << ": \"new\": \"animations\" should be a list of files: [\"animations/<name>.bin\"]"
                          << std::endl;
                continue;
            }
            for (const vendetta_mods::Json &item : list->items)
            {
                if (!item.isString())
                {
                    std::cerr << "[mods] " << mod.name << ": new animations: each file should be a name in quotes" << std::endl;
                    continue;
                }
                ModClip *clip = loadModClip(mod, item.text, "new animation");
                if (!clip)
                    continue;
                const std::string name = std::filesystem::path(item.text).stem().string();
                const std::string key = upper(name);
                if (gameNames.count(key))
                    std::cerr << "[mods] " << mod.name << ": new animation \"" << name
                              << "\" has the name of one of the game's own clips - to change the game's, use \"replace\""
                              << std::endl;
                const auto earlier = byName.find(key);
                if (earlier != byName.end())
                {
                    NewClip &was = added[earlier->second];
                    if (was.clip != clip)
                        std::cerr << "[mods] " << mod.name << ": new animation \"" << name << "\" was also added by "
                                  << was.clip->modName << " - " << mod.name << "'s is used (later in the load order)" << std::endl;
                    was.clip = clip;
                    continue;
                }
                byName.emplace(key, added.size());
                added.push_back({name, clip});
            }
        }
        return added;
    }

    // Bank 1's index grows in place by 4 bytes per new clip: the game clips it would cover move to the store first.
    // Returns false (nothing changed) if they can't.
    bool growBank1Index(uint32_t newCount, uint32_t &moved)
    {
        std::vector<Clip> &clips = g_clips[1];
        const uint32_t indexEnd = kBank1 + 0xCu + 4u * newCount;
        std::vector<std::pair<Clip *, uint32_t>> moves;
        for (Clip &clip : clips)
        {
            if (clip.address >= indexEnd || clip.address < kBank1)
                continue;
            const uint32_t to = vendetta_store::allocate(vendetta_store::Area::Boot, clip.size, "game clips moved for the index",
                                                         16u, 8u);
            if (to == 0u)
                return false;
            moves.push_back({&clip, to});
        }
        for (auto &[clip, to] : moves)
        {
            std::memcpy(vendetta_store::host(to), g_rdram + clip->address, clip->size);
            pointIndexAt(1, clip->index, to);
            if (clip->address + clip->size > indexEnd)
                g_unused.push_back({indexEnd, clip->address + clip->size});
            clip->address = to;
        }
        moved = static_cast<uint32_t>(moves.size());
        return true;
    }

    // Bank 1's timing records (FZUI), copied to the store with room for the new clips (those start empty).
    bool growBank1Timing(uint32_t newCount)
    {
        const uint32_t oldArray = read32(kWaza + kFzuiBank1);
        const uint32_t oldBytes = kExpectedCount1 * kFzuiSize;
        const uint32_t array = vendetta_store::allocate(vendetta_store::Area::Boot, newCount * kFzuiSize,
                                                        "animation timing records", 16u, 0u);
        if (array == 0u)
            return false;
        uint8_t *to = vendetta_store::host(array);
        std::memcpy(to, g_rdram + oldArray, oldBytes);
        std::memset(to + oldBytes, 0, (newCount - kExpectedCount1) * kFzuiSize);
        write32(kWaza + kFzuiBank1, array);
        g_unused.push_back({oldArray, oldArray + oldBytes});
        return true;
    }

    // Applies the mods: new clips first (they move the clips under the grown index), then replacements.
    void applyMods()
    {
        // the game's own clips by name (only those: "replace" is for the game's clips); built again once the new
        // clips are in, since adding them can move the list
        std::unordered_map<std::string, std::vector<Clip *>> byName;
        const auto indexGameNames = [&byName]()
        {
            byName.clear();
            for (int bank = 0; bank < 2; ++bank)
            {
                const uint32_t count = bank == 0 ? kExpectedCount0 : kExpectedCount1;
                for (uint32_t i = 0; i < count && i < g_clips[bank].size(); ++i)
                    if (!g_clips[bank][i].name.empty())
                        byName[upper(g_clips[bank][i].name)].push_back(&g_clips[bank][i]);
            }
        };
        indexGameNames();

        uint32_t replaced = 0, skipped = 0, moved = 0, addedCount = 0;

        std::vector<NewClip> added = collectNewClips(byName);
        const uint32_t room = kMaxClipId + 1u - kExpectedCount1;
        if (added.size() > room)
        {
            std::cerr << "[mods] new animations: " << added.size() << " asked for, " << room
                      << " fit for now (a move holds its clip id in 12 bits, so ids stop at 4095) - the rest skipped"
                      << std::endl;
            skipped += static_cast<uint32_t>(added.size() - room);
            added.resize(room);
        }
        // every new clip's bytes placed first, so a full store leaves the game untouched
        std::vector<NewClip> placed;
        for (NewClip &item : added)
        {
            if (placeModClip(*item.clip))
                placed.push_back(item);
            else
                ++skipped;
        }
        if (!placed.empty())
        {
            const uint32_t newCount = kExpectedCount1 + static_cast<uint32_t>(placed.size());
            if (!growBank1Index(newCount, moved) || !growBank1Timing(newCount))
            {
                std::cerr << "[mods] new animations skipped: no room in the store" << std::endl;
                skipped += static_cast<uint32_t>(placed.size());
            }
            else
            {
                std::vector<Clip> &clips = g_clips[1];
                for (const NewClip &item : placed)
                {
                    Clip clip;
                    clip.bank = 1;
                    clip.index = static_cast<uint32_t>(clips.size());
                    clip.address = item.clip->address;
                    clip.size = static_cast<uint32_t>(item.clip->bytes.size());
                    clip.name = item.name;
                    clip.from = item.clip->modName;
                    pointIndexAt(1, clip.index, clip.address);
                    std::cerr << "[mods] " << clip.from << ": added " << clip.name << " as clip " << clip.index << " ("
                              << lastFrameOf(item.clip->bytes) + 1u << " frames, " << clip.size << " bytes)" << std::endl;
                    clips.push_back(clip);
                }
                write32(kBank1 + 8u, static_cast<uint32_t>(clips.size()));
                addedCount = static_cast<uint32_t>(placed.size());
            }
        }
        indexGameNames();

        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
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
                ModClip *modClip = loadModClip(mod, fileValue.text, "replace " + gameName + ":");
                if (!modClip || !placeModClip(*modClip))
                {
                    ++skipped;
                    continue;
                }
                for (Clip *clip : found->second)
                {
                    const uint32_t oldLastFrame = read16(clip->address);
                    if (clip->from != "game")
                        std::cerr << "[mods] " << mod.name << ": " << clip->name << " was already replaced by " << clip->from
                                  << " - " << mod.name << "'s is used (later in the load order)" << std::endl;
                    else if (clip->address >= kBank0 && clip->address < 0x2000000u)
                        g_unused.push_back({clip->address, clip->address + clip->size});
                    pointIndexAt(clip->bank, clip->index, modClip->address);
                    std::cerr << "[mods] " << mod.name << ": replaced " << clip->name << " (bank " << clip->bank << " clip "
                              << clip->index << ", id " << (clip->bank == 0 ? 10000u + clip->index : clip->index) << ", "
                              << oldLastFrame + 1u << " frames) with " << modClip->fileName << " ("
                              << lastFrameOf(modClip->bytes) + 1u << " frames, " << modClip->bytes.size() << " bytes)"
                              << std::endl;
                    clip->address = modClip->address;
                    clip->size = static_cast<uint32_t>(modClip->bytes.size());
                    clip->from = mod.name;
                    ++replaced;
                }
            }
        }
        std::cerr << "[mods] animations: " << replaced << " replaced, " << addedCount << " added";
        if (addedCount)
            std::cerr << " (clips " << kExpectedCount1 << "-" << kExpectedCount1 + addedCount - 1u << ", " << moved
                      << " game clips moved to make room)";
        if (skipped)
            std::cerr << ", " << skipped << " skipped";
        std::cerr << std::endl;
    }

    // A move record field (25 bytes, bit-packed MSB-first, FUN_0011c608): field 28 = the clip, at bit 163, 12 bits.
    constexpr uint32_t kClipBit = 163u, kClipBits = 12u;

    uint32_t recordField(const uint8_t *record, uint32_t bit, uint32_t bits)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < bits; ++i, ++bit)
            value = (value << 1u) | ((record[bit >> 3u] >> (7u - (bit & 7u))) & 1u);
        return value;
    }
    void setRecordField(uint8_t *record, uint32_t bit, uint32_t bits, uint32_t value)
    {
        for (uint32_t i = 0; i < bits; ++i, ++bit)
        {
            const uint8_t mask = static_cast<uint8_t>(1u << (7u - (bit & 7u)));
            if ((value >> (bits - 1u - i)) & 1u)
                record[bit >> 3u] |= mask;
            else
                record[bit >> 3u] &= static_cast<uint8_t>(~mask);
        }
    }

    // A wrestler attire's moveset: 128 u16 slots (flags in bits 12-15, the move record in bits 0-11; 0xFFFF = empty),
    // pointed to by the WAZA struct at +0x7C + 16 x record (record = the attire's row in the character table).
    uint32_t movesetAddress(uint32_t attire)
    {
        uint32_t address = read32(kWaza + kMovesets + 16u * attire);
        if (std::memcmp(g_rdram + (address & PS2_RAM_MASK), "WWAZ", 4) == 0)
            address += 8u; // the block's tag and size
        return address;
    }

    // The character table's rows (one per wrestler attire) whose name - full or short - is `name` (any case).
    std::vector<uint32_t> attiresOf(const std::string &name)
    {
        std::vector<uint32_t> rows;
        const std::string want = upper(name);
        for (uint32_t row = 0; row < kAttires; ++row)
        {
            const uint32_t entry = kCharacters + 0x20u * row;
            if (upper(guestString(read32(entry))) == want || upper(guestString(read32(entry + 4u))) == want)
                rows.push_back(row);
        }
        return rows;
    }

    // "wrestlers": { "<wrestler>": "<file>" }: the file lists moveset slots and what goes in them -
    // { "84": "Jericho Entrance" } - only the listed slots change. A new animation in a slot gets its own move: a copy
    // of the slot's move with the clip changed (so it keeps the slot's behaviour - an intro stays an intro), and the
    // clip takes the replaced clip's timing record (camera, frame markers) unless it already has one. The game's
    // 999 move records are copied to the store with room for the new ones (*(0x2331C8 + 0x14), count at + 0x08).
    void applyWrestlers()
    {
        std::unordered_map<std::string, uint32_t> newClips; // a new animation's name -> its clip id
        for (uint32_t i = kExpectedCount1; i < g_clips[1].size(); ++i)
            newClips[upper(g_clips[1][i].name)] = i;

        struct Change
        {
            uint32_t clip;
            std::string modName;
            std::string wrestler;
            std::string animation;
        };
        std::map<std::pair<uint32_t, uint32_t>, Change> changes; // (attire, slot) -> what goes there
        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            const vendetta_mods::Json *wrestlers = mod.manifest.find("wrestlers");
            if (!wrestlers)
                continue;
            if (!wrestlers->isObject())
            {
                std::cerr << "[mods] " << mod.name << ": \"wrestlers\" should be { \"<wrestler>\": \"<file>\" }" << std::endl;
                continue;
            }
            for (const auto &[wrestler, fileValue] : wrestlers->members)
            {
                const std::vector<uint32_t> attires = attiresOf(wrestler);
                if (attires.empty())
                {
                    std::cerr << "[mods] " << mod.name << ": the game has no wrestler called \"" << wrestler << "\" - skipped"
                              << std::endl;
                    continue;
                }
                const std::filesystem::path file =
                    fileValue.isString() ? vendetta_mods::modFile(mod, fileValue.text, "wrestler") : std::filesystem::path();
                std::vector<uint8_t> bytes;
                vendetta_mods::Json moveset;
                std::string error;
                if (file.empty() || !readFile(file, bytes) ||
                    !vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), moveset, error) || !moveset.isObject())
                {
                    std::cerr << "[mods] " << mod.name << ": " << wrestler << "'s file can't be read"
                              << (error.empty() ? "" : ": " + error) << " - skipped" << std::endl;
                    continue;
                }
                for (const auto &[slotText, value] : moveset.members)
                {
                    char *end = nullptr;
                    const long slot = std::strtol(slotText.c_str(), &end, 10);
                    if (slotText.empty() || *end != '\0' || slot < 0 || slot >= static_cast<long>(kSlots))
                    {
                        std::cerr << "[mods] " << mod.name << ": " << wrestler << ": \"" << slotText
                                  << "\" isn't a moveset slot (0-127) - skipped" << std::endl;
                        continue;
                    }
                    const auto found = value.isString() ? newClips.find(upper(value.text)) : newClips.end();
                    if (found == newClips.end())
                    {
                        std::cerr << "[mods] " << mod.name << ": " << wrestler << " slot " << slot << ": no new animation called \""
                                  << value.text << "\" (a mod's \"new\" list names them) - skipped" << std::endl;
                        continue;
                    }
                    for (uint32_t attire : attires)
                    {
                        Change &change = changes[{attire, static_cast<uint32_t>(slot)}];
                        if (!change.modName.empty() && change.modName != mod.name)
                            std::cerr << "[mods] " << mod.name << ": " << wrestler << " slot " << slot << " was also set by "
                                      << change.modName << " - " << mod.name << "'s is used (later in the load order)" << std::endl;
                        change = {found->second, mod.name, wrestler, g_clips[1][found->second].name};
                    }
                }
            }
        }
        if (changes.empty())
            return;

        // one new move per (the slot's move, the new clip)
        const uint32_t stride = read32(kWaza + 4u);
        const uint32_t oldRecords = read32(kWaza + kRecords);
        const uint32_t oldCount = read32(kWaza + 8u);
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> newRecords; // (base record, clip) -> new record
        std::vector<std::pair<uint32_t, uint32_t>> order;
        for (auto &[where, change] : changes)
        {
            const uint16_t value = read16(movesetAddress(where.first) + 2u * where.second);
            if (value == 0xFFFFu || (value & 0xFFFu) >= oldCount)
                continue;
            const auto key = std::make_pair(static_cast<uint32_t>(value & 0xFFFu), change.clip);
            if (!newRecords.count(key))
            {
                newRecords[key] = oldCount + static_cast<uint32_t>(order.size());
                order.push_back(key);
            }
        }
        const uint32_t count = oldCount + static_cast<uint32_t>(order.size());
        if (count > 0x1000u)
        {
            std::cerr << "[mods] wrestlers: " << count << " moves needed; a moveset slot holds 12 bits (4,096) - skipped" << std::endl;
            return;
        }
        uint32_t records = 0;
        if (!order.empty())
        {
            records = vendetta_store::allocate(vendetta_store::Area::Boot, count * stride, "move records", 16u, 0u);
            if (records == 0u)
                return;
            uint8_t *to = vendetta_store::host(records);
            std::memcpy(to, g_rdram + oldRecords, oldCount * stride);
            uint8_t *fzui = g_rdram + (read32(kWaza + kFzuiBank1) & PS2_RAM_MASK);
            for (size_t i = 0; i < order.size(); ++i)
            {
                const auto [base, clip] = order[i];
                uint8_t *record = to + (oldCount + i) * stride;
                std::memcpy(record, to + base * stride, stride);
                const uint32_t baseClip = recordField(record, kClipBit, kClipBits);
                setRecordField(record, kClipBit, kClipBits, clip);
                // the new clip's timing record: the one of the clip it stands in for, the first time it's used
                static const uint8_t empty[kFzuiSize] = {};
                if (std::memcmp(fzui + clip * kFzuiSize, empty, kFzuiSize) == 0 && baseClip < g_clips[1].size())
                    std::memcpy(fzui + clip * kFzuiSize, fzui + baseClip * kFzuiSize, kFzuiSize);
            }
            write32(kWaza + kRecords, records);
            write32(kWaza + 8u, count);
            g_unused.push_back({oldRecords, oldRecords + oldCount * stride});
        }

        const uint32_t recordArray = read32(kWaza + kRecords);
        uint32_t set = 0;
        for (auto &[where, change] : changes)
        {
            const uint32_t slotAddress = movesetAddress(where.first) + 2u * where.second;
            const uint16_t value = read16(slotAddress);
            if (value == 0xFFFFu || (value & 0xFFFu) >= oldCount)
            {
                std::cerr << "[mods] " << change.modName << ": " << change.wrestler << " (attire row " << where.first << ") slot "
                          << where.second << " is empty - a new animation needs a move to copy there (later) - skipped"
                          << std::endl;
                continue;
            }
            const uint32_t base = value & 0xFFFu;
            const uint32_t record = newRecords[{base, change.clip}];
            const uint8_t *baseRecord = g_rdram + (recordArray & PS2_RAM_MASK) + base * stride;
            const uint32_t baseClip = recordField(baseRecord, kClipBit, kClipBits);
            const uint16_t newValue = static_cast<uint16_t>((value & 0xF000u) | record);
            std::memcpy(g_rdram + (slotAddress & PS2_RAM_MASK), &newValue, 2);
            std::cerr << "[mods] " << change.modName << ": " << change.wrestler << " (attire row " << where.first << ") slot "
                      << where.second << ": move " << base << " (clip " << baseClip << " "
                      << (baseClip < g_clips[1].size() ? g_clips[1][baseClip].name : "?") << ") -> move " << record
                      << " (clip " << change.clip << " " << change.animation << ")" << std::endl;
            ++set;
        }
        std::cerr << "[mods] wrestlers: " << set << " moveset slots set, " << order.size() << " new moves" << std::endl;
    }

    // VENDETTA_ANIM_POISON=1: the bytes no index entry or timing pointer uses any more are filled with 0xFF - unless a
    // clip still in use overlaps them (two names can share one clip's bytes).
    void poisonUnused()
    {
        if (!envOn("VENDETTA_ANIM_POISON"))
            return;
        uint32_t bytes = 0;
        for (const auto &[from, to] : g_unused)
        {
            bool inUse = false;
            for (const std::vector<Clip> &clips : g_clips)
                for (const Clip &clip : clips)
                    if (clip.address < to && clip.address + clip.size > from)
                        inUse = true;
            if (inUse)
                continue;
            std::memset(g_rdram + from, 0xFF, to - from);
            bytes += to - from;
        }
        std::cerr << "[anim] VENDETTA_ANIM_POISON: " << bytes << " unused bytes filled with 0xFF (" << g_unused.size()
                  << " ranges)" << std::endl;
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
        applyWrestlers();
        poisonUnused();
        vendetta_store::report();
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

    // A fighter playing a moveset slot (FUN_00102938): it reads the slot before it starts the clip, so the mods must
    // be in by then too.
    void playSlot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        prepare(rdram);
        g_originalPlaySlot(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        g_originalPlaySlot = runtime.lookupFunction(kPlaySlot);
        if (g_originalPlaySlot)
            runtime.replaceFunction(kPlaySlot, playSlot);
        g_originalPlayClip = runtime.lookupFunction(kPlayClip);
        if (g_originalPlayClip)
            runtime.replaceFunction(kPlayClip, playClip);
    }
}

void installVendettaAnimations(PS2Runtime &runtime) { vendetta_animations::install(runtime); }

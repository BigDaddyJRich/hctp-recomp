// Def Jam Vendetta (USA, SLUS-20639) — the mods\ folder: finding mods, reading their mod.json, dependencies, order.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// mods\<folder>\mod.json says what a mod does; its files sit in folders by type (animations\, wrestlers\, …) under any
// names. A mod tool writes this; it is plain text, so it can be edited by hand too:
//
//   {
//     "name": "Briggs Makeover", "author": "…", "version": 1, "description": "…",
//     "needs": ["No Mercy Entrances"],                       other mods this one uses (they load first)
//     "replace": {                                           things the game already has, by the game's names:
//       "animations": { "N61A1_AP": "animations/Jericho Entrance.bin" }    changed everywhere they are used
//     },
//     "new": { "animations": ["animations/Jericho Entrance.bin"] },        things the game doesn't have
//     "wrestlers": { "Briggs": "wrestlers/Briggs.json" }                  slot changes per wrestler (later)
//   }
//
// Load order: mods\order.txt (one mod folder per line; a later one wins when two change the same thing), the others
// alphabetically, and a mod always after the mods it needs. A mod whose needs are missing is skipped, with a message.
// VENDETTA_MODS=0: none (the plain game). VENDETTA_MODS_DIR=<folder>: another mods folder (test runs).
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vendetta_mods
{
    // A small JSON value (what mod.json needs: objects, arrays, strings, numbers, true / false / null).
    struct Json
    {
        enum class Type
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object,
        };
        Type type = Type::Null;
        bool boolean = false;
        double number = 0.0;
        std::string text;
        std::vector<Json> items;                                  // an array
        std::vector<std::pair<std::string, Json>> members;        // an object, in file order

        const Json *find(const std::string &key) const;           // an object's member, or null
        bool isObject() const { return type == Type::Object; }
        bool isArray() const { return type == Type::Array; }
        bool isString() const { return type == Type::String; }
    };

    // Parses JSON text; false (and why, with the line) on an error.
    bool parseJson(const std::string &text, Json &out, std::string &error);

    struct Mod
    {
        std::string folderName;            // the folder under mods\ (what order.txt and "needs" name)
        std::string name;                  // mod.json's "name" (else the folder's)
        std::filesystem::path folder;
        Json manifest;
    };

    // The enabled mods in load order (read once; later calls return the same list).
    const std::vector<Mod> &loadedMods();

    // A file a mod.json names ("animations/x.bin", relative to the mod's folder), as a path; empty if it leaves the
    // mod's folder or doesn't exist (with a message).
    std::filesystem::path modFile(const Mod &mod, const std::string &relative, const char *what);
}

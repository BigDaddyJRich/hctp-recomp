#pragma once
#include <cstdint>

namespace hctp_media_defaults {
struct MediaDefault { int music = -1, video = -1; };
// The original record layout, confirmed by CCharProp's memcpy and the CAM drawer.
// Deliberate non-default choices are preserved. No writes outside these two bytes.
inline bool apply(uint8_t *record, const uint8_t *original, const MediaDefault &choice)
{
    bool changed = false;
    if (choice.music >= 0 && choice.music <= 255 && record[0x15f] == original[0x15f])
    {
        changed |= record[0x15f] != choice.music;
        record[0x15f] = static_cast<uint8_t>(choice.music);
    }
    if (choice.video >= 0 && choice.video <= 255 && record[0x15e] == original[0x15e])
    {
        changed |= record[0x15e] != choice.video;
        record[0x15e] = static_cast<uint8_t>(choice.video);
    }
    return changed;
}
}

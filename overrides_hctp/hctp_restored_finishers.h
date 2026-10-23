#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hctp_restored_finishers {
inline bool restored(uint16_t id) {
    switch (id) {
    case 13: case 17: case 24: case 32: case 33: case 35:
    case 36: case 41: case 43: case 57: case 77: case 78:
    case 50: case 52: case 53: case 54: case 55: case 65: return true;
    default: return false;
    }
}
inline bool range(size_t size, uint32_t addr, size_t bytes) {
    return addr <= size && bytes <= size - addr;
}
inline uint32_t u32(const uint8_t *ram, uint32_t addr) {
    uint32_t value; std::memcpy(&value, ram + addr, 4); return value;
}
inline uint16_t u16(const uint8_t *ram, uint32_t addr) {
    uint16_t value; std::memcpy(&value, ram + addr, 2); return value;
}
// Mirrors CSSNameListReader::ReadEndProcess's GetSmackSituation calls, but
// ONLY for the restored records appended AFTER that original loop ends.
// Use live character data, not preset constants: preserve user-assigned moves.
// The singleton/table layout is from 0x245520 / 0x245220 / 0x245420.
inline bool refresh(uint8_t *ram, size_t size, uint32_t dest, uint16_t id) {
    if (!restored(id) || !range(size, dest, 0x80) || !range(size, 0x529318, 4)) return false;
    const uint32_t prop = 0x4c0f54u + uint32_t(id) * 0x170u;
    if (!range(size, prop, 0x24)) return false;
    const uint32_t self = u32(ram, 0x529318);
    if (!self || !range(size, self, 0x34)) return false;
    const uint32_t count = u32(ram, self + 0x28), table = u32(ram, self + 0x30);
    if (!count || count > 7000 || !table || !range(size, table, size_t(count) * 36)) return false;
    uint8_t result[68] = {};
    for (uint32_t slot = 0; slot < 2; ++slot) {
        const uint16_t move = u16(ram, prop + 0x20 + slot * 2);
        bool found = false;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t row = table + k * 36;
            if (u16(ram, row + 0x20) != move) continue;
            std::memcpy(result + slot * 34, ram + row, 32);
            result[slot * 34 + 31] = 0;
            const uint8_t code = ram[row + 0x22];
            result[slot * 34 + 32] = code == 0xff ? 0 : code;
            found = true;
            break;
        }
        // Failure is atomic: never leave one field refreshed and one stale.
        if (!found) return false;
    }
    std::memcpy(ram + dest + 0x24, result, sizeof(result));
    return true;
}
} // namespace hctp_restored_finishers

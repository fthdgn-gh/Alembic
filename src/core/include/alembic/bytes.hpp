// SPDX-License-Identifier: GPL-3.0-or-later
// Little-endian load/store helpers.
#pragma once

#include <cstddef>
#include <cstdint>

namespace alembic {

inline void put_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t get_u32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

constexpr std::uint64_t div_round_up(std::uint64_t value, std::uint64_t divisor) {
    return value == 0 ? 0 : (value - 1) / divisor + 1;
}
constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return div_round_up(value, alignment) * alignment;
}

} // namespace alembic

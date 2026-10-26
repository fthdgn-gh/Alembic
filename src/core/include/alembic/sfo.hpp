// SPDX-License-Identifier: GPL-3.0-or-later
// param.sfo: the PS4's (and PS3/Vita's) title parameters, the PS4 counterpart
// of a PS5 game's sce_sys/param.json. A PSF file, little-endian:
//   0x00 "\0PSF"  0x04 u32 version  0x08 u32 key table  0x0C u32 data table
//   0x10 u32 count, then `count` 16-byte rows at 0x14:
//   {u16 key offset, u16 format, u32 used length, u32 max length, u32 data offset}
// Formats: 0x0004 / 0x0204 UTF-8 text (NUL-terminated), 0x0404 u32.
#pragma once

#include "alembic/io.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace alembic::sfo {

struct Param {
    std::map<std::string, std::string> text;
    std::map<std::string, std::uint32_t> numbers;

    std::optional<std::string> get(const std::string& key) const {
        auto it = text.find(key);
        return it == text.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
};

bool looks_like_sfo(Bytes data);
Result<Param> parse(Bytes data);

} // namespace alembic::sfo

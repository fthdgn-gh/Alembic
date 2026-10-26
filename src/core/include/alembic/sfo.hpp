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
#include <vector>

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

// An editable param.sfo, read and written the way maxton/LibOrbisPkg's
// ParamSfo does (LGPL-3.0, SFO/ParamSfo.cs): values sorted by key in .NET's
// string order, each in a slot of its maximum length, the data table aligned
// to 4 bytes.
inline constexpr std::uint16_t kUtf8Special = 0x0004, kUtf8 = 0x0204, kInteger = 0x0404;
struct Value {
    std::string key;
    std::uint16_t format = kUtf8;
    std::string text;          // UTF-8 formats
    std::uint32_t number = 0;  // kInteger
    std::uint32_t max_length = 4;
};
class Document {
public:
    static Result<Document> read(Bytes data);
    const Value* find(const std::string& key) const;
    void set_text(const std::string& key, const std::string& text, std::uint32_t max_length);
    void set_number(const std::string& key, std::uint32_t number);
    void remove(const std::string& key);
    // What LibOrbisPkg's Value.ToString() gives: the text, or "0x%08x" for a
    // number; "" when the key is missing.
    std::string display(const std::string& key) const;
    std::vector<std::uint8_t> serialize() const;
    const std::vector<Value>& values() const { return values_; }

private:
    void sort();
    std::vector<Value> values_;
};

// The sce_sys/param.json a PS4 game needs next to its param.sfo to start from
// an image or folder through ShadowMount+: title names per language
// (TITLE_00..29), an en-US fallback, the title ID and name, and any non-zero
// USER_DEFINED_PARAM_1..4. Ported from SadykovIV/PS4pkg_to_ffpfsc (GPL-3.0)
// sfo.py build_param_json and choose_title, byte for byte (sorted keys,
// 2-space indent, UTF-8, final newline). No contentId: that is what tells it
// from a PS5 game's param.json.
std::string ps4_param_json(const Document& sfo, const std::string& title_id);

} // namespace alembic::sfo

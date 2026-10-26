// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/sfo.hpp"

#include "alembic/bytes.hpp"

#include <cstring>

namespace alembic::sfo {

namespace {

constexpr std::size_t kHeader = 0x14;
constexpr std::size_t kRow = 0x10;
constexpr std::uint32_t kMaxEntries = 4096;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, "param.sfo: " + message);
}

} // namespace

bool looks_like_sfo(Bytes data) {
    return data.size() >= kHeader && std::memcmp(data.data(), "\0PSF", 4) == 0;
}

Result<Param> parse(Bytes data) {
    if (!looks_like_sfo(data)) return invalid("no PSF header");
    std::uint32_t keys = get_u32(data.data() + 0x08);
    std::uint32_t values = get_u32(data.data() + 0x0C);
    std::uint32_t count = get_u32(data.data() + 0x10);
    if (count > kMaxEntries || kHeader + std::uint64_t(count) * kRow > data.size() || keys > data.size() ||
        values > data.size())
        return invalid("the tables lie outside the file");
    Param out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* row = data.data() + kHeader + std::size_t(i) * kRow;
        std::uint64_t key_at = std::uint64_t(keys) + get_u16(row);
        std::uint16_t format = get_u16(row + 2);
        std::uint32_t length = get_u32(row + 4);
        std::uint64_t value_at = std::uint64_t(values) + get_u32(row + 12);
        if (key_at >= data.size() || value_at > data.size() || length > data.size() - value_at)
            return invalid("an entry lies outside the file");
        const char* key_start = reinterpret_cast<const char*>(data.data() + key_at);
        const void* key_end = std::memchr(key_start, 0, data.size() - key_at);
        if (!key_end) return invalid("a key is not terminated");
        std::string key(key_start, static_cast<const char*>(key_end));
        const std::uint8_t* value = data.data() + value_at;
        if (format == 0x0404) {
            if (length < 4) return invalid("a number is too short");
            out.numbers[key] = get_u32(value);
        } else if (format == 0x0004 || format == 0x0204) {
            std::size_t n = 0;
            while (n < length && value[n] != 0) ++n;
            out.text[key] = std::string(reinterpret_cast<const char*>(value), n);
        }
    }
    return out;
}

} // namespace alembic::sfo

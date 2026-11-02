// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers shared by the zstd pack format, builder and reader.
#pragma once

#include "alembic/zstdpack.hpp"

#include <string>

namespace alembic::zstdpack::detail {

// CRC-32 (IEEE), as crc32fast.
std::uint32_t crc(Bytes data, std::uint32_t seed = 0);
// The index order: (path_hash, path bytes).
bool key_less(const std::string& a, const std::string& b);

} // namespace alembic::zstdpack::detail

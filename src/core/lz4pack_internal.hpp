// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers shared by the LZ4 pack format, builder and reader.
#pragma once

#include "alembic/lz4pack.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::lz4pack::detail {

std::uint32_t crc(Bytes data, std::uint32_t seed = 0);
// canonical_asset_path() of ampr_pack_format.py: "/app0/..." or nullopt.
std::optional<std::string> canonical_asset_path(std::string_view path);
// safe_output_path()'s textual checks for a relative name.
bool is_safe_relative(std::string_view name);
std::vector<std::uint8_t> encode_file_records(const std::vector<FileRecord>& files);
std::vector<std::uint8_t> encode_chunk_records(const std::vector<ChunkRecord>& chunks);
std::vector<std::uint8_t> encode_pack_records(const std::vector<PackRecord>& packs);

} // namespace alembic::lz4pack::detail

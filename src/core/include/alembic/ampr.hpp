// SPDX-License-Identifier: GPL-3.0-or-later
// AMPRIDX3: a path index added to dumps that ship fakelib/libSceAmpr.sprx.
// Ported from PS5PKGTool's AmprIndex.cs.
//
// Layout: 48-byte header ("AMPRIDX3", version 3, record size 24, row count,
// path blob length, hash table offset, slot size 16, slot count), then 24-byte
// records {u32 path offset, u32 path length, u64 size, i64 mtime}, then the
// NUL-terminated UTF-8 paths ("/app0/..."), then a 16-byte aligned open-address
// hash table of {u64 FNV-1a hash, u32 row index + 1, u32 collision flag}.
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alembic::ampr {

struct FileRecord {
    std::string relative_path; // UTF-8, '/' separated, relative to the dump root
    std::uint64_t size = 0;
    std::int64_t unix_modified_time = 0;
};

struct IndexInfo {
    std::uint32_t record_count = 0;
    std::uint32_t slot_count = 0;
    std::uint64_t path_blob_length = 0;
};

// Builds the index. Returns an empty vector when there are no files.
Result<std::vector<std::uint8_t>> build(const std::vector<FileRecord>& files);

// Structural validation.
Result<IndexInfo> inspect(const std::vector<std::uint8_t>& data);

// FNV-1a-style hash over the code points of the lower-cased path; never 0.
// Uses PS5PKGTool's non-standard offset basis (see ampr.cpp).
Result<std::uint64_t> hash_path(const std::string& utf8_path);

} // namespace alembic::ampr

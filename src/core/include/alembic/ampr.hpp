// SPDX-License-Identifier: GPL-3.0-or-later
// AMPRIDX3: a path index added to dumps that ship fakelib/libSceAmpr.sprx. It
// is read on the console by drakmor/ampr_emu. Ported from PS5PKGTool's
// AmprIndex.cs, with ampr_emu's own rules as the default.
//
// Layout: 48-byte header ("AMPRIDX3", version 3, record size 24, row count,
// path blob length, hash table offset, slot size 16, slot count), then 24-byte
// records {u32 path offset, u32 path length, u64 size, i64 mtime}, then the
// NUL-terminated UTF-8 paths ("/app0/..."), then a 16-byte aligned open-address
// hash table of {u64 hash, u32 row index + 1, u32 duplicate-hash flag}.
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alembic::ampr {

// How paths are folded for hashing, sorting and de-duplication.
enum class Scheme {
    // What ampr_emu's reader and tools/build_ampr_index.py do: UTF-8 bytes,
    // only A-Z lower-cased. The only scheme the console accepts for non-ASCII
    // names: on load the reader recomputes every hash and rejects the whole
    // index on any mismatch.
    ampr_emu,
    // What PS5PKGTool does: Unicode code points after .NET ToLowerInvariant.
    // Identical to ampr_emu for all-ASCII paths. Kept for byte-for-byte tests.
    ps5pkgtool,
};

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

// Builds the index. Returns an empty vector when there are no files. Paths that
// fold to the same key keep the first record only.
Result<std::vector<std::uint8_t>> build(const std::vector<FileRecord>& files, Scheme scheme = Scheme::ampr_emu);

// Structural validation.
Result<IndexInfo> inspect(const std::vector<std::uint8_t>& data);

// FNV-1a-style hash of the folded path; never 0. Both schemes use the offset
// basis 1469598103934665603 (the standard one with a digit missing), which is
// what ampr_emu reads.
Result<std::uint64_t> hash_path(const std::string& utf8_path, Scheme scheme = Scheme::ampr_emu);

} // namespace alembic::ampr

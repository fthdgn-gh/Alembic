// SPDX-License-Identifier: GPL-3.0-or-later
// A read-only picture of folders on the console, for finding out what the
// system's installer writes (NOTES, 2026-11-26: installing without the
// extra copies). POST /api/snapshot starts one, GET /api/snapshot says how
// far it is. It reads the roots and writes only under `out`:
//   out/listing.tsv  one line per entry: kind, size, mtime, SHA-256 (files
//                    up to hash_max, else "-"), path; "error" lines for
//                    what could not be read
//   out/files/...    copies of the files under `copy` and of every file
//                    whose path contains `match` (up to copy_max each),
//                    at their own paths
//   out/summary.json what was asked, counts, errors, seconds
// tools/snapshot/diff.py compares two of them on a PC.
#pragma once

#include "json_value.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alembic::engine::snapshot {

struct Options {
    std::string out;                      // a new folder (must not exist)
    std::vector<std::string> roots;       // folders to list
    std::vector<std::string> copy;        // files or folders to copy whole
    std::string match;                    // also copy files whose path has it
    std::uint64_t hash_max = 64ull << 20; // hash files up to this size
    std::uint64_t copy_max = 256ull << 20;
    int max_depth = 24;
};

// The defaults for the console: /user and /system_data listed,
// /system_data/priv/mms (app.db and its siblings) copied.
Options defaults();
// Reads a request body over the defaults; an error message when it is wrong.
std::string parse(const JsonValue& body, Options& options);

// Starts one on its own thread; false when one is running.
bool start(const Options& options);
// {running, out, entries, files_hashed, copied, errors, seconds, error}
JsonValue status();
// Runs one on this thread (tests, the CLI).
JsonValue run(const Options& options);

} // namespace alembic::engine::snapshot

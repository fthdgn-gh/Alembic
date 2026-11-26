// SPDX-License-Identifier: GPL-3.0-or-later
// Experiments that write to the console's system files and databases, for
// finding out what installing without the system installer takes (NOTES,
// 2026-11-26: the replay). Built only with -DALEMBIC_EXPERIMENTS=ON; release
// payloads do not contain it.
//
// POST /api/experiment {"dry_run": bool, "ops": [...]} runs the operations in
// order on a thread of their own and stops at the first that fails;
// GET /api/experiment says how far it is and what each one did.
//   {"op":"backup_db", "db": path, "to": new file}   SQLite's online backup
//   {"op":"mkdir", "path", "mode"}                    an existing folder is fine
//   {"op":"copy", "path", "mode", "from", "offset"?, "length"?}
//        `length` bytes of `from` at `offset` (default: all of it) into a new
//        file, written next to it under a '.' name and renamed when complete
//   {"op":"remove", "path"}                           a file or an empty folder;
//                                                     a missing one is fine
//   {"op":"sql", "db": path, "statements": [{"sql", "params": [...],
//        "changes"?: n}]}  one transaction; a statement that changes another
//        number of rows than `changes` rolls it all back
// A dry run checks every source, and runs each "sql" op's statements inside
// a transaction that it then rolls back; it writes nothing.
#pragma once

#include "json_value.hpp"

namespace alembic::engine::experiment {

// Starts a run; an error message when one is running or the body is wrong.
std::string start(const JsonValue& body);
JsonValue status();
// Runs one on this thread (tests).
JsonValue run(const JsonValue& body);

} // namespace alembic::engine::experiment

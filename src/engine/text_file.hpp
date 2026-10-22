// SPDX-License-Identifier: GPL-3.0-or-later
// Small text files in the engine's state folder (jobs.json, peers.json, ...):
// read whole, written whole through AtomicOutput (a crash leaves the old one).
#pragma once

#include "alembic/result.hpp"

#include <cstddef>
#include <string>

namespace alembic::engine {

Result<std::string> read_text(const std::string& path, std::size_t max_bytes = 64u << 20);
Status write_text(const std::string& path, const std::string& text);

} // namespace alembic::engine

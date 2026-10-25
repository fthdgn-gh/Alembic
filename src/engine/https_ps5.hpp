// SPDX-License-Identifier: GPL-3.0-or-later
// HTTPS downloads on the console (engine payload and app): see https_ps5.cpp.
#pragma once

#include "alembic/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alembic::engine {

// GETs `url` into memory (redirects followed; at most `max_bytes`). Only
// https:// URLs of hosts whose CA github_ca.inc carries.
Result<std::vector<std::uint8_t>> https_get(const std::string& url, std::size_t max_bytes);

} // namespace alembic::engine

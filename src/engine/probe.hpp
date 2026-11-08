// SPDX-License-Identifier: GPL-3.0-or-later
// What a process on the console may do: the same checks run in the engine
// payload (GET /api/probe) and in a game process (the Probe Bench dev title,
// app/bench), so the two can be compared before moving work into the app.
// Plain libc and libkernel calls only, none of the payload's kernel helpers,
// so the app links it too. On a PC it says so and measures CPU only.
#pragma once

#include "json_value.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace alembic::engine::probe {

struct Options {
    std::string process;       // "engine" or "app": goes into the result
    bool io = true;            // write and read 256 MB where it can
    bool https = true;         // reach api.github.com
    std::uint64_t mem_cap_mb = 2048;  // stop the mapping test there
    // Called before each section ("cpu", "files", ...), for a status line.
    std::function<void(const std::string&)> step;
};

JsonValue run(const Options& options);

} // namespace alembic::engine::probe

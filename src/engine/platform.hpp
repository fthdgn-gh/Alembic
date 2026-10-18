// SPDX-License-Identifier: GPL-3.0-or-later
// What differs between the PS5 payload and a PC build of the engine.
#pragma once

#include <string>
#include <vector>

namespace alembic::engine::platform {

const char* name(); // "ps5" or "host"
// A system notification on the PS5; a line on stderr elsewhere.
void notify(const std::string& message);
// Keeps the console out of rest mode while a job runs; call every few seconds.
void keep_awake();
// Where browsing, inputs and outputs are allowed by default.
std::vector<std::string> default_roots();
// Where the engine keeps its queue and log.
std::string default_state_dir();

} // namespace alembic::engine::platform

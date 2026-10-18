// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.hpp"

#include <cstdio>
#include <cstdlib>

namespace alembic::engine::platform {

const char* name() { return "host"; }

void notify(const std::string& message) { std::fprintf(stderr, "[notify] %s\n", message.c_str()); }

void keep_awake() {}

std::vector<std::string> default_roots() {
    const char* home = std::getenv("HOME");
    return {home && *home ? home : "/"};
}

std::string default_state_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.alembic";
}

} // namespace alembic::engine::platform

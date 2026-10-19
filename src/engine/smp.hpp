// SPDX-License-Identifier: GPL-3.0-or-later
// A small client for ShadowMount+'s local HTTP/JSON API (docs/api.md in
// drakmor/ShadowMountPlus): every call is a POST of one JSON object. The engine
// uses it to ask which source ShadowMount+ runs a title from and to request a
// rescan after a conversion, so a new image is picked up without uninstalling
// the game.
//
// ShadowMount+ keeps one source per title (its mount.lnk) and sets any other
// source of that title aside while the remembered one exists. Once that one is
// gone, its pre-scan cleanup drops the link (the game stays installed) and the
// next scan links the title to the source that is left. Its scanner skips names
// that start with '.', and finds images only by their extension, so renaming
// an old source to ".name" makes it switch.
#pragma once

#include "json_value.hpp"

#include "alembic/result.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace alembic::engine::smp {

struct Endpoint {
    bool enabled = false;
    std::string address = "127.0.0.1";
    std::uint16_t port = 10101;
};

// From ShadowMount+'s config.ini (api_enabled, api_bind_address, api_port);
// its defaults when a key is missing, disabled when the file is missing.
Endpoint read_endpoint(const std::string& config_ini);
// "HOST:PORT" (for tests on a PC).
std::optional<Endpoint> parse_endpoint(const std::string& text);

// POSTs `body` to `path` and parses the JSON reply. Fails when nothing listens,
// on a timeout, or when the reply's "status" is not 0.
Result<JsonValue> post(const Endpoint& endpoint, const std::string& path, const JsonValue& body,
                       int timeout_ms = 4000);

// The physical source (image file or folder) ShadowMount+ uses for the title,
// or nullopt when it does not know the title (or it is an installed package).
Result<std::optional<std::string>> game_source(const Endpoint& endpoint, const std::string& title_id);

// Queues a full rescan; `reset_attempts` also clears its retry counters (a
// title whose earlier images failed to mount is otherwise skipped).
Status rescan(const Endpoint& endpoint, bool reset_attempts);

// "/a/b/name" -> "/a/b/.name" (and back): the name ShadowMount+ does not scan.
std::string hidden_path(const std::string& path);
bool is_hidden(const std::string& path);

} // namespace alembic::engine::smp

// SPDX-License-Identifier: GPL-3.0-or-later
// What differs between the PS5 payload and a PC build of the engine.
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
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
// Whether a root can be used now. On the PS5 the /mnt/usbN and /mnt/extN
// folders always exist; only a mounted drive counts (writing into an empty
// mount folder would land on the system storage).
bool root_available(const std::string& root);
// Suggested destinations: label + folder (it may not exist yet; outputs
// create their folders). Only for available roots.
struct Location {
    std::string label;
    std::string path;
};
std::vector<Location> suggested_locations();
// Hands a .pkg file to the system installer (it appears in Downloads and
// installs in the background). Not available on a PC.
Status install_package(const std::string& path);
// Downloads `url` (HTTPS, redirects followed) into memory, at most
// `max_bytes`. On the PS5 the trust anchors are the GitHub CA certificates in
// github_ca.inc; on a PC it runs curl.
Result<std::vector<std::uint8_t>> http_get(const std::string& url, std::size_t max_bytes);
// Where the engine keeps its queue and log.
std::string default_state_dir();

} // namespace alembic::engine::platform

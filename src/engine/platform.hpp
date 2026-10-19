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
// The console's "Enter button" setting: "cross" or "circle" ("" when unknown,
// or not on a PS5). The page shows its button hints to match.
std::string enter_button();
// The console's (or the PC's) language as a two-letter code: "tr", "ja", ...
std::string system_language();
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

// The home-screen shortcut: a stub title in the Media tab whose param.json
// has a deeplinkUri, so the console opens the engine's page in its browser.
// Only on the PS5.
inline constexpr const char* kLauncherTitleId = "ALEM00001";
bool launcher_supported();
// How the system installer was found, or why not (for Settings).
std::string launcher_problem();
// Whether the shortcut is in place: its files in /user/app are there (the
// system keeps no record Alembic can rely on across firmwares).
bool launcher_installed();
// Whether the system's copy in /user/appmeta/<id>/param.json exists.
bool launcher_recorded();
// What /user/appmeta/<id> holds, for Settings ("no folder", or its files).
std::string launcher_record_note();
// Whether /user/app/<id>/sce_sys holds exactly the files install_launcher
// would write for this port and icon (it removes them when registering fails).
bool launcher_current(std::uint16_t port, const std::uint8_t* icon_png, std::size_t icon_size);
// Writes /user/app/<id>/sce_sys/{param.json,icon0.png} pointing at
// http://127.0.0.1:<port>/ and registers it. Does nothing when those files are
// already current, unless `again` (then it unregisters and registers it anew):
// on firmwares without AppInstallTitleDir, registering re-registers every app
// in /user/app, which has crashed a game that was starting at the time.
Status install_launcher(std::uint16_t port, const std::uint8_t* icon_png, std::size_t icon_size, bool again = false);
// Unregisters the shortcut and deletes its files.
Status remove_launcher();

} // namespace alembic::engine::platform

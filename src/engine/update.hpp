// SPDX-License-Identifier: GPL-3.0-or-later
// Updating the engine from Alembic's GitHub releases.
//
// The engine asks GitHub for the latest release; when it is newer, it
// downloads the payload (alembic-vX.Y.Z.elf) and the release's SHA256SUMS,
// checks the file against both the checksum and the version written inside it,
// and then:
//   1. replaces the copies of the payload that loaders start it from: Payload
//      Manager's slots (renaming the file to the new version and fixing the
//      name in that slot's meta.json by a plain text replace, as its schema is
//      not ours), autoloader folders (contents only: their autoload.txt names
//      the file) and anything a search of the drives found;
//   2. hands the new payload to the ELF loader (port 9021) and stops. The new
//      engine asks any engine still on its port to stop and waits for it
//      (see take_over_port in main.cpp); jobs with a checkpoint continue.
//
// A payload is recognised by the marker "ALEMBIC_PAYLOAD_VERSION:X.Y.Z" in it
// (0.2.3 and later) or, for older releases, by its SHA-256.
//
// The console app (from 0.3.0) shares the version and the release: its image
// (alembic-app-X.Y.Z.ffpkg) is replaced in the same update when its SHA-256
// differs from the release's. ShadowMount+ runs it, so the new image is put
// next to the old one under a new name and the old one deleted: ShadowMount+
// then mounts the new one. While the app is open that waits (the new image
// is staged under a '.' name ShadowMount+ skips, noted in
// <state>/update/app-pending.json) until the app has been quiet for a while;
// a new engine finishes what an old one staged.
#pragma once

#include "json_value.hpp"

#include "alembic/io.hpp"
#include "alembic/result.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace alembic::engine::update {

inline constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/fthdgn-gh/Alembic/releases/latest";
inline constexpr const char* kMarker = "ALEMBIC_PAYLOAD_VERSION:";
inline constexpr const char* kAppTitleId = "PPSA99594";

// < 0, 0, > 0 like strcmp, by number ("0.2.10" > "0.2.9"; a leading "v" is ignored).
int compare_versions(const std::string& a, const std::string& b);

struct Release {
    std::string version; // "0.2.3"
    std::string notes;   // the release's text
    std::string page_url;
    std::string elf_name, elf_url;
    std::uint64_t elf_size = 0;
    std::string sums_url;
    std::string app_name, app_url; // the console app's image ("" before 0.3.0)
    std::uint64_t app_size = 0;
};
// GitHub's "latest release" reply.
Result<Release> parse_release(std::string_view json);
// The checksum SHA256SUMS lists for `name` ("" when it isn't there).
std::string checksum_for(std::string_view sums, const std::string& name);

// What a file is: an Alembic payload or not, and which version ("" when it
// is one but the version is unknown).
struct PayloadId {
    bool alembic = false;
    std::string version;
};
PayloadId identify(Bytes data);

// A payload's file name for the new version: the old version in it replaced
// ("alembic-v0.2.2.elf" -> "alembic-v0.2.3.elf"); the same name when it holds none.
std::string renamed(const std::string& name, const std::string& old_version, const std::string& new_version);
// `text` with every `from` replaced by `to`; `count` gets how many.
std::string replace_all(std::string text, const std::string& from, const std::string& to, int* count = nullptr);

struct Found {
    std::string path;
    std::string version;   // of the file found ("" when unknown)
    std::string rename_to; // the new file name, "" to keep the name
    std::string loader;    // "Payload Manager", "autoloader" or ""
};
// Alembic payloads (*.elf, and *.bin in loader folders) in `dirs`, down to
// `depth` folder levels; `skip` folders are not entered.
std::vector<Found> find_payloads(const std::vector<std::string>& dirs, int depth, const std::string& new_version,
                                 const std::vector<std::string>& skip = {});
// Where loaders keep payloads (those that exist): Payload Manager's slots and
// the autoloaders' ps5_autoloader folders, on the internal drive and USB.
std::vector<std::string> usual_places();
// Writes `elf` over the found file, or under its new name (then fixes the
// name in the folder's meta.json and removes the old file).
Status replace_payload(const Found& found, const std::string& elf);

// "00.003.000" (param.json's contentVersion) -> "0.3.0"; other text as it is.
std::string app_version_text(const std::string& content_version);
// The console app's image name for a version, unused in `dir` (a suffix when taken).
std::string app_image_name(const std::string& dir, const std::string& version);

// Sends a payload to an ELF loader ("127.0.0.1:9021").
Status send_to_loader(const std::string& address, const std::string& elf);

// The update state the page shows, and the work behind its buttons.
class Updater {
public:
    struct Options {
        std::string state_dir;
        std::string current_version;
        std::string release_url = kLatestReleaseUrl;
        std::string loader; // "host:port"; "" when the payload can't be started here (a PC)
        // Downloads into memory (platform::http_get).
        std::function<Result<std::vector<std::uint8_t>>(const std::string& url, std::size_t max_bytes)> get;
        std::function<void(const std::string&)> log;
        // Called once the new engine was handed to the loader: stop this one.
        std::function<void()> quit;
        // Folders a search of the drives covers.
        std::function<std::vector<std::string>()> roots;
        // The console app: where ShadowMount+ runs a title from ("" when it
        // can't say), a rescan request, the folders it scans, a game's title ID
        // and contentVersion (false when unreadable), and seconds since the
        // app last asked this engine anything (since the engine started when never).
        std::function<std::string(const std::string& title_id)> smp_source;
        std::function<void()> smp_rescan;
        std::function<std::vector<std::string>()> scan_dirs;
        std::function<bool(const std::string& path, std::string* title_id, std::string* version)> game_info;
        std::function<double()> app_quiet;
    };
    explicit Updater(Options options);
    ~Updater();

    void check();               // in the background; nothing when busy
    void find(bool everywhere); // the usual places, or every drive too
    // Downloads, checks, replaces the chosen files (paths from the last
    // search) and, with `app`, the console app's image; starts the new engine
    // and stops this one.
    Status install(const std::vector<std::string>& paths, bool app = true);
    // Only the console app's image, from the latest release, which must be
    // this engine's version (an app older than its engine).
    Status install_app();
    // Finishes a staged app image once the app is quiet (called every few seconds).
    void tick();
    // Starts this same version again: a copy of its payload (its own update
    // download, or one a loader starts) goes to the ELF loader, and this
    // engine stops. Fails when no copy of this version is found.
    Status restart();

    JsonValue status() const;
    JsonValue summary() const; // for /api/info: {latest, newer, state, app_pending}

    struct AppImage {
        std::string path;        // the image or folder ShadowMount+ runs; "" when not found
        bool image = false;      // a file (else a folder, which is not replaced)
        std::string version;     // its contentVersion as X.Y.Z ("" unknown)
        std::string sha256;      // of the image
        std::string release_sha; // of the release's image ("" when it has none)
    };

private:
    void run(std::function<void()> work);
    Status do_install(const std::vector<Found>& chosen, const Release& release, bool app);
    // Where the app is and, with `hash`, its SHA-256 (images only).
    AppImage find_app(bool hash) const;
    // Downloads the release's app image next to `app` under a '.' name and
    // notes it as pending; then swaps at once when the app is quiet.
    Status stage_app(const AppImage& app, const Release& release, const std::string& sums);
    Status finish_app(); // the pending swap; nothing when none
    bool app_pending() const;
    void set_state(const std::string& state);

    Options o_;
    mutable std::mutex mutex_;
    std::string state_ = "idle"; // checking, searching, downloading, replacing, starting, restarting
    std::string error_;
    std::int64_t checked_ = 0;
    bool has_release_ = false;
    Release release_;
    std::vector<Found> found_;
    std::string searched_ = "no"; // "usual", "everywhere"
    AppImage app_;
    bool app_checked_ = false;
    std::mutex swap_mutex_; // one swap at a time (tick and an install)
    std::thread thread_;
    // Calls tick() every few seconds.
    std::thread watcher_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stop_ = false;
};

} // namespace alembic::engine::update

// SPDX-License-Identifier: GPL-3.0-or-later
#include "images.hpp"

#include "deps.hpp"
#include "smp.hpp"
#include "text_file.hpp"
#include "update.hpp"

#include "alembic/ampr_emu.hpp"
#include "alembic/io.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace alembic::engine::images {

namespace {

bool has_image_extension(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* ext : {".exfat", ".ffpfsc", ".ffpkg", ".ffpfs"}) {
        const std::size_t n = std::strlen(ext);
        if (lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0) return true;
    }
    return false;
}

bool is_game_folder(const std::string& path) {
    return is_regular_file(path + "/eboot.bin") || is_regular_file(path + "/sce_sys/param.json");
}

void scan_dir(const std::string& dir, int depth, std::vector<Found>& out, std::vector<std::string>& seen) {
    auto entries = list_directory_entries(dir);
    if (!entries.ok()) return;
    for (const DirEntry& e : *entries) {
        if (e.name.empty() || e.name[0] == '.' || e.name == "backports") continue;
        const std::string path = join_path(dir, e.name);
        if (std::find(seen.begin(), seen.end(), path) != seen.end()) continue;
        if (e.stat.kind == PathKind::directory) {
            if (is_game_folder(path)) {
                seen.push_back(path);
                out.push_back({path, ""});
            } else if (depth > 0) {
                scan_dir(path, depth - 1, out, seen);
            }
        } else if (has_image_extension(e.name)) {
            seen.push_back(path);
            out.push_back({path, ""});
        }
    }
}

// The SHA-256 of the libSceAmpr.sprx in a backport folder ShadowMount+
// named, and whether that backport has a fakelib (which then replaces the
// game's: no emulator in it means the game runs without one).
bool backport_emulator(const std::string& backport, std::string* sha) {
    for (const char* dir : {"fakelib2", "fakelib"}) {
        const std::string folder = backport + "/" + dir;
        if (!is_directory(folder)) continue;
        *sha = file_sha256(folder + "/" + ampr_emu::kFileName);
        return true;
    }
    return false;
}

JsonValue emulators_json(const EngineConfig& config) {
    JsonValue list = JsonValue::array();
    const Dependency* in_use = ampr_runtime_in_use(config.state_dir);
    for (const Dependency& d : dependencies()) {
        if (d.group != kAmprRuntimeGroup) continue;
        const JsonValue info = dependency_json(d, config.state_dir);
        JsonValue e = JsonValue::object();
        e.set("id", d.id);
        e.set("label", d.label);
        e.set("name", d.name);
        e.set("version", info["version"]);
        e.set("latest", info["latest"]);
        e.set("downloaded", is_regular_file(config.emulator_path(d.id)));
        e.set("in_use", in_use == &d);
        list.push(std::move(e));
    }
    return list;
}

} // namespace

std::vector<std::string> scan_dirs(const EngineConfig& config) {
    std::vector<std::string> dirs;
    if (!config.shadowmount_config.empty())
        if (auto text = read_text(config.shadowmount_config, 1u << 20); text.ok())
            for (std::size_t at = 0; at < text->size();) {
                std::size_t end = text->find('\n', at);
                if (end == std::string::npos) end = text->size();
                std::string line = text->substr(at, end - at);
                at = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.rfind("scanpath=", 0) == 0 && line.size() > 9) dirs.push_back(without_trailing_slashes(line.substr(9)));
            }
    if (!dirs.empty()) return dirs;
    if (config.shadowmount_config.empty()) return {config.state_dir + "/homebrew"};
    dirs = {"/data/homebrew", "/data/etaHEN/games", "/mnt/ext0/homebrew", "/mnt/ext0/etaHEN/games",
            "/mnt/ext1/homebrew", "/mnt/ext1/etaHEN/games"};
    for (int i = 0; i < 8; ++i) {
        dirs.push_back("/mnt/usb" + std::to_string(i) + "/homebrew");
        dirs.push_back("/mnt/usb" + std::to_string(i) + "/etaHEN/games");
    }
    for (int i = 0; i < 8; ++i) dirs.push_back("/mnt/usb" + std::to_string(i));
    dirs.push_back("/mnt/ext0");
    dirs.push_back("/mnt/ext1");
    return dirs;
}

std::vector<Found> scan(const std::vector<std::string>& dirs) {
    std::vector<Found> out;
    std::vector<std::string> seen;
    // Two levels: ShadowMount+ searches its scan folders recursively, and
    // games often sit in a folder of their own below one.
    for (const std::string& dir : dirs) scan_dir(dir, 1, out, seen);
    return out;
}

std::optional<JsonValue> evaluate(const Found& found, const EngineConfig& config, bool* unreadable) {
    if (unreadable) *unreadable = false;
    auto state = ampr_state(found.path, std::string(32, '0'));
    if (!state) {
        if (unreadable) *unreadable = true;
        return std::nullopt;
    }
    if (!state->imports) return std::nullopt;
    const bool folder = is_directory(found.path);
    // Which emulator it runs with: ShadowMount+'s backport when it named one,
    // else the one Alembic would find (backports_root_for), else the game's.
    std::string sha, where;
    if (!found.backport.empty() && backport_emulator(found.backport, &sha)) {
        where = sha.empty() ? "" : "backport";
    } else {
        sha = emulator_in_place(found.path, *state, config);
        where = sha.empty() ? "" : sha == state->ships_sha256 ? "game" : "backport";
    }
    const auto known = ampr_runtime_by_hash(sha, config.state_dir);
    // The emulator an Update or Add puts there: the game's own kind; for
    // none or an unknown build, the one in use (ampr-emu-ng for zstd packs,
    // which only it reads).
    std::string target;
    if (known) {
        target = known->dep->id;
    } else if (state->zstd_packs) {
        target = "ampr_runtime_ng";
    } else {
        const Dependency* in_use = ampr_runtime_in_use(config.state_dir);
        target = in_use ? in_use->id : "ampr_runtime";
    }
    std::string status;
    if (sha.empty()) {
        status = "missing";
    } else if (!known) {
        status = "unknown";
    } else {
        const std::string ours_path = config.emulator_path(target);
        const std::string ours = is_regular_file(ours_path) ? file_sha256(ours_path) : "";
        const auto ours_known = ampr_runtime_by_hash(ours, config.state_dir);
        if (ours.empty()) status = "not_downloaded";
        else if (ours == sha) status = "current";
        else if (ours_known && update::compare_versions(ours_known->version, known->version) > 0) status = "update";
        else status = "current"; // the game's build is as new as ours, or newer
    }
    JsonValue v = JsonValue::object();
    v.set("path", found.path);
    v.set("title_id", state->title_id);
    v.set("kind", folder ? "folder" : "image");
    v.set("where", where);
    if (known) {
        JsonValue e = JsonValue::object();
        e.set("id", known->dep->id);
        e.set("name", known->name());
        e.set("version", known->version);
        v.set("emulator", std::move(e));
    } else {
        v.set("emulator", JsonValue());
    }
    v.set("status", status);
    v.set("target", target);
    v.set("zstd_packs", state->zstd_packs);
    v.set("allowed", path_allowed(found.path, config.available_roots()));
    return v;
}

Index::~Index() {
    if (worker_.joinable()) worker_.join();
}

JsonValue Index::snapshot(std::int64_t max_age_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ && (max_age_s == 0 || unix_now() - updated_ > max_age_s)) {
        running_ = true;
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this] { refresh(); });
    }
    JsonValue v = JsonValue::object();
    v.set("images", images_);
    v.set("source", source_);
    v.set("refreshing", running_);
    v.set("updated", updated_);
    v.set("others", others_);
    v.set("unreadable", unreadable_);
    v.set("smp_update_emulators", smp_update_emulators_);
    v.set("emulators", emulators_json(config_));
    return v;
}

void Index::refresh() {
    std::vector<Found> found;
    std::string source = "scan";
    JsonValue update_emulators;
    const smp::Endpoint endpoint = shadowmount_endpoint(config_);
    if (endpoint.enabled) {
        if (auto reply = smp::post(endpoint, "/api/v1/games", JsonValue::object(), 8000); reply.ok()) {
            source = "shadowmount";
            for (const JsonValue& g : (*reply)["games"].items()) {
                const std::string type = g["source_type"].str();
                if (type != "folder" && type != "image") continue; // installed packages use the console's APR
                const std::string path = without_trailing_slashes(g["path"].str());
                if (path.empty() || !path_exists(path)) continue;
                found.push_back({path, g["backport_path"].str()});
            }
        }
        if (auto s = smp::post(endpoint, "/api/v1/settings", JsonValue::object()); s.ok())
            if ((*s)["update_emulators"].type() == JsonValue::Type::boolean) update_emulators = (*s)["update_emulators"];
    }
    if (source == "scan") found = scan(scan_dirs(config_));
    JsonValue list = JsonValue::array();
    std::int64_t others = 0, unreadable = 0;
    for (const Found& f : found) {
        bool bad = false;
        if (auto v = evaluate(f, config_, &bad)) list.push(std::move(*v));
        else if (bad) ++unreadable;
        else ++others;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    images_ = std::move(list);
    source_ = source;
    others_ = others;
    unreadable_ = unreadable;
    smp_update_emulators_ = update_emulators;
    updated_ = unix_now();
    running_ = false;
}

} // namespace alembic::engine::images

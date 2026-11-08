// SPDX-License-Identifier: GPL-3.0-or-later
#include "images.hpp"

#include "deps.hpp"
#include "smp.hpp"
#include "text_file.hpp"
#include "update.hpp"

#include "alembic/ampr_emu.hpp"
#include "alembic/io.hpp"

#include <algorithm>
#include <optional>
#include <map>
#include <atomic>
#include <cctype>
#include <cstring>

namespace alembic::engine::images {

namespace {

// The kind of a file Images lists, by extension; "" for any other file.
std::string kind_of_file(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* ext : {"exfat", "ffpfsc", "ffpkg", "ffpfs", "pkg"}) {
        const std::size_t n = std::strlen(ext) + 1;
        if (lower.size() > n && lower[lower.size() - n] == '.' && lower.compare(lower.size() - n + 1, n - 1, ext) == 0)
            return std::strcmp(ext, "pkg") == 0 ? "fpkg" : ext;
    }
    return "";
}

bool is_game_folder(const std::string& path) {
    return is_regular_file(path + "/eboot.bin") || is_regular_file(path + "/sce_sys/param.json");
}

void scan_dir(const std::string& dir, int depth, std::vector<Found>& out, std::vector<std::string>& seen) {
    auto entries = list_directory_entries(dir);
    if (!entries.ok()) return;
    for (const DirEntry& e : *entries) {
        if (e.name.empty() || e.name == "." || e.name == ".." || e.name == "backports") continue;
        const bool hidden = e.name[0] == '.';
        const std::string path = join_path(dir, e.name);
        if (std::find(seen.begin(), seen.end(), path) != seen.end()) continue;
        if (e.stat.kind == PathKind::directory) {
            if (is_game_folder(path)) {
                seen.push_back(path);
                out.push_back({path, "folder", dir, 0, e.stat.mtime, ""});
            } else if (depth > 0 && !hidden) { // a hidden folder is not searched (.Trashes, say)
                scan_dir(path, depth - 1, out, seen);
            }
        } else if (const std::string kind = kind_of_file(e.name); !kind.empty()) {
            seen.push_back(path);
            out.push_back({path, kind, dir, e.stat.size, e.stat.mtime, ""});
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

std::vector<std::string> scan_dirs(const EngineConfig& config, const std::vector<std::string>& custom) {
    std::vector<std::string> dirs;
    for (const std::string& d : custom)
        if (!d.empty()) dirs.push_back(without_trailing_slashes(d));
    if (!dirs.empty()) return dirs;
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

namespace {

// What a refresh works out once: the SHA-256 of each emulator downloaded here.
struct Context {
    const EngineConfig& config;
    std::map<std::string, std::string> ours; // component id -> SHA-256 ("" when not downloaded)
    explicit Context(const EngineConfig& c) : config(c) {
        for (const Dependency& d : dependencies()) {
            if (d.group != kAmprRuntimeGroup) continue;
            const std::string path = c.emulator_path(d.id);
            ours[d.id] = is_regular_file(path) ? file_sha256(path) : "";
        }
    }
};

JsonValue ampr_from_state(const Found& found, const std::optional<AmprState>& state, const Context& ctx) {
    const EngineConfig& config = ctx.config;
    JsonValue v = JsonValue::object();
    if (!state) {
        v.set("status", "unreadable");
        return v;
    }
    v.set("zstd_packs", state->zstd_packs);
    if (!state->imports) {
        v.set("status", "none");
        return v;
    }
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
        const auto it = ctx.ours.find(target);
        const std::string ours = it == ctx.ours.end() ? "" : it->second;
        const auto ours_known = ampr_runtime_by_hash(ours, config.state_dir);
        if (ours.empty()) status = "not_downloaded";
        else if (ours == sha) status = "current";
        else if (ours_known && update::compare_versions(ours_known->version, known->version) > 0) status = "update";
        else status = "current"; // the game's build is as new as ours, or newer
    }
    v.set("status", status);
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
    v.set("target", target);
    return v;
}

// What reading a game's eboot.bin gave, kept in <state>/images_cache.json
// by path, with what it was read from (size and time; for a folder its
// fakelib emulators too), so an unchanged game is not opened again.
std::string state_key(const Found& f) {
    std::string key = std::to_string(f.size) + "|" + std::to_string(f.mtime);
    if (f.kind == "folder")
        for (const char* dir : {"/fakelib/", "/fakelib2/"}) {
            const PathStat st = stat_path(f.path + dir + ampr_emu::kFileName);
            key += "|" + std::to_string(st.size) + "|" + std::to_string(st.mtime);
        }
    return key;
}

JsonValue state_json(const std::optional<AmprState>& a) {
    JsonValue v = JsonValue::object();
    v.set("ok", a.has_value());
    if (!a) return v;
    v.set("imports", a->imports);
    v.set("ships", a->ships);
    v.set("ships_sha256", a->ships_sha256);
    v.set("title_id", a->title_id);
    v.set("zstd", a->zstd_packs);
    v.set("lz4", a->lz4_packs);
    return v;
}

std::optional<AmprState> state_from(const JsonValue& v) {
    if (!v["ok"].boolean()) return std::nullopt;
    AmprState a;
    a.imports = v["imports"].boolean();
    a.ships = v["ships"].boolean();
    a.ships_sha256 = v["ships_sha256"].str();
    a.title_id = v["title_id"].str();
    a.zstd_packs = v["zstd"].boolean();
    a.lz4_packs = v["lz4"].boolean();
    return a;
}

std::string cache_file(const EngineConfig& config) { return config.state_dir + "/images_cache.json"; }

} // namespace

JsonValue ampr_of(const Found& found, const EngineConfig& config) {
    if (found.kind == "fpkg") return JsonValue(); // installed, it uses the console's APR
    return ampr_from_state(found, ampr_state(found.path, std::string(32, '0')), Context(config));
}

namespace {
// Files' fields and where it is, without the AMPR state.
JsonValue entry_json_base(const Found& found, const EngineConfig& config) {
    JsonValue v = JsonValue::object();
    const std::string name = base_name(found.path);
    v.set("name", name);
    v.set("path", found.path);
    v.set("kind", found.kind);
    if (found.kind == "folder") v.set("game", true);
    else v.set("size", found.size);
    v.set("mtime", found.mtime);
    v.set("folder", found.folder);
    v.set("hidden", !name.empty() && name[0] == '.');
    v.set("allowed", path_allowed(found.path, config.available_roots()));
    return v;
}
} // namespace

JsonValue entry_json(const Found& found, const EngineConfig& config) {
    JsonValue v = entry_json_base(found, config);
    v.set("ampr", ampr_of(found, config));
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
    JsonValue rows = JsonValue::array();
    for (const JsonValue& e : images_) rows.push(e);
    v.set("images", std::move(rows));
    v.set("dirs", dirs_);
    v.set("smp", smp_);
    v.set("refreshing", running_);
    v.set("updated", updated_);
    v.set("smp_update_emulators", smp_update_emulators_);
    v.set("emulators", emulators_json(config_));
    return v;
}

void Index::refresh() {
    std::vector<std::string> custom;
    std::vector<std::pair<std::string, std::string>> backports; // path -> backport, from ShadowMount+
    JsonValue update_emulators;
    bool smp = false;
    const smp::Endpoint endpoint = shadowmount_endpoint(config_);
    if (endpoint.enabled) {
        if (auto s = smp::post(endpoint, "/api/v1/settings", JsonValue::object()); s.ok()) {
            smp = true;
            if ((*s)["update_emulators"].type() == JsonValue::Type::boolean) update_emulators = (*s)["update_emulators"];
            for (const JsonValue& d : (*s)["scan_paths"].items()) custom.push_back(d.str());
        }
        if (auto reply = smp::post(endpoint, "/api/v1/games", JsonValue::object(), 8000); reply.ok()) {
            smp = true;
            for (const JsonValue& g : (*reply)["games"].items())
                if (!g["backport_path"].str().empty())
                    backports.emplace_back(without_trailing_slashes(g["path"].str()), g["backport_path"].str());
        }
    }
    const std::vector<std::string> dirs = scan_dirs(config_, custom);
    std::vector<Found> found = scan(dirs);
    for (Found& f : found)
        for (const auto& [path, backport] : backports)
            if (path == f.path) f.backport = backport;
    // First the list as the folders hold it, with what is known already:
    // shown at once, the rest filled in as each game is read.
    const Context ctx(config_);
    JsonValue cache = JsonValue::object();
    if (auto text = read_text(cache_file(config_), 16u << 20); text.ok())
        if (auto parsed = parse_json(*text); parsed.ok() && parsed->is_object()) cache = std::move(*parsed);
    JsonValue list = JsonValue::array();
    std::vector<std::size_t> todo;
    std::vector<std::string> keys(found.size());
    for (std::size_t i = 0; i < found.size(); ++i) {
        const Found& f = found[i];
        JsonValue e = entry_json_base(f, config_);
        if (f.kind == "fpkg") {
            e.set("ampr", JsonValue());
        } else {
            keys[i] = state_key(f);
            const JsonValue& c = cache[f.path];
            if (c.is_object() && c["key"].str() == keys[i]) {
                e.set("ampr", ampr_from_state(f, state_from(c), ctx));
            } else {
                JsonValue pending = JsonValue::object();
                pending.set("status", "checking");
                e.set("ampr", std::move(pending));
                todo.push_back(i);
            }
        }
        list.push(std::move(e));
    }
    JsonValue dirs_json = JsonValue::array();
    for (const std::string& d : dirs)
        if (is_directory(d)) dirs_json.push(d);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        images_ = list.items();
        dirs_ = std::move(dirs_json);
        smp_ = smp;
        smp_update_emulators_ = update_emulators;
    }
    // The games not read before, a few at a time (each opens an image).
    std::atomic<std::size_t> next{0};
    std::mutex cache_mutex;
    auto work = [&] {
        for (std::size_t n; (n = next++) < todo.size();) {
            const std::size_t i = todo[n];
            const std::optional<AmprState> state = ampr_state(found[i].path, std::string(32, '0'));
            JsonValue ampr = ampr_from_state(found[i], state, ctx);
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                JsonValue c = state_json(state);
                c.set("key", keys[i]);
                cache.set(found[i].path, std::move(c));
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (i < images_.size()) images_[i].set("ampr", std::move(ampr)); // only this refresh changes the rows
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < 3; ++t) pool.emplace_back(work);
    for (std::thread& t : pool) t.join();
    // Only the games still there are kept.
    JsonValue kept = JsonValue::object();
    for (const Found& f : found)
        if (cache[f.path].is_object()) kept.set(f.path, cache[f.path]);
    (void)write_text(cache_file(config_), kept.dump());
    std::lock_guard<std::mutex> lock(mutex_);
    updated_ = unix_now();
    running_ = false;
}

} // namespace alembic::engine::images

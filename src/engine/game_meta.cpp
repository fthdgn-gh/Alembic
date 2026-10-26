// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_meta.hpp"
#include "json_value.hpp"

#include "alembic/convert.hpp"
#include "alembic/io.hpp"
#include "alembic/sfo.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>

namespace alembic::engine {

namespace {

constexpr std::size_t kMaxIcon = 4u * 1024 * 1024;
constexpr std::size_t kMaxParam = 1u * 1024 * 1024;

// At most two inputs are opened at once: a page full of large packages must
// not starve a running conversion.
std::mutex g_slots_mutex;
std::condition_variable g_slots_cv;
int g_slots = 2;

struct Slot {
    Slot() {
        std::unique_lock<std::mutex> lock(g_slots_mutex);
        g_slots_cv.wait(lock, [] { return g_slots > 0; });
        --g_slots;
    }
    ~Slot() {
        {
            std::lock_guard<std::mutex> lock(g_slots_mutex);
            ++g_slots;
        }
        g_slots_cv.notify_one();
    }
};

// One lock per key, so two requests for the same file read it once.
std::mutex g_memory_mutex;
std::map<std::string, GameMeta> g_memory;
std::map<std::string, std::shared_ptr<std::mutex>> g_key_locks;

std::uint64_t fnv1a64(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

Result<std::vector<std::uint8_t>> read_node(const tree::Source& source, const tree::Node& node, std::size_t max) {
    if (node.size > max) return make_error(Errc::too_large, "too large");
    ALEMBIC_ASSIGN(auto file, source.open(node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node.size));
    ALEMBIC_TRY(file->read_at(0, data));
    return data;
}

const tree::Node* sce_sys_file(const tree::Node& root, const char* name) {
    const tree::Node* sys = tree::find_child_icase(root, "sce_sys");
    if (!sys || !sys->is_directory) return nullptr;
    const tree::Node* f = tree::find_child_icase(*sys, name);
    return f && !f->is_directory ? f : nullptr;
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path) {
    ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
    ALEMBIC_ASSIGN(std::uint64_t size, file.size());
    if (size > kMaxIcon) return make_error(Errc::too_large, "too large");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    ALEMBIC_TRY(file.read_at(0, data));
    return data;
}

Status write_file(const std::string& path, Bytes data) {
    ALEMBIC_TRY(make_parent_directories(path));
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, true));
    ALEMBIC_TRY(out.file().write_at(0, data));
    return out.commit();
}

JsonValue to_json(const GameMeta& m) {
    JsonValue v = JsonValue::object();
    v.set("title", m.title);
    v.set("title_id", m.title_id);
    v.set("content_id", m.content_id);
    v.set("version", m.version);
    v.set("icon", m.has_icon);
    return v;
}

GameMeta from_json(const JsonValue& v) {
    GameMeta m;
    m.title = v["title"].str();
    m.title_id = v["title_id"].str();
    m.content_id = v["content_id"].str();
    m.version = v["version"].str();
    m.has_icon = v["icon"].boolean();
    return m;
}

GameMeta parse_param(const std::string& text) {
    GameMeta m;
    auto json = parse_json(text);
    if (!json.ok()) return m;
    m.title_id = (*json)["titleId"].str();
    m.content_id = (*json)["contentId"].str();
    m.version = (*json)["contentVersion"].str();
    const JsonValue& lp = (*json)["localizedParameters"];
    std::string lang = lp["defaultLanguage"].str();
    for (const std::string& l : {std::string("en-US"), std::string("en-GB"), lang}) {
        std::string t = lp[l]["titleName"].str();
        if (!t.empty()) {
            m.title = t;
            break;
        }
    }
    if (m.title.empty())
        for (const auto& [k, v] : lp.fields())
            if (!v["titleName"].str().empty()) {
                m.title = v["titleName"].str();
                break;
            }
    return m;
}

// A PS4 game's param.sfo.
GameMeta parse_sfo(const std::vector<std::uint8_t>& data) {
    GameMeta m;
    auto param = sfo::parse(data);
    if (!param.ok()) return m;
    m.title = param->get("TITLE").value_or("");
    m.title_id = param->get("TITLE_ID").value_or("");
    m.content_id = param->get("CONTENT_ID").value_or("");
    m.version = param->get("APP_VER").value_or(param->get("VERSION").value_or(""));
    return m;
}

} // namespace

GameMetaCache::GameMetaCache(std::string state_dir) : dir_(std::move(state_dir) + "/cache/meta") {}

Result<std::string> GameMetaCache::key_for(const std::string& path) const {
    PathStat st = stat_path(path);
    // A file a browser shares: its path already names its size and time.
    if (st.kind == PathKind::missing && !convert::is_provided(path))
        return make_error(Errc::invalid_argument, "no such file or folder: " + path);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx",
                  static_cast<unsigned long long>(fnv1a64(path + "|" + std::to_string(st.size) + "|" +
                                                          std::to_string(static_cast<long long>(st.mtime)))));
    return std::string(buf);
}

Status GameMetaCache::load(const std::string& path, const std::string& key) {
    std::shared_ptr<std::mutex> key_lock;
    {
        std::lock_guard<std::mutex> lock(g_memory_mutex);
        if (g_memory.count(key)) return {};
        auto& l = g_key_locks[key];
        if (!l) l = std::make_shared<std::mutex>();
        key_lock = l;
    }
    std::lock_guard<std::mutex> once(*key_lock);
    {
        std::lock_guard<std::mutex> lock(g_memory_mutex);
        if (g_memory.count(key)) return {};
    }
    // On disk from an earlier run?
    std::string json_path = dir_ + "/" + key + ".json";
    if (auto data = read_file(json_path); data.ok()) {
        auto v = parse_json(std::string(data->begin(), data->end()));
        if (v.ok()) {
            (void)touch_file(json_path); // used now (once per run: it stays in memory)
            std::lock_guard<std::mutex> lock(g_memory_mutex);
            g_memory[key] = from_json(*v);
            return {};
        }
    }
    GameMeta meta;
    std::vector<std::uint8_t> icon;
    {
        Slot slot;
        ALEMBIC_ASSIGN(convert::Input input, convert::open(path));
        const tree::Node& root = input.tree->root();
        if (const tree::Node* p = sce_sys_file(root, "param.json")) {
            ALEMBIC_ASSIGN(auto data, read_node(*input.tree, *p, kMaxParam));
            meta = parse_param(std::string(data.begin(), data.end()));
        } else if (const tree::Node* s = sce_sys_file(root, "param.sfo")) {
            ALEMBIC_ASSIGN(auto data, read_node(*input.tree, *s, kMaxParam));
            meta = parse_sfo(data);
        }
        if (const tree::Node* i = sce_sys_file(root, "icon0.png")) {
            auto data = read_node(*input.tree, *i, kMaxIcon);
            if (data.ok() && data->size() > 8) {
                icon = std::move(*data);
                meta.has_icon = true;
            }
        }
    }
    if (meta.has_icon) {
        Status s = write_file(dir_ + "/" + key + ".png", icon);
        if (!s.ok()) std::fprintf(stderr, "icon cache: %s\n", s.error().message.c_str());
    }
    std::string text = to_json(meta).dump();
    (void)write_file(json_path, Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    std::lock_guard<std::mutex> lock(g_memory_mutex);
    if (g_memory.size() > 2048) g_memory.clear();
    g_memory[key] = meta;
    return {};
}

Result<GameMeta> GameMetaCache::meta(const std::string& path) {
    ALEMBIC_ASSIGN(std::string key, key_for(path));
    ALEMBIC_TRY(load(path, key));
    std::lock_guard<std::mutex> lock(g_memory_mutex);
    return g_memory[key];
}

Result<std::vector<std::uint8_t>> GameMetaCache::icon(const std::string& path) {
    ALEMBIC_ASSIGN(GameMeta m, meta(path));
    if (!m.has_icon) return make_error(Errc::invalid_argument, "no icon");
    ALEMBIC_ASSIGN(std::string key, key_for(path));
    std::string cached = dir_ + "/" + key + ".png";
    if (auto png = read_file(cached); png.ok()) return png;
    // The cache file could not be written (or was removed): read the icon
    // from the input again and try to cache it once more.
    Slot slot;
    ALEMBIC_ASSIGN(convert::Input input, convert::open(path));
    const tree::Node* i = sce_sys_file(input.tree->root(), "icon0.png");
    if (!i) return make_error(Errc::invalid_argument, "no icon");
    ALEMBIC_ASSIGN(auto data, read_node(*input.tree, *i, kMaxIcon));
    (void)write_file(cached, data);
    return data;
}

namespace {

// The cache folder's entries: key -> {last use, bytes of its .json and .png}.
struct Entry {
    std::time_t used = 0;
    std::uint64_t bytes = 0;
    bool has_json = false;
};

std::map<std::string, Entry> scan(const std::string& dir, std::uint64_t* other_bytes) {
    std::map<std::string, Entry> out;
    auto names = list_directory(dir);
    if (!names.ok()) return out;
    for (const std::string& name : *names) {
        PathStat st = stat_path(dir + "/" + name);
        if (!st.is_file()) continue;
        std::size_t dot = name.rfind('.');
        std::string ext = dot == std::string::npos ? "" : name.substr(dot);
        if (ext != ".json" && ext != ".png") {
            if (other_bytes) *other_bytes += st.size;
            continue;
        }
        Entry& entry = out[name.substr(0, dot)];
        entry.bytes += st.size;
        if (ext == ".json") {
            entry.has_json = true;
            entry.used = st.mtime;
        } else if (!entry.has_json) {
            entry.used = std::max<std::int64_t>(entry.used, st.mtime);
        }
    }
    return out;
}

} // namespace

GameMetaCache::Stats GameMetaCache::stats() const {
    Stats s;
    for (const auto& [key, e] : scan(dir_, &s.bytes)) {
        s.bytes += e.bytes;
        if (e.has_json) ++s.games;
    }
    return s;
}

std::uint64_t GameMetaCache::prune(std::uint64_t max_age_days, std::uint64_t max_bytes) {
    auto entries = scan(dir_, nullptr);
    std::time_t oldest = std::time(nullptr) - static_cast<std::time_t>(max_age_days * 86400);
    std::uint64_t total = 0, dropped = 0;
    std::vector<std::pair<std::time_t, std::string>> by_use;
    for (const auto& [key, e] : entries) {
        total += e.bytes;
        by_use.push_back({e.used, key});
    }
    std::sort(by_use.begin(), by_use.end());
    auto drop = [&](const std::string& key) {
        (void)remove_file((dir_ + "/" + key + ".json"));
        (void)remove_file((dir_ + "/" + key + ".png"));
        std::lock_guard<std::mutex> lock(g_memory_mutex);
        g_memory.erase(key);
    };
    for (const auto& [used, key] : by_use) {
        const Entry& e = entries[key];
        // An icon without its .json is left over from an interrupted write.
        if (used >= oldest && total <= max_bytes && e.has_json) continue;
        drop(key);
        total -= e.bytes;
        ++dropped;
    }
    return dropped;
}

Status GameMetaCache::clear() {
    for (const auto& [key, e] : scan(dir_, nullptr)) {
        (void)e;
        (void)remove_file((dir_ + "/" + key + ".json"));
        (void)remove_file((dir_ + "/" + key + ".png"));
    }
    std::lock_guard<std::mutex> lock(g_memory_mutex);
    g_memory.clear();
    return {};
}

} // namespace alembic::engine

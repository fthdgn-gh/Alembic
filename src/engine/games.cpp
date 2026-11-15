// SPDX-License-Identifier: GPL-3.0-or-later
#include "games.hpp"

#include "game_meta.hpp"

#include "alembic/io.hpp"

#include <algorithm>

namespace alembic::engine::games {

namespace {

constexpr std::string_view kApp0 = "-app0";

std::string read_text(const std::string& path) {
    auto f = File::open(path, File::Mode::read);
    if (!f.ok()) return {};
    auto size = f->size();
    if (!size.ok() || *size > (1u << 20)) return {};
    std::string text(static_cast<std::size_t>(*size), '\0');
    if (!f->read_at(0, MutableBytes(reinterpret_cast<std::uint8_t*>(text.data()), text.size())).ok()) return {};
    return text;
}

} // namespace

bool valid_title_id(std::string_view id) {
    if (id.size() != 9) return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (id[i] < 'A' || id[i] > 'Z') return false;
    for (std::size_t i = 4; i < 9; ++i)
        if (id[i] < '0' || id[i] > '9') return false;
    return true;
}

std::vector<std::string> mounted(const Paths& paths) {
    std::vector<std::string> ids;
    auto names = list_directory(paths.pfsmnt);
    if (!names.ok()) return ids;
    for (const std::string& n : *names) {
        if (n.size() != 9 + kApp0.size() || n.compare(9, kApp0.size(), kApp0) != 0) continue;
        std::string id = n.substr(0, 9);
        if (valid_title_id(id)) ids.push_back(std::move(id));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string source_tree(const Paths& paths, const std::string& id, bool* with_update) {
    const bool update = is_directory(paths.user_patch + "/" + id);
    if (with_update) *with_update = update;
    return paths.pfsmnt + "/" + id + (update ? "-app0-patch0-union" : "-app0");
}

Info info(const Paths& paths, const std::string& id) {
    Info i;
    i.title_id = id;
    if (!valid_title_id(id)) return i;
    const std::string dir = paths.appmeta + "/" + id;
    GameMeta m = parse_param_json(read_text(dir + "/param.json"));
    i.title = m.title;
    i.version = m.version;
    i.content_id = m.content_id;
    if (is_regular_file(dir + "/icon0.png")) i.icon = dir + "/icon0.png";
    return i;
}

Watcher::Watcher(Paths paths, std::int64_t settle_seconds) : paths_(std::move(paths)), settle_(settle_seconds) {}

bool Watcher::poll(std::int64_t now) {
    const std::vector<std::string> ids = mounted(paths_);
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    std::map<std::string, std::int64_t> since;
    std::map<std::string, bool> settled;
    for (const std::string& id : ids) {
        auto it = since_.find(id);
        const std::int64_t first = it == since_.end() ? now : it->second;
        if (it == since_.end()) changed = true;
        since[id] = first;
        const bool s = now - first >= settle_;
        if (s != (settled_.count(id) && settled_[id])) changed = true;
        settled[id] = s;
    }
    if (since.size() != since_.size()) changed = true; // a game closed
    since_ = std::move(since);
    settled_ = std::move(settled);
    return changed;
}

bool Watcher::running(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return since_.count(id) != 0;
}

bool Watcher::settled(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = settled_.find(id);
    return it != settled_.end() && it->second;
}

std::vector<std::string> Watcher::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : since_) ids.push_back(id);
    return ids;
}

void Watcher::forget(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    since_.erase(id);
    settled_.erase(id);
}

std::optional<int> ProgressSteps::update(std::uint64_t job, const std::string& stage, std::uint64_t done,
                                         std::uint64_t total, std::int64_t now) {
    if (total == 0) return std::nullopt;
    State& s = jobs_[job];
    if (s.stage != stage) s = State{stage, 0, now}; // the first one 15 s in, not at once
    const int step = static_cast<int>(std::min<std::uint64_t>(100, done * 100 / total));
    if (step <= s.shown) return std::nullopt;
    if (now - s.at < min_gap_ && step < 100) return std::nullopt;
    s.shown = step;
    s.at = now;
    return step;
}

void ProgressSteps::forget(std::uint64_t job) { jobs_.erase(job); }

} // namespace alembic::engine::games

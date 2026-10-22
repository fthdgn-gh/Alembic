// SPDX-License-Identifier: GPL-3.0-or-later
#include "places.hpp"

#include "json_value.hpp"
#include "text_file.hpp"

#include "alembic/io.hpp"

#include <algorithm>

namespace alembic::engine {

namespace {
constexpr std::size_t kMaxPlaces = 64;
constexpr std::size_t kMaxLabel = 80;
} // namespace

Places::Places(std::string state_dir) : file_(std::move(state_dir) + "/places.json") {
    auto text = read_text(file_, 1u << 20);
    if (!text.ok()) return;
    auto v = parse_json(*text);
    if (!v.ok()) return;
    for (const JsonValue& item : (*v)["places"].items()) {
        Place p{item["path"].str(), item["label"].str()};
        if (!p.path.empty() && places_.size() < kMaxPlaces) places_.push_back(std::move(p));
    }
}

std::vector<Place> Places::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return places_;
}

bool Places::has(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(places_.begin(), places_.end(), [&](const Place& p) { return p.path == path; });
}

Status Places::add(const std::string& path, const std::string& label) {
    std::string name = label.empty() ? base_name(path) : label.substr(0, kMaxLabel);
    std::lock_guard<std::mutex> lock(mutex_);
    for (Place& p : places_)
        if (p.path == path) {
            p.label = name;
            return save_locked();
        }
    if (places_.size() >= kMaxPlaces) return make_error(Errc::too_large, "Places holds at most 64 folders");
    places_.push_back({path, name});
    return save_locked();
}

Status Places::remove(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(places_.begin(), places_.end(), [&](const Place& p) { return p.path == path; });
    if (it == places_.end()) return make_error(Errc::invalid_argument, "not in Places: " + path);
    places_.erase(it, places_.end());
    return save_locked();
}

Status Places::save_locked() const {
    JsonValue list = JsonValue::array();
    for (const Place& p : places_) {
        JsonValue v = JsonValue::object();
        v.set("path", p.path);
        v.set("label", p.label);
        list.push(std::move(v));
    }
    JsonValue root = JsonValue::object();
    root.set("places", std::move(list));
    (void)make_parent_directories(file_);
    return write_text(file_, root.dump());
}

} // namespace alembic::engine

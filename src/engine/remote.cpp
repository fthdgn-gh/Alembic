// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>

namespace alembic::engine {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kAhead = 16;    // at most this many pieces asked for ahead of a forward read
constexpr std::size_t kKeep = 128;      // pieces kept (128 MiB)
constexpr std::size_t kPerAnswer = 4;     // pieces handed out per want
constexpr auto kResend = std::chrono::seconds(20); // a piece asked for but not sent by then is asked for again
constexpr auto kGone = std::chrono::seconds(45);   // no want from the page for this long: give up

std::string make_id(const std::string& name, std::uint64_t size, std::int64_t last_modified) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ULL;
    };
    mix(name.data(), name.size());
    mix(&size, sizeof size);
    mix(&last_modified, sizeof last_modified);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// "/remote/<id>/<name>" -> id
std::string id_of(const std::string& path) {
    const std::string prefix = RemoteFiles::kPrefix;
    if (path.compare(0, prefix.size(), prefix) != 0) return "";
    std::size_t slash = path.find('/', prefix.size());
    return slash == std::string::npos ? "" : path.substr(prefix.size(), slash - prefix.size());
}

} // namespace

struct RemoteFiles::File final : RandomAccess {
    Info info;
    std::string id;
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    mutable std::map<std::uint64_t, std::shared_ptr<const std::vector<std::uint8_t>>> pieces;
    mutable std::deque<std::uint64_t> order; // pieces by last use, oldest first
    mutable std::set<std::uint64_t> wanted;  // to hand out
    mutable std::map<std::uint64_t, Clock::time_point> sent; // handed out, not back yet
    mutable Clock::time_point last_poll = Clock::now();
    mutable std::uint64_t last_piece = ~std::uint64_t{0};
    mutable unsigned streak = 0; // forward reads in a row

    std::uint64_t piece_count() const { return (info.size + kPiece - 1) / kPiece; }
    std::uint64_t piece_length(std::uint64_t k) const { return std::min(kPiece, info.size - k * kPiece); }

    Result<std::uint64_t> size() const override { return info.size; }

    void ask(std::uint64_t k) const {
        if (k < piece_count() && !pieces.count(k) && !sent.count(k)) wanted.insert(k);
    }

    void touch(std::uint64_t k) const {
        order.erase(std::remove(order.begin(), order.end(), k), order.end());
        order.push_back(k);
        while (order.size() > kKeep) {
            pieces.erase(order.front());
            order.pop_front();
        }
    }

    Result<std::shared_ptr<const std::vector<std::uint8_t>>> piece(std::uint64_t k) const {
        std::unique_lock<std::mutex> lock(mutex);
        // Reading forward: ask for the next pieces too, more the longer it
        // goes on (2, 4, 8, 16). A jump drops what was not handed out yet:
        // readers go file by file, and the bytes after a file are often
        // another file, read much later or not at all.
        bool forward = last_piece != ~std::uint64_t{0} && k >= last_piece && k <= last_piece + 1;
        if (forward && k == last_piece + 1) ++streak;
        if (!forward) {
            streak = 0;
            wanted.clear();
        }
        last_piece = k;
        ask(k);
        if (streak > 0) {
            std::uint64_t ahead = std::min<std::uint64_t>(kAhead, 1ull << std::min(streak, 4u));
            for (std::uint64_t a = k + 1; a <= k + ahead; ++a) ask(a);
        }
        if (!wanted.empty()) changed.notify_all();
        for (;;) {
            if (auto it = pieces.find(k); it != pieces.end()) {
                auto p = it->second;
                touch(k);
                return p;
            }
            auto now = Clock::now();
            if (now - last_poll > kGone)
                return make_error(Errc::io, "the computer sharing " + info.name +
                                                " stopped answering (keep Alembic open in its browser)");
            for (auto it = sent.begin(); it != sent.end();) {
                if (now - it->second > kResend) {
                    wanted.insert(it->first);
                    it = sent.erase(it);
                } else {
                    ++it;
                }
            }
            if (!sent.count(k)) wanted.insert(k);
            changed.wait_for(lock, std::chrono::seconds(1));
        }
    }

    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > info.size || out.size() > info.size - offset)
            return make_error(Errc::invalid_argument, "read past the end of " + info.name);
        std::size_t done = 0;
        while (done < out.size()) {
            std::uint64_t at = offset + done, k = at / kPiece;
            ALEMBIC_ASSIGN(auto p, piece(k));
            std::size_t within = static_cast<std::size_t>(at - k * kPiece);
            std::size_t n = std::min<std::size_t>(out.size() - done, p->size() - within);
            std::memcpy(out.data() + done, p->data() + within, n);
            done += n;
        }
        return {};
    }
};

bool RemoteFiles::is_remote(const std::string& path) { return !id_of(path).empty(); }

Result<std::string> RemoteFiles::add(const std::string& name, std::uint64_t size, std::int64_t last_modified) {
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..")
        return make_error(Errc::invalid_argument, "a shared file needs a plain name");
    if (size == 0) return make_error(Errc::invalid_argument, "the shared file is empty");
    std::string id = make_id(name, size, last_modified);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& f = files_[id];
    if (!f) {
        f = std::make_shared<File>();
        f->info = {name, size, last_modified};
        f->id = id;
    }
    {
        std::lock_guard<std::mutex> l(f->mutex);
        f->last_poll = Clock::now();
    }
    return std::string(kPrefix) + id + "/" + name;
}

std::shared_ptr<const RandomAccess> RemoteFiles::open(const std::string& path) const {
    std::string id = id_of(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(id);
    if (it == files_.end()) return nullptr;
    if (path != std::string(kPrefix) + id + "/" + it->second->info.name) return nullptr;
    return it->second;
}

std::optional<RemoteFiles::Info> RemoteFiles::info(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(id_of(path));
    if (it == files_.end()) return std::nullopt;
    return it->second->info;
}

JsonValue RemoteFiles::want(const std::string& id, int wait_ms) {
    std::shared_ptr<File> f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(id);
        if (it != files_.end()) f = it->second;
    }
    JsonValue v = JsonValue::object();
    JsonValue pieces = JsonValue::array();
    if (!f) {
        v.set("known", false);
        v.set("pieces", std::move(pieces));
        return v;
    }
    std::unique_lock<std::mutex> lock(f->mutex);
    f->last_poll = Clock::now();
    f->changed.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return !f->wanted.empty(); });
    f->last_poll = Clock::now();
    for (std::size_t n = 0; n < kPerAnswer && !f->wanted.empty(); ++n) {
        std::uint64_t k = *f->wanted.begin();
        f->wanted.erase(f->wanted.begin());
        f->sent[k] = Clock::now();
        JsonValue p = JsonValue::object();
        p.set("offset", k * kPiece);
        p.set("length", f->piece_length(k));
        pieces.push(std::move(p));
    }
    v.set("known", true);
    v.set("pieces", std::move(pieces));
    return v;
}

Status RemoteFiles::data(const std::string& id, std::uint64_t offset, const std::string& bytes) {
    std::shared_ptr<File> f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(id);
        if (it != files_.end()) f = it->second;
    }
    if (!f) return make_error(Errc::invalid_argument, "no such shared file");
    if (offset % kPiece != 0 || offset >= f->info.size) return make_error(Errc::invalid_argument, "bad piece offset");
    std::uint64_t k = offset / kPiece;
    if (bytes.size() != f->piece_length(k)) return make_error(Errc::invalid_argument, "bad piece length");
    auto data = std::make_shared<std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    std::lock_guard<std::mutex> lock(f->mutex);
    f->last_poll = Clock::now();
    f->sent.erase(k);
    f->wanted.erase(k);
    f->pieces[k] = std::move(data);
    f->touch(k);
    f->changed.notify_all();
    return {};
}

JsonValue RemoteFiles::list() const {
    JsonValue out = JsonValue::array();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, f] : files_) {
        std::lock_guard<std::mutex> l(f->mutex);
        JsonValue v = JsonValue::object();
        v.set("id", id);
        v.set("path", std::string(kPrefix) + id + "/" + f->info.name);
        v.set("name", f->info.name);
        v.set("size", f->info.size);
        v.set("waiting", !f->wanted.empty() || !f->sent.empty());
        out.push(std::move(v));
    }
    return out;
}

} // namespace alembic::engine

// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapshot.hpp"

#include "alembic/io.hpp"
#include "alembic/sha256.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace alembic::engine::snapshot {
namespace {

// A folder holding this name is a snapshot: never listed (the output may sit
// inside a root, /data being /user/data on the console).
constexpr const char* kMarker = ".alembic-snapshot";

struct Progress {
    bool running = false;
    std::string out;
    std::uint64_t entries = 0;
    std::uint64_t hashed = 0;
    std::uint64_t copied = 0;
    std::uint64_t errors = 0;
    double seconds = 0;
    std::string error;
    std::string current;
};

std::mutex g_mutex;
Progress g_progress;

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '%' || c == '\t' || c == '\n' || c == '\r') {
            static const char* hex = "0123456789ABCDEF";
            out += '%';
            out += hex[(static_cast<unsigned char>(c) >> 4) & 15];
            out += hex[static_cast<unsigned char>(c) & 15];
        } else {
            out += c;
        }
    }
    return out;
}

const char* kind_name(PathKind k) {
    switch (k) {
    case PathKind::file: return "file";
    case PathKind::directory: return "dir";
    case PathKind::link: return "link";
    case PathKind::other: return "other";
    case PathKind::missing: break;
    }
    return "missing";
}

class Run {
public:
    explicit Run(const Options& o) : o_(o) {}

    JsonValue go() {
        auto t0 = std::chrono::steady_clock::now();
        JsonValue summary = JsonValue::object();
        std::string err = setup();
        if (err.empty()) {
            for (const auto& root : o_.roots) walk(without_trailing_slashes(root), 0);
            for (const auto& path : o_.copy) copy_tree(without_trailing_slashes(path), 0);
            if (auto s = listing_->commit(); !s.ok()) err = "listing.tsv: " + s.error().message;
        }
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        summary.set("out", o_.out);
        JsonValue roots = JsonValue::array(), copy = JsonValue::array();
        for (const auto& r : o_.roots) roots.push(r);
        for (const auto& c : o_.copy) copy.push(c);
        summary.set("roots", std::move(roots));
        summary.set("copy", std::move(copy));
        summary.set("match", o_.match);
        summary.set("hash_max", o_.hash_max);
        summary.set("copy_max", o_.copy_max);
        summary.set("entries", entries_);
        summary.set("files_hashed", hashed_);
        summary.set("copied", copied_);
        summary.set("errors", errors_);
        JsonValue first = JsonValue::array();
        for (const auto& e : first_errors_) first.push(e);
        summary.set("first_errors", std::move(first));
        summary.set("seconds", seconds_);
        summary.set("time", static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count()));
        if (!err.empty()) summary.set("error", err);
        if (opened_) {
            if (auto out = AtomicOutput::create(join_path(o_.out, "summary.json"), true); out.ok()) {
                std::string text = summary.dump() + "\n";
                if (out->file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())).ok())
                    (void)out->commit();
            }
        }
        report(err);
        return summary;
    }

private:
    std::string setup() {
        if (o_.out.empty() || !is_absolute_path(o_.out)) return "out must be an absolute path";
        if (stat_path(o_.out, false).kind != PathKind::missing) return o_.out + " exists already";
        if (auto s = make_parent_directories(join_path(o_.out, "x")); !s.ok()) return s.error().message;
        auto marker = AtomicOutput::create(join_path(o_.out, kMarker));
        if (!marker.ok()) return marker.error().message;
        if (auto s = marker->commit(); !s.ok()) return s.error().message;
        opened_ = true;
        auto listing = AtomicOutput::create(join_path(o_.out, "listing.tsv"));
        if (!listing.ok()) return listing.error().message;
        listing_.emplace(std::move(*listing));
        line("# kind\tsize\tmtime\tsha256\tpath\tmode\tuid\tgid\tlink target");
        return {};
    }

    void line(const std::string& text) {
        std::string l = text + "\n";
        if (listing_ && listing_->file().write_at(pos_, Bytes(reinterpret_cast<const std::uint8_t*>(l.data()), l.size())).ok())
            pos_ += l.size();
    }

    void fail(const std::string& path, const std::string& message) {
        ++errors_;
        line("error\t-\t-\t-\t" + escape(path) + "\t" + escape(message));
        if (first_errors_.size() < 200) first_errors_.push_back(path + ": " + message);
    }

    void tick(const std::string& path) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_progress.entries = entries_;
        g_progress.hashed = hashed_;
        g_progress.copied = copied_;
        g_progress.errors = errors_;
        g_progress.current = path;
    }

    // SHA-256 of a whole file, or "" (and an error line) when unreadable.
    std::string hash(const std::string& path) {
        auto f = File::open(path, File::Mode::read);
        if (!f.ok()) {
            fail(path, f.error().message);
            return {};
        }
        auto size = f->size();
        if (!size.ok()) {
            fail(path, size.error().message);
            return {};
        }
        Sha256 h;
        buffer_.resize(1u << 20);
        for (std::uint64_t at = 0; at < *size;) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), *size - at));
            if (auto s = f->read_at(at, MutableBytes(buffer_.data(), n)); !s.ok()) {
                fail(path, s.error().message);
                return {};
            }
            h.update(Bytes(buffer_.data(), n));
            at += n;
        }
        ++hashed_;
        return Sha256::hex(h.finish());
    }

    void walk(const std::string& dir, int depth) {
        auto entries = list_directory_entries(dir, false);
        if (!entries.ok()) {
            fail(dir, entries.error().message);
            return;
        }
        for (const auto& e : *entries)
            if (e.name == kMarker) return; // a snapshot's own folder
        for (const auto& e : *entries) {
            std::string path = join_path(dir, e.name);
            ++entries_;
            if ((entries_ & 255) == 0) tick(path);
            std::string digest = "-";
            bool matched = !o_.match.empty() && path.find(o_.match) != std::string::npos;
            if (e.stat.is_file() && (e.stat.size <= o_.hash_max || matched)) { // the title's own files: always
                std::string d = hash(path);
                if (!d.empty()) digest = d;
            }
            char mode[16];
            std::snprintf(mode, sizeof(mode), "%04o", static_cast<unsigned>(e.stat.mode));
            std::string target;
            if (e.stat.kind == PathKind::link) {
                auto t = link_target(path);
                target = t.ok() ? escape(*t) : std::string("?");
            }
            line(std::string(kind_name(e.stat.kind)) + "\t" + std::to_string(e.stat.size) + "\t" +
                 std::to_string(e.stat.mtime) + "\t" + digest + "\t" + escape(path) + "\t" + mode + "\t" +
                 std::to_string(e.stat.uid) + "\t" + std::to_string(e.stat.gid) + "\t" + target);
            if (e.stat.is_file() && matched) copy_file(path, e.stat.size);
            if (e.stat.is_directory()) {
                if (depth + 1 >= o_.max_depth) fail(path, "deeper than max_depth, not listed");
                else walk(path, depth + 1);
            }
        }
    }

    void copy_tree(const std::string& path, int depth) {
        PathStat st = stat_path(path, false);
        if (st.is_file()) {
            copy_file(path, st.size);
            return;
        }
        if (!st.is_directory()) {
            if (st.kind == PathKind::missing) fail(path, "not found (copy)");
            return;
        }
        if (depth >= o_.max_depth) return;
        auto entries = list_directory_entries(path, false);
        if (!entries.ok()) {
            fail(path, entries.error().message);
            return;
        }
        for (const auto& e : *entries) copy_tree(join_path(path, e.name), depth + 1);
    }

    void copy_file(const std::string& path, std::uint64_t size) {
        std::string dest = o_.out + "/files" + (path.empty() || path[0] != '/' ? "/" : "") + path;
        if (stat_path(dest, false).kind != PathKind::missing) return; // copied already (match and copy)
        if (size > o_.copy_max) {
            fail(path, "larger than copy_max, not copied");
            return;
        }
        auto in = File::open(path, File::Mode::read);
        if (!in.ok()) {
            fail(path, in.error().message);
            return;
        }
        auto out = AtomicOutput::create(dest);
        if (!out.ok()) {
            fail(dest, out.error().message);
            return;
        }
        buffer_.resize(1u << 20);
        auto total = in->size();
        if (!total.ok()) {
            fail(path, total.error().message);
            return;
        }
        for (std::uint64_t at = 0; at < *total;) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), *total - at));
            if (auto s = in->read_at(at, MutableBytes(buffer_.data(), n)); !s.ok()) {
                fail(path, s.error().message);
                return;
            }
            if (auto s = out->file().write_at(at, Bytes(buffer_.data(), n)); !s.ok()) {
                fail(dest, s.error().message);
                return;
            }
            at += n;
        }
        if (auto s = out->commit(); !s.ok()) {
            fail(dest, s.error().message);
            return;
        }
        PathStat st = stat_path(path);
        (void)set_file_times(dest, st.atime, st.mtime);
        ++copied_;
        tick(path);
    }

    void report(const std::string& err) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_progress.entries = entries_;
        g_progress.hashed = hashed_;
        g_progress.copied = copied_;
        g_progress.errors = errors_;
        g_progress.seconds = seconds_;
        g_progress.error = err;
        g_progress.current.clear();
    }

    Options o_;
    std::optional<AtomicOutput> listing_;
    std::uint64_t pos_ = 0;
    bool opened_ = false;
    std::uint64_t entries_ = 0, hashed_ = 0, copied_ = 0, errors_ = 0;
    double seconds_ = 0;
    std::vector<std::string> first_errors_;
    std::vector<std::uint8_t> buffer_;
};

std::vector<std::string> strings(const JsonValue& v) {
    std::vector<std::string> out;
    for (const auto& item : v.items())
        if (!item.str().empty()) out.push_back(item.str());
    return out;
}

} // namespace

Options defaults() {
    Options o;
    o.roots = {"/user", "/system_data"};
    o.copy = {"/system_data/priv/mms"};
    return o;
}

std::string parse(const JsonValue& body, Options& o) {
    if (!body.is_object()) return "the body must be a JSON object";
    o.out = body["out"].str();
    if (o.out.empty() || !is_absolute_path(o.out)) return "out: an absolute path for a new folder";
    if (body.has("roots")) o.roots = strings(body["roots"]);
    if (body.has("copy")) o.copy = strings(body["copy"]);
    o.match = body["match"].str();
    if (body.has("hash_max")) o.hash_max = static_cast<std::uint64_t>(body["hash_max"].integer(0));
    if (body.has("copy_max")) o.copy_max = static_cast<std::uint64_t>(body["copy_max"].integer(0));
    if (body.has("max_depth")) o.max_depth = static_cast<int>(body["max_depth"].integer(24));
    for (const auto& r : o.roots)
        if (!is_absolute_path(r)) return "roots: absolute paths only (" + r + ")";
    for (const auto& c : o.copy)
        if (!is_absolute_path(c)) return "copy: absolute paths only (" + c + ")";
    return {};
}

JsonValue run(const Options& options) { return Run(options).go(); }

bool start(const Options& options) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_progress.running) return false;
        g_progress = Progress();
        g_progress.running = true;
        g_progress.out = options.out;
    }
    std::thread([options] {
        (void)run(options);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_progress.running = false;
    }).detach();
    return true;
}

JsonValue status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    JsonValue v = JsonValue::object();
    v.set("running", g_progress.running);
    v.set("out", g_progress.out);
    v.set("entries", g_progress.entries);
    v.set("files_hashed", g_progress.hashed);
    v.set("copied", g_progress.copied);
    v.set("errors", g_progress.errors);
    v.set("seconds", g_progress.seconds);
    if (!g_progress.current.empty()) v.set("current", g_progress.current);
    if (!g_progress.error.empty()) v.set("error", g_progress.error);
    return v;
}

} // namespace alembic::engine::snapshot

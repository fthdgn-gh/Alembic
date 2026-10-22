// SPDX-License-Identifier: GPL-3.0-or-later
#include "peer_fs.hpp"

#include "json_value.hpp"
#include "net.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace alembic::engine::peer_fs {

const char* kind_name(PathKind k) {
    switch (k) {
    case PathKind::file: return "file";
    case PathKind::directory: return "directory";
    case PathKind::link: return "link";
    case PathKind::other: return "other";
    default: return "missing";
    }
}

namespace {

PathStat stat_from(const JsonValue& v) {
    PathStat st;
    std::string k = v["kind"].str();
    st.kind = k == "file" ? PathKind::file : k == "directory" ? PathKind::directory : k == "link" ? PathKind::link
            : k == "other" ? PathKind::other : PathKind::missing;
    st.size = static_cast<std::uint64_t>(v["size"].integer(0));
    st.mtime = v["mtime"].integer(0);
    st.atime = v["atime"].integer(0);
    return st;
}

} // namespace

std::string url_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::optional<Target> parse(const std::string& path) {
    if (path.compare(0, 6, kPrefix) != 0) return std::nullopt;
    std::size_t slash = path.find('/', 6);
    Target t;
    t.id = path.substr(6, slash == std::string::npos ? std::string::npos : slash - 6);
    if (t.id.empty()) return std::nullopt;
    if (slash == std::string::npos || slash + 1 >= path.size()) return t;
    std::string rest = path.substr(slash); // "/data/x" or "/C:/Games"
    if (rest.size() >= 3 && rest[2] == ':' && std::isalpha(static_cast<unsigned char>(rest[1]))) rest.erase(0, 1);
    t.path = rest;
    return t;
}

std::string name_for(const std::string& id, const std::string& local_path) {
    return std::string(kPrefix) + id + (local_path.empty() || local_path[0] == '/' ? "" : "/") + local_path;
}

Result<std::string> request(peers::Discovery& discovery, pairing::Pairing& pairing, const std::string& id,
                            const std::string& method, const std::string& path, const std::map<std::string, std::string>& query,
                            const std::string& body, int timeout_ms, std::size_t max_reply) {
    auto peer = discovery.find(id);
    if (!peer) return make_error(Errc::io, "engine " + id + " is not known here (is it on the network?)");
    if (!pairing.is_paired(id)) return make_error(Errc::io, peer->who.name + " is not paired with this engine");
    ALEMBIC_ASSIGN(auto headers, pairing.sign(id, method, path, query, body));
    std::string target = path;
    char sep = '?';
    for (const auto& [k, v] : query) {
        target += sep + url_encode(k) + "=" + url_encode(v);
        sep = '&';
    }
    auto reply = net::http_request(peer->address, peer->who.port, method, target, body, timeout_ms, max_reply, headers);
    if (!reply.ok()) return make_error(Errc::io, peer->who.name + ": " + reply.error().message);
    if (reply->status != 200) {
        auto v = parse_json(reply->body);
        std::string why = v.ok() ? (*v)["error"].str() : "";
        Errc code = reply->status == 404 ? Errc::invalid_argument : reply->status == 501 ? Errc::unsupported : Errc::io;
        return make_error(code, peer->who.name + ": " + (why.empty() ? "HTTP " + std::to_string(reply->status) : why));
    }
    return std::move(reply->body);
}

namespace {

class PeerFs;

// A file on another engine: reads through a 4 MiB window that moves on with
// the reader; writes collected while they follow each other.
class PeerFile final : public FileHandle {
public:
    PeerFile(PeerFs& fs, Target t, std::uint64_t size) : fs_(fs), t_(std::move(t)), size_(size) {}
    ~PeerFile() override { (void)flush(); }
    Result<std::uint64_t> size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::max(size_, pending_offset_ + pending_.size());
    }
    Status read_at(std::uint64_t offset, MutableBytes out) const override;
    Status write_at(std::uint64_t offset, Bytes data) override;
    Status truncate(std::uint64_t length) override;
    Status sync() override;
    Status close() override { return flush(); }

private:
    Status flush() const;          // the pending writes
    Status flush_locked() const;
    PeerFs& fs_;
    Target t_;
    mutable std::mutex mutex_;
    mutable std::uint64_t size_;
    mutable std::string window_;           // read cache
    mutable std::uint64_t window_offset_ = 0;
    mutable std::string pending_;          // writes not sent yet
    mutable std::uint64_t pending_offset_ = 0;
};

class PeerFs final : public FileSystem {
public:
    PeerFs(peers::Discovery& d, pairing::Pairing& p) : discovery_(d), pairing_(p) {}

    Result<std::string> call(const Target& t, const std::string& method, const std::string& op,
                             std::map<std::string, std::string> query, const std::string& body = "") {
        if (!t.path.empty()) query["path"] = t.path;
        return request(discovery_, pairing_, t.id, method, "/api/peer/fs/" + op, query, body);
    }
    Result<JsonValue> call_json(const Target& t, const std::string& method, const std::string& op,
                                std::map<std::string, std::string> query = {}, const std::string& body = "") {
        ALEMBIC_ASSIGN(std::string text, call(t, method, op, std::move(query), body));
        return parse_json(text.empty() ? "{}" : text);
    }

    static Result<Target> target(const std::string& path) {
        auto t = parse(path);
        if (!t || t->path.empty()) return make_error(Errc::invalid_argument, "not a file on another engine: " + path);
        return *t;
    }

    Result<std::unique_ptr<FileHandle>> open(const std::string& path, File::Mode mode) override {
        ALEMBIC_ASSIGN(Target t, target(path));
        const char* m = mode == File::Mode::create_new ? "create_new" : mode == File::Mode::read_write ? "read_write" : "read";
        ALEMBIC_ASSIGN(JsonValue v, call_json(t, "POST", "open", {{"mode", m}}));
        return std::unique_ptr<FileHandle>(new PeerFile(*this, t, static_cast<std::uint64_t>(v["size"].integer(0))));
    }
    PathStat stat(const std::string& path, bool follow, std::string* error) override {
        auto t = parse(path);
        PathStat st;
        if (!t) { // "/peer" itself
            st.kind = PathKind::directory;
            return st;
        }
        if (t->path.empty()) { // "/peer/<id>": there while it is paired
            if (pairing_.is_paired(t->id)) st.kind = PathKind::directory;
            else if (error) *error = "not paired with that engine";
            return st;
        }
        auto v = call_json(*t, "GET", "stat", {{"follow", follow ? "1" : "0"}});
        if (!v.ok()) {
            if (error) *error = v.error().message;
            return st;
        }
        return stat_from(*v);
    }
    Result<std::vector<DirEntry>> list(const std::string& path, bool follow) override {
        auto t = parse(path);
        std::vector<DirEntry> out;
        if (!t) { // "/peer": the paired engines
            for (const pairing::Paired& p : pairing_.list()) {
                DirEntry e;
                e.name = p.id;
                e.stat.kind = PathKind::directory;
                out.push_back(e);
            }
            return out;
        }
        if (t->path.empty()) return make_error(Errc::unsupported, "list that engine's shared folders instead");
        ALEMBIC_ASSIGN(JsonValue v, call_json(*t, "GET", "list", {{"follow", follow ? "1" : "0"}}));
        for (const JsonValue& item : v["entries"].items()) {
            DirEntry e;
            e.name = item["name"].str();
            if (!safe_file_name(e.name)) continue; // never let a name leave the folder
            e.stat = stat_from(item);
            out.push_back(std::move(e));
        }
        return out;
    }
    Status simple(const std::string& path, const char* op, std::map<std::string, std::string> query = {}) {
        ALEMBIC_ASSIGN(Target t, target(path));
        ALEMBIC_TRY(call(t, "POST", op, std::move(query)));
        return {};
    }
    Status make_directory(const std::string& path) override { return simple(path, "mkdir"); }
    Status remove_file(const std::string& path) override { return simple(path, "remove"); }
    Status remove_directory(const std::string& path) override { return simple(path, "rmdir"); }
    Status rename(const std::string& from, const std::string& to) override {
        ALEMBIC_ASSIGN(Target a, target(from));
        ALEMBIC_ASSIGN(Target b, target(to));
        if (a.id != b.id) return make_error(Errc::unsupported, "cannot rename across engines");
        ALEMBIC_TRY(call(a, "POST", "rename", {{"to", b.path}}));
        return {};
    }
    Status set_times(const std::string& path, std::int64_t atime, std::int64_t mtime) override {
        return simple(path, "times", {{"atime", std::to_string(atime)}, {"mtime", std::to_string(mtime)}});
    }
    Result<DiskSpace> disk_space(const std::string& path) override {
        ALEMBIC_ASSIGN(Target t, target(path));
        ALEMBIC_ASSIGN(JsonValue v, call_json(t, "GET", "space"));
        return DiskSpace{static_cast<std::uint64_t>(v["total"].integer(0)), static_cast<std::uint64_t>(v["free"].integer(0))};
    }

private:
    peers::Discovery& discovery_;
    pairing::Pairing& pairing_;
};

Status PeerFile::read_at(std::uint64_t offset, MutableBytes out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ALEMBIC_TRY(flush_locked()); // read what was written
    std::size_t done = 0;
    while (done < out.size()) {
        std::uint64_t at = offset + done;
        if (at < window_offset_ || at >= window_offset_ + window_.size()) {
            if (at >= size_) return make_error(Errc::invalid_data, "unexpected end of file in '" + t_.path + "'");
            std::uint64_t want = std::min<std::uint64_t>(std::max<std::size_t>(kChunk, out.size() - done), size_ - at);
            want = std::min<std::uint64_t>(want, kChunk);
            ALEMBIC_ASSIGN(window_, fs_.call(t_, "GET", "read", {{"offset", std::to_string(at)}, {"length", std::to_string(want)}}));
            window_offset_ = at;
            if (window_.empty()) return make_error(Errc::invalid_data, "unexpected end of file in '" + t_.path + "'");
        }
        std::size_t from = static_cast<std::size_t>(at - window_offset_);
        std::size_t n = std::min(out.size() - done, window_.size() - from);
        std::memcpy(out.data() + done, window_.data() + from, n);
        done += n;
    }
    return {};
}

Status PeerFile::write_at(std::uint64_t offset, Bytes data) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A write inside the read window makes it stale.
    if (offset < window_offset_ + window_.size() && offset + data.size() > window_offset_) window_.clear();
    while (!data.empty()) {
        bool follows = !pending_.empty() && offset == pending_offset_ + pending_.size();
        if (!follows || pending_.size() >= kChunk) {
            ALEMBIC_TRY(flush_locked());
            pending_offset_ = offset;
        }
        std::size_t n = std::min(data.size(), kChunk - pending_.size());
        pending_.append(reinterpret_cast<const char*>(data.data()), n);
        offset += n;
        data = data.subspan(n);
    }
    return {};
}

Status PeerFile::flush_locked() const {
    if (pending_.empty()) return {};
    ALEMBIC_TRY(fs_.call(t_, "POST", "write", {{"offset", std::to_string(pending_offset_)}}, pending_));
    size_ = std::max(size_, pending_offset_ + pending_.size());
    pending_.clear();
    return {};
}

Status PeerFile::flush() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_locked();
}

Status PeerFile::truncate(std::uint64_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    ALEMBIC_TRY(flush_locked());
    ALEMBIC_TRY(fs_.call(t_, "POST", "truncate", {{"length", std::to_string(length)}}));
    size_ = length;
    window_.clear();
    return {};
}

Status PeerFile::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    ALEMBIC_TRY(flush_locked());
    ALEMBIC_TRY(fs_.call(t_, "POST", "sync", {}));
    return {};
}

} // namespace

std::shared_ptr<FileSystem> make(peers::Discovery& discovery, pairing::Pairing& pairing) {
    return std::make_shared<PeerFs>(discovery, pairing);
}

} // namespace alembic::engine::peer_fs

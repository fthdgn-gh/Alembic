// SPDX-License-Identifier: GPL-3.0-or-later
#include "netfs.hpp"

#include "ftp.hpp"
#include "json_value.hpp"
#include "net.hpp"
#include "smb.hpp"
#include "webdav.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

namespace alembic::engine::netfs {

const char* protocol_name(Protocol p) {
    switch (p) {
    case Protocol::smb: return "smb";
    case Protocol::webdav: return "webdav";
    default: return "ftp";
    }
}

const char* protocol_label(Protocol p) {
    switch (p) {
    case Protocol::smb: return "SMB";
    case Protocol::webdav: return "WebDAV";
    default: return "FTP";
    }
}

const char* path_prefix(Protocol p) {
    switch (p) {
    case Protocol::smb: return "/smb";
    case Protocol::webdav: return "/dav";
    default: return "/ftp";
    }
}

std::uint16_t default_port(Protocol p) {
    switch (p) {
    case Protocol::smb: return 445;
    case Protocol::webdav: return 80;
    default: return 21;
    }
}

std::optional<Protocol> parse_protocol(const std::string& name) {
    if (name.empty() || name == "ftp") return Protocol::ftp;
    if (name == "smb") return Protocol::smb;
    if (name == "webdav" || name == "dav") return Protocol::webdav;
    return std::nullopt;
}

std::string Server::path() const { return std::string(path_prefix(protocol)) + "/" + id; }

Result<std::unique_ptr<Session>> connect(const Server& server) {
    switch (server.protocol) {
    case Protocol::smb: return smb::open(server);
    case Protocol::webdav: return webdav::open(server);
    default: return ftp::open(server);
    }
}

Status normalise(Server& s) {
    auto trim = [](std::string v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        std::size_t i = 0;
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        return v.substr(i);
    };
    s.host = trim(s.host);
    s.root = trim(s.root);
    s.user = trim(s.user);
    // A URL in the address: scheme, host, port and path.
    if (std::size_t scheme = s.host.find("://"); scheme != std::string::npos) {
        std::string kind = s.host.substr(0, scheme);
        for (char& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string rest = s.host.substr(scheme + 3);
        if (kind == "https" || kind == "davs" || kind == "ftps" || kind == "sftp")
            return make_error(Errc::invalid_argument, kind + " is not supported: use plain " + protocol_label(s.protocol));
        std::size_t slash = rest.find('/');
        std::string authority = rest.substr(0, slash), path = slash == std::string::npos ? "" : rest.substr(slash);
        if (std::size_t at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
        if (std::size_t colon = authority.rfind(':'); colon != std::string::npos && authority.find(']') == std::string::npos) {
            int port = std::atoi(authority.c_str() + colon + 1);
            if (port > 0 && port < 65536) s.port = static_cast<std::uint16_t>(port);
            authority = authority.substr(0, colon);
        }
        s.host = authority;
        if (!path.empty() && path != "/" && s.root.empty()) s.root = path;
    } else if (s.protocol == Protocol::smb && s.host.rfind("\\\\", 0) == 0) {
        // "\\pc\Games\PS5", as Windows writes it.
        std::string rest = s.host.substr(2);
        std::size_t slash = rest.find('\\');
        s.host = rest.substr(0, slash);
        if (slash != std::string::npos && s.root.empty()) s.root = rest.substr(slash + 1);
    }
    if (s.host.empty()) return make_error(Errc::invalid_argument, std::string("the ") + protocol_label(s.protocol) + " server needs an address");
    if (!s.port) s.port = default_port(s.protocol);
    for (char& c : s.root)
        if (c == '\\') c = '/';
    if (s.protocol == Protocol::smb) {
        while (!s.root.empty() && s.root.front() == '/') s.root.erase(0, 1);
        while (!s.root.empty() && s.root.back() == '/') s.root.pop_back();
        if (s.root.empty()) return make_error(Errc::invalid_argument, "the SMB server needs a share name");
    } else if (s.protocol == Protocol::webdav) {
        if (s.root.empty() || s.root.front() != '/') s.root = "/" + s.root;
        while (s.root.size() > 1 && s.root.back() == '/') s.root.pop_back();
    } else {
        s.root.clear();
        if (s.user.empty()) s.user = "anonymous";
    }
    return {};
}

// ---------------------------------------------------------------- sockets

namespace {
constexpr int kIoTimeoutSeconds = 30;
}

Result<std::intptr_t> tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms, const char* label, std::string* peer) {
    auto fail = [&](const std::string& what) { return make_error(Errc::io, std::string(label) + ": " + what); };
    if (!net::startup()) return fail("cannot start the network (Winsock)");
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found)
        return fail("cannot find " + host);
    sockaddr_in addr{};
    std::memcpy(&addr, found->ai_addr, sizeof addr);
    ::freeaddrinfo(found);
    std::string why;
    net::Socket fd = net::connect(addr, timeout_ms, false, &why);
    if (fd == net::kNone) return fail("cannot connect to " + host + ":" + std::to_string(port) + " (" + why + ")");
    net::set_timeouts(fd, kIoTimeoutSeconds);
    net::set_no_delay(fd);
    if (peer) {
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text);
        *peer = text;
    }
    return static_cast<std::intptr_t>(fd);
}

Status send_all(std::intptr_t fd, const void* data, std::size_t size, const char* label) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        long n = net::send(fd, p, size);
        if (n < 0 && net::should_retry()) continue;
        if (n <= 0) return make_error(Errc::io, std::string(label) + ": send: " + net::error_text());
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return {};
}

Status recv_all(std::intptr_t fd, void* data, std::size_t size, const char* label) {
    char* p = static_cast<char*>(data);
    while (size) {
        long n = net::recv(fd, p, size);
        if (n < 0 && net::should_retry()) continue;
        if (n == 0) return make_error(Errc::io, std::string(label) + ": the server closed the connection");
        if (n < 0) return make_error(Errc::io, std::string(label) + ": receive: " + net::error_text());
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return {};
}

// ---------------------------------------------------------------- files

namespace {
constexpr std::uint64_t kPiece = 1 << 20;
constexpr std::uint64_t kAhead = 16; // pieces read ahead of a forward read
constexpr std::size_t kKeep = 64;    // pieces kept
} // namespace

// A file on a server: one download runs on a thread of its own, from where
// the reader is, and keeps up to kAhead pieces ahead of it.
struct Servers::File final : RandomAccess {
    Server server;
    std::string path;
    std::uint64_t length = 0;

    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    mutable std::map<std::uint64_t, std::shared_ptr<const std::vector<std::uint8_t>>> pieces;
    mutable std::deque<std::uint64_t> order;
    mutable std::uint64_t next = 0;   // the piece the download produces next
    mutable std::uint64_t target = 0; // read up to this piece (inclusive)
    mutable std::uint64_t last = ~std::uint64_t{0};
    mutable std::optional<Error> failed;
    mutable bool wanted = false; // nothing is downloaded before the first read
    bool stop = false;
    std::thread thread;

    std::uint64_t count() const { return (length + kPiece - 1) / kPiece; }

    void start() { thread = std::thread([this] { run(); }); }
    ~File() override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        changed.notify_all();
        if (thread.joinable()) thread.join();
    }

    void run() {
        std::unique_ptr<Session> conn;
        std::uint64_t at = ~std::uint64_t{0}; // where the open download is, in pieces
        std::vector<std::uint8_t> buffer;
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stop || (wanted && next <= target && next < count() && !failed); });
            if (stop) return;
            std::uint64_t k = next;
            if (pieces.count(k)) { // already here
                ++next;
                continue;
            }
            lock.unlock();
            Status s;
            if (!conn) {
                auto c = connect(server);
                if (c.ok()) conn = std::move(*c);
                else s = c.error();
            }
            if (s.ok() && at != k) {
                // A connection whose replies may be out of step is not reused.
                if (at != ~std::uint64_t{0} && !conn->stop_download().ok()) {
                    conn.reset();
                    auto c = connect(server);
                    if (c.ok()) conn = std::move(*c);
                    else s = c.error();
                }
                if (s.ok()) s = conn->start_download(path, k * kPiece);
                at = s.ok() ? k : ~std::uint64_t{0};
            }
            std::uint64_t want = std::min(kPiece, length - k * kPiece);
            buffer.resize(static_cast<std::size_t>(want));
            for (std::uint64_t got = 0; s.ok() && got < want;) {
                auto n = conn->read(MutableBytes(buffer.data() + got, static_cast<std::size_t>(want - got)));
                if (!n.ok()) s = n.error();
                else if (*n == 0) s = make_error(Errc::invalid_data, path + " ended early");
                else got += *n;
            }
            lock.lock();
            if (!s.ok()) {
                conn.reset(); // start clean next time
                at = ~std::uint64_t{0};
                failed = s.error();
                changed.notify_all();
                continue;
            }
            at = k + 1;
            if (at >= count()) {
                lock.unlock();
                // The end and the server's confirmation; without them the connection is dropped.
                auto end = conn->read(MutableBytes(buffer.data(), 1));
                if (!end.ok() || *end != 0) conn.reset();
                lock.lock();
                at = ~std::uint64_t{0};
            }
            pieces[k] = std::make_shared<const std::vector<std::uint8_t>>(buffer);
            order.push_back(k);
            while (order.size() > kKeep) {
                pieces.erase(order.front());
                order.pop_front();
            }
            if (next == k) ++next;
            changed.notify_all();
        }
    }

    Result<std::shared_ptr<const std::vector<std::uint8_t>>> piece(std::uint64_t k) const {
        std::unique_lock<std::mutex> lock(mutex);
        failed.reset();
        wanted = true;
        const bool forward = last != ~std::uint64_t{0} && (k == last || k == last + 1);
        last = k;
        // Not here and not coming next: the download moves there.
        if (!pieces.count(k) && !(k >= next && k <= next + 2)) next = k;
        // Reading forward, keep kAhead pieces coming; after a jump, just this one.
        target = forward ? std::max(target, k + kAhead) : k;
        changed.notify_all();
        for (;;) {
            if (auto it = pieces.find(k); it != pieces.end()) return it->second;
            if (failed) return *failed;
            changed.wait(lock);
        }
    }

    Result<std::uint64_t> size() const override { return length; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > length || out.size() > length - offset) return make_error(Errc::invalid_argument, "read past the end of " + path);
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

namespace {

// An upload under a temporary name; commit renames it to the real one.
class Upload final : public convert::OutputStream {
public:
    Upload(std::unique_ptr<Session> conn, std::string temp, std::string final, bool overwrite)
        : conn_(std::move(conn)), temp_(std::move(temp)), final_(std::move(final)), overwrite_(overwrite) {}
    ~Upload() override {
        if (!committed_ && conn_) {
            (void)conn_->finish_upload();
            (void)conn_->remove(temp_);
        }
    }
    Status write(Bytes data) override { return conn_->write(data); }
    Status commit() override {
        ALEMBIC_TRY(conn_->finish_upload());
        if (overwrite_) (void)conn_->remove(final_);
        ALEMBIC_TRY(conn_->rename(temp_, final_));
        committed_ = true;
        return {};
    }

private:
    std::unique_ptr<Session> conn_;
    std::string temp_, final_;
    bool overwrite_;
    bool committed_ = false;
};

std::string read_text(const std::string& path) {
    std::FILE* f = alembic::open_stdio(path, "rb");
    if (!f) return "";
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return text;
}

} // namespace

Servers::Servers(std::string state_dir)
    : file_(state_dir + "/servers.json"), legacy_file_(std::move(state_dir) + "/ftp.json") {
    // servers.json since SMB and WebDAV; ftp.json (FTP only) before.
    std::string text = read_text(file_);
    if (text.empty()) text = read_text(legacy_file_);
    if (text.empty()) return;
    auto v = parse_json(text);
    if (!v.ok()) return;
    for (const JsonValue& s : (*v)["servers"].items()) {
        auto protocol = parse_protocol(s["protocol"].str());
        if (!protocol) continue; // from a newer engine
        Server server;
        server.protocol = *protocol;
        server.id = s["id"].str();
        server.name = s["name"].str();
        server.host = s["host"].str();
        server.port = static_cast<std::uint16_t>(s["port"].integer(default_port(*protocol)));
        server.user = s["user"].str(*protocol == Protocol::ftp ? "anonymous" : "");
        server.password = s["password"].str();
        server.root = s["root"].str();
        if (!server.id.empty() && !server.host.empty()) servers_.push_back(std::move(server));
    }
}

Servers::~Servers() = default;

std::vector<Server> Servers::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return servers_;
}

Status Servers::save() const {
    JsonValue list = JsonValue::array();
    for (const Server& s : servers_) {
        JsonValue v = JsonValue::object();
        v.set("id", s.id);
        v.set("protocol", protocol_name(s.protocol));
        v.set("name", s.name);
        v.set("host", s.host);
        v.set("port", static_cast<std::int64_t>(s.port));
        v.set("user", s.user);
        v.set("password", s.password);
        if (!s.root.empty()) v.set("root", s.root);
        list.push(std::move(v));
    }
    JsonValue root = JsonValue::object();
    root.set("servers", std::move(list));
    std::string text = root.dump();
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(file_, true));
    ALEMBIC_TRY(out.file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    return out.commit();
}

Result<Server> Servers::add(Server server) {
    ALEMBIC_TRY(normalise(server));
    ALEMBIC_ASSIGN(auto conn, connect(server)); // check the login
    (void)conn;
    // An id that stays the same for the same server, user and share, so
    // paths last. The port is part of it unless it is the protocol's own.
    std::string base;
    std::string from = server.host + (server.port == default_port(server.protocol) ? "" : "-" + std::to_string(server.port)) +
                       "-" + (server.user.empty() ? std::string("guest") : server.user);
    if (server.protocol != Protocol::ftp && server.root != "/") from += "-" + server.root;
    for (char c : from) base += (std::isalnum(static_cast<unsigned char>(c)) ? c : '-');
    while (base.find("--") != std::string::npos && server.protocol != Protocol::ftp) base.erase(base.find("--"), 1);
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.size() > 40) base.resize(40);
    std::lock_guard<std::mutex> lock(mutex_);
    // The same server again replaces it; another server that took this id
    // (one edited since) keeps it, and this one gets "-2", "-3", ...
    auto same = [&](const Server& s) {
        return s.protocol == server.protocol && s.host == server.host && s.port == server.port && s.user == server.user &&
               s.root == server.root;
    };
    std::string id = base;
    for (int n = 2;; ++n) {
        auto it = std::find_if(servers_.begin(), servers_.end(), [&](const Server& s) { return s.protocol == server.protocol && s.id == id; });
        if (it == servers_.end() || same(*it)) break;
        id = base + "-" + std::to_string(n);
    }
    std::erase_if(servers_, [&](const Server& s) { return s.protocol == server.protocol && s.id == id; });
    server.id = id;
    if (server.name.empty()) server.name = server.host;
    servers_.push_back(server);
    ALEMBIC_TRY(save());
    return server;
}

Result<Server> Servers::update(const std::string& id, Server server, bool keep_password) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(servers_.begin(), servers_.end(), [&](const Server& s) { return s.id == id; });
        if (it == servers_.end()) return make_error(Errc::invalid_argument, "no such server: " + id);
        if (keep_password) server.password = it->password;
        server.protocol = it->protocol;
    }
    ALEMBIC_TRY(normalise(server));
    ALEMBIC_ASSIGN(auto conn, connect(server)); // check the login
    (void)conn;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(servers_.begin(), servers_.end(), [&](const Server& s) { return s.id == id && s.protocol == server.protocol; });
    if (it == servers_.end()) return make_error(Errc::invalid_argument, "no such server: " + id);
    server.id = id;
    if (server.name.empty()) server.name = server.host;
    *it = server;
    ALEMBIC_TRY(save());
    return server;
}

Status Servers::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(servers_, [&](const Server& s) { return s.id == id; });
    return save();
}

namespace {
// The protocol a path's prefix names, and where the server id starts.
std::optional<Protocol> prefix_of(const std::string& path) {
    for (Protocol p : {Protocol::ftp, Protocol::smb, Protocol::webdav}) {
        std::string prefix = std::string(path_prefix(p)) + "/";
        if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0) return p;
    }
    return std::nullopt;
}
} // namespace

bool Servers::handles(const std::string& path) { return prefix_of(path).has_value(); }

Result<std::pair<Server, std::string>> Servers::split(const std::string& path) const {
    auto protocol = prefix_of(path);
    if (!protocol) return make_error(Errc::invalid_argument, "not a network server path: " + path);
    const std::size_t start = std::strlen(path_prefix(*protocol)) + 1;
    std::size_t slash = path.find('/', start);
    std::string id = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    std::string on_server = slash == std::string::npos ? "/" : path.substr(slash);
    while (on_server.size() > 1 && on_server.back() == '/') on_server.pop_back();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Server& s : servers_)
        if (s.protocol == *protocol && s.id == id) return std::make_pair(s, on_server);
    return make_error(Errc::invalid_argument, std::string("no such ") + protocol_label(*protocol) + " server: " + id);
}

std::optional<Server> Servers::server_of(const std::string& path) const {
    auto where = split(path);
    if (!where.ok()) return std::nullopt;
    return where->first;
}

std::shared_ptr<const RandomAccess> Servers::open(const std::string& path) {
    if (!handles(path)) return nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = open_.find(path); it != open_.end())
            if (auto f = it->second.lock()) return f;
    }
    auto where = split(path);
    if (!where.ok()) return nullptr;
    auto conn = connect(where->first);
    if (!conn.ok()) return nullptr;
    auto size = (*conn)->size(where->second);
    if (!size.ok()) return nullptr;
    auto f = std::make_shared<File>();
    f->server = where->first;
    f->path = where->second;
    f->length = *size;
    f->start();
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(open_, [](const auto& e) { return e.second.expired(); });
    open_[path] = f;
    return f;
}

Result<std::vector<Entry>> Servers::list_directory(const std::string& path) {
    ALEMBIC_ASSIGN(auto where, split(path));
    ALEMBIC_ASSIGN(auto conn, connect(where.first));
    return conn->list(where.second);
}

namespace {

// Makes each missing folder above `path` on the server (as mkdir -p does).
// Failures are ignored: an existing folder fails too, and the command that
// follows reports a real problem.
void make_parents(Session& conn, const std::string& path) {
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
        (void)conn.make_directory(path.substr(0, slash));
}

bool exists_on(Session& conn, const std::string& path) {
    if (path == "/") return true;
    if (conn.size(path).ok()) return true;
    // A folder: its parent lists it.
    std::size_t slash = path.rfind('/');
    std::string parent = slash == 0 ? "/" : path.substr(0, slash), name = path.substr(slash + 1);
    auto entries = conn.list(parent);
    if (!entries.ok()) return false;
    for (const Entry& e : *entries)
        if (e.name == name) return true;
    return false;
}

} // namespace

Status Servers::make_directory(const std::string& path) {
    ALEMBIC_ASSIGN(auto where, split(path));
    ALEMBIC_ASSIGN(auto conn, connect(where.first));
    make_parents(*conn, where.second);
    return conn->make_directory(where.second);
}

bool Servers::exists(const std::string& path) {
    auto where = split(path);
    if (!where.ok()) return false;
    auto conn = connect(where->first);
    if (!conn.ok()) return false;
    return exists_on(**conn, where->second);
}

Result<std::unique_ptr<convert::OutputStream>> Servers::create(const std::string& path, bool overwrite) {
    ALEMBIC_ASSIGN(auto where, split(path));
    ALEMBIC_ASSIGN(auto conn, connect(where.first));
    if (!overwrite && exists_on(*conn, where.second)) return make_error(Errc::invalid_argument, "the output file already exists: " + path);
    std::string temp = where.second + ".alembic-part";
    make_parents(*conn, where.second);
    ALEMBIC_TRY(conn->start_upload(temp));
    return std::unique_ptr<convert::OutputStream>(new Upload(std::move(conn), temp, where.second, overwrite));
}

} // namespace alembic::engine::netfs

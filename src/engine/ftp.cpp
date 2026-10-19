// SPDX-License-Identifier: GPL-3.0-or-later
#include "ftp.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace alembic::engine::ftp {

namespace {

constexpr int kIoTimeoutSeconds = 30;

Error net_error(const std::string& what) { return make_error(Errc::io, "FTP: " + what); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

// A TCP connection to host:port, with a timeout on connecting and on I/O.
Result<int> connect_to(const std::string& host, std::uint16_t port, int timeout_ms, std::string* peer = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found)
        return net_error("cannot find " + host);
    sockaddr_in addr{};
    std::memcpy(&addr, found->ai_addr, sizeof addr);
    ::freeaddrinfo(found);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return net_error(std::string("socket: ") + std::strerror(errno));
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        bool ok = errno == EINPROGRESS;
        if (ok) {
            pollfd p{fd, POLLOUT, 0};
            ok = ::poll(&p, 1, timeout_ms) > 0;
            int err = 0;
            socklen_t len = sizeof err;
            if (ok) ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            ok = ok && err == 0;
        }
        if (!ok) {
            ::close(fd);
            return net_error("cannot connect to " + host + ":" + std::to_string(port));
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    timeval tv{kIoTimeoutSeconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (peer) {
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text);
        *peer = text;
    }
    return fd;
}

Status send_all(int fd, const char* data, std::size_t size) {
    while (size) {
        ssize_t n = ::send(fd, data, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return net_error(std::string("send: ") + std::strerror(errno));
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return {};
}

// "type=file;size=12;modify=20261019101500; name" (MLSD)
bool parse_mlsd(const std::string& line, Entry& e) {
    std::size_t space = line.find(' ');
    if (space == std::string::npos) return false;
    e.name = line.substr(space + 1);
    std::string facts = lower(line.substr(0, space));
    std::size_t at = 0;
    while (at < facts.size()) {
        std::size_t semi = facts.find(';', at);
        std::string fact = facts.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
        at = semi == std::string::npos ? facts.size() : semi + 1;
        std::size_t eq = fact.find('=');
        if (eq == std::string::npos) continue;
        std::string key = fact.substr(0, eq), value = fact.substr(eq + 1);
        if (key == "type") {
            if (value == "cdir" || value == "pdir") return false;
            e.is_directory = value == "dir";
        } else if (key == "size") {
            e.size = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "modify" && value.size() >= 14) {
            std::tm tm{};
            tm.tm_year = std::atoi(value.substr(0, 4).c_str()) - 1900;
            tm.tm_mon = std::atoi(value.substr(4, 2).c_str()) - 1;
            tm.tm_mday = std::atoi(value.substr(6, 2).c_str());
            tm.tm_hour = std::atoi(value.substr(8, 2).c_str());
            tm.tm_min = std::atoi(value.substr(10, 2).c_str());
            tm.tm_sec = std::atoi(value.substr(12, 2).c_str());
            e.mtime = static_cast<std::int64_t>(::timegm(&tm));
        }
    }
    return e.name != "." && e.name != "..";
}

// LIST in the Unix ("-rw-r--r-- 1 u g 123 Oct 19 10:15 name") or the
// Windows ("10-19-26  10:15AM  <DIR>  name") style; no times.
bool parse_list(const std::string& line, Entry& e) {
    if (line.empty()) return false;
    std::vector<std::string> words;
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < line.size();) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        starts.push_back(i);
        std::size_t j = line.find(' ', i);
        words.push_back(line.substr(i, j == std::string::npos ? std::string::npos : j - i));
        i = j == std::string::npos ? line.size() : j;
    }
    if (words.size() >= 9 && (line[0] == '-' || line[0] == 'd' || line[0] == 'l')) {
        e.is_directory = line[0] == 'd';
        e.size = std::strtoull(words[4].c_str(), nullptr, 10);
        e.name = line.substr(starts[8]);
        if (line[0] == 'l') e.name = e.name.substr(0, e.name.find(" -> "));
    } else if (words.size() >= 4 && words[0].size() >= 8 && words[0][2] == '-') {
        e.is_directory = words[2] == "<DIR>";
        e.size = e.is_directory ? 0 : std::strtoull(words[2].c_str(), nullptr, 10);
        e.name = line.substr(starts[3]);
    } else {
        return false;
    }
    return e.name != "." && e.name != "..";
}

} // namespace

// ---------------------------------------------------------------- Connection

Connection::~Connection() {
    if (data_ >= 0) ::close(data_);
    if (control_ >= 0) {
        (void)send_all(control_, "QUIT\r\n", 6);
        ::close(control_);
    }
}

Result<Connection::Reply> Connection::reply() {
    // A reply is "123 text", or lines from "123-text" to "123 text".
    std::string first_code;
    Reply r;
    for (;;) {
        std::size_t eol;
        while ((eol = buffer_.find("\r\n")) == std::string::npos) {
            char chunk[4096];
            ssize_t n = ::recv(control_, chunk, sizeof chunk, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return net_error("the server closed the connection");
            buffer_.append(chunk, static_cast<std::size_t>(n));
        }
        std::string line = buffer_.substr(0, eol);
        buffer_.erase(0, eol + 2);
        if (first_code.empty()) {
            if (line.size() < 3) return net_error("unexpected reply: " + line);
            first_code = line.substr(0, 3);
            r.code = std::atoi(first_code.c_str());
            r.text = line.size() > 4 ? line.substr(4) : "";
            if (line.size() < 4 || line[3] != '-') return r;
        } else if (line.size() >= 4 && line.compare(0, 3, first_code) == 0 && line[3] == ' ') {
            return r;
        }
    }
}

Result<Connection::Reply> Connection::command(const std::string& line) {
    std::string out = line + "\r\n";
    ALEMBIC_TRY(send_all(control_, out.data(), out.size()));
    return reply();
}

Result<std::unique_ptr<Connection>> Connection::open(const Server& server, int timeout_ms) {
    std::unique_ptr<Connection> c(new Connection());
    ALEMBIC_ASSIGN(c->control_, connect_to(server.host, server.port, timeout_ms, &c->peer_));
    ALEMBIC_ASSIGN(Reply hello, c->reply());
    if (hello.code != 220) return net_error("the server refused the connection: " + hello.text);
    ALEMBIC_ASSIGN(Reply user, c->command("USER " + server.user));
    if (user.code == 331 || user.code == 332) {
        // Anonymous logins conventionally give an e-mail-like password; some
        // servers refuse an empty one.
        std::string password = server.password;
        if (password.empty() && (server.user == "anonymous" || server.user == "ftp")) password = "anonymous@";
        ALEMBIC_ASSIGN(Reply pass, c->command("PASS " + password));
        if (pass.code != 230 && pass.code != 202) return net_error("login failed: " + pass.text);
    } else if (user.code != 230) {
        return net_error("login failed: " + user.text);
    }
    ALEMBIC_ASSIGN(Reply type, c->command("TYPE I"));
    if (type.code != 200) return net_error("binary mode refused: " + type.text);
    (void)c->command("OPTS UTF8 ON"); // optional
    return c;
}

Status Connection::open_data() {
    if (data_ >= 0) {
        ::close(data_);
        data_ = -1;
    }
    std::uint16_t port = 0;
    ALEMBIC_ASSIGN(Reply epsv, command("EPSV"));
    if (epsv.code == 229) {
        // "Entering Extended Passive Mode (|||6446|)"
        std::size_t open = epsv.text.find("(|||");
        if (open != std::string::npos) port = static_cast<std::uint16_t>(std::atoi(epsv.text.c_str() + open + 4));
    }
    if (!port) {
        ALEMBIC_ASSIGN(Reply pasv, command("PASV"));
        if (pasv.code != 227) return net_error("passive mode refused: " + pasv.text);
        // "(h1,h2,h3,h4,p1,p2)": the address is the control connection's (NAT).
        std::size_t open = pasv.text.find('(');
        int v[6] = {};
        if (open == std::string::npos ||
            std::sscanf(pasv.text.c_str() + open, "(%d,%d,%d,%d,%d,%d)", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
            return net_error("cannot read the passive address: " + pasv.text);
        port = static_cast<std::uint16_t>(v[4] * 256 + v[5]);
    }
    ALEMBIC_ASSIGN(data_, connect_to(peer_, port, 10000));
    data_ended_ = false;
    return {};
}

Result<std::uint64_t> Connection::size(const std::string& path) {
    ALEMBIC_ASSIGN(Reply r, command("SIZE " + path));
    if (r.code != 213) return make_error(Errc::invalid_argument, "FTP: no such file: " + path);
    return static_cast<std::uint64_t>(std::strtoull(r.text.c_str(), nullptr, 10));
}

Result<std::vector<Entry>> Connection::list(const std::string& directory) {
    bool mlsd = true;
    ALEMBIC_TRY(open_data());
    ALEMBIC_ASSIGN(Reply r, command("MLSD " + directory));
    if (r.code >= 500) { // no MLSD: LIST
        mlsd = false;
        ALEMBIC_TRY(open_data());
        ALEMBIC_ASSIGN(r, command("LIST " + directory));
    }
    if (r.code != 150 && r.code != 125) {
        ::close(data_);
        data_ = -1;
        return make_error(Errc::invalid_argument, "FTP: cannot list " + directory + ": " + r.text);
    }
    std::string text;
    char chunk[16384];
    for (;;) {
        ssize_t n = ::recv(data_, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return net_error("listing interrupted");
        if (n == 0) break;
        text.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(data_);
    data_ = -1;
    ALEMBIC_ASSIGN(Reply done, reply());
    if (done.code != 226 && done.code != 250) return net_error("listing failed: " + done.text);
    std::vector<Entry> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t eol = text.find('\n', at);
        std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        at = eol == std::string::npos ? text.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Entry e;
        if (mlsd ? parse_mlsd(line, e) : parse_list(line, e)) out.push_back(std::move(e));
    }
    return out;
}

Status Connection::make_directory(const std::string& path) {
    ALEMBIC_ASSIGN(Reply r, command("MKD " + path));
    if (r.code != 257 && r.code != 250) return make_error(Errc::io, "FTP: cannot create " + path + ": " + r.text);
    return {};
}

Status Connection::rename(const std::string& from, const std::string& to) {
    ALEMBIC_ASSIGN(Reply a, command("RNFR " + from));
    if (a.code != 350) return make_error(Errc::io, "FTP: cannot rename " + from + ": " + a.text);
    ALEMBIC_ASSIGN(Reply b, command("RNTO " + to));
    if (b.code != 250) return make_error(Errc::io, "FTP: cannot rename to " + to + ": " + b.text);
    return {};
}

Status Connection::remove(const std::string& path) {
    ALEMBIC_ASSIGN(Reply r, command("DELE " + path));
    if (r.code != 250) return make_error(Errc::io, "FTP: cannot delete " + path + ": " + r.text);
    return {};
}

Status Connection::start_download(const std::string& path, std::uint64_t offset) {
    ALEMBIC_TRY(open_data());
    if (offset) {
        ALEMBIC_ASSIGN(Reply rest, command("REST " + std::to_string(offset)));
        if (rest.code != 350) return net_error("the server cannot read from an offset: " + rest.text);
    }
    ALEMBIC_ASSIGN(Reply r, command("RETR " + path));
    if (r.code != 150 && r.code != 125) {
        ::close(data_);
        data_ = -1;
        return make_error(Errc::io, "FTP: cannot read " + path + ": " + r.text);
    }
    return {};
}

Result<std::size_t> Connection::read(MutableBytes out) {
    if (data_ < 0 || data_ended_) return std::size_t{0};
    for (;;) {
        ssize_t n = ::recv(data_, out.data(), out.size(), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return net_error("download interrupted");
        if (n == 0) {
            data_ended_ = true;
            ::close(data_);
            data_ = -1;
            ALEMBIC_ASSIGN(Reply done, reply());
            if (done.code != 226 && done.code != 250) return net_error("download failed: " + done.text);
        }
        return static_cast<std::size_t>(n);
    }
}

Status Connection::stop_download() {
    if (data_ < 0) return {};
    // Dropping the data connection ends the transfer; the server then sends
    // 426, or 226 if it had already sent everything. Servers differ in what
    // they send (and how many replies) around an abort, so instead of ABOR a
    // NOOP follows, and every reply up to the NOOP's 200 is skipped: the
    // next command then reads its own reply.
    ::close(data_);
    data_ = -1;
    ALEMBIC_TRY(send_all(control_, "NOOP\r\n", 6));
    for (int i = 0; i < 8; ++i) {
        ALEMBIC_ASSIGN(Reply r, reply());
        if (r.code == 200) return {};
    }
    return net_error("the server did not answer after a stopped download");
}

Status Connection::start_upload(const std::string& path) {
    ALEMBIC_TRY(open_data());
    ALEMBIC_ASSIGN(Reply r, command("STOR " + path));
    if (r.code != 150 && r.code != 125) {
        ::close(data_);
        data_ = -1;
        return make_error(Errc::io, "FTP: cannot write " + path + ": " + r.text);
    }
    return {};
}

Status Connection::write(Bytes data) {
    if (data_ < 0) return net_error("no upload is open");
    return send_all(data_, reinterpret_cast<const char*>(data.data()), data.size());
}

Status Connection::finish_upload() {
    if (data_ < 0) return net_error("no upload is open");
    ::close(data_);
    data_ = -1;
    ALEMBIC_ASSIGN(Reply done, reply());
    if (done.code != 226 && done.code != 250) return net_error("upload failed: " + done.text);
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
        std::unique_ptr<Connection> conn;
        std::uint64_t at = ~std::uint64_t{0}; // where the open download is, in pieces
        std::vector<std::uint8_t> buffer;
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stop || (next <= target && next < count() && !failed); });
            if (stop) return;
            std::uint64_t k = next;
            if (pieces.count(k)) { // already here
                ++next;
                continue;
            }
            lock.unlock();
            Status s;
            if (!conn) {
                auto c = Connection::open(server);
                if (c.ok()) conn = std::move(*c);
                else s = c.error();
            }
            if (s.ok() && at != k) {
                if (at != ~std::uint64_t{0}) (void)conn->stop_download();
                s = conn->start_download(path, k * kPiece);
                at = s.ok() ? k : ~std::uint64_t{0};
            }
            std::uint64_t want = std::min(kPiece, length - k * kPiece);
            buffer.resize(static_cast<std::size_t>(want));
            for (std::uint64_t got = 0; s.ok() && got < want;) {
                auto n = conn->read(MutableBytes(buffer.data() + got, static_cast<std::size_t>(want - got)));
                if (!n.ok()) s = n.error();
                else if (*n == 0) s = make_error(Errc::invalid_data, "FTP: " + path + " ended early");
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
                (void)conn->read(MutableBytes(buffer.data(), 1)); // the end and its 226
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
    Upload(std::unique_ptr<Connection> conn, std::string temp, std::string final, bool overwrite)
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
    std::unique_ptr<Connection> conn_;
    std::string temp_, final_;
    bool overwrite_;
    bool committed_ = false;
};

} // namespace

Servers::Servers(std::string state_dir) : file_(std::move(state_dir) + "/ftp.json") {
    std::FILE* f = std::fopen(file_.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    auto v = parse_json(text);
    if (!v.ok()) return;
    for (const JsonValue& s : (*v)["servers"].items()) {
        Server server;
        server.id = s["id"].str();
        server.name = s["name"].str();
        server.host = s["host"].str();
        server.port = static_cast<std::uint16_t>(s["port"].integer(21));
        server.user = s["user"].str("anonymous");
        server.password = s["password"].str();
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
        v.set("name", s.name);
        v.set("host", s.host);
        v.set("port", static_cast<std::int64_t>(s.port));
        v.set("user", s.user);
        v.set("password", s.password);
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
    if (server.host.empty()) return make_error(Errc::invalid_argument, "the FTP server needs an address");
    if (server.user.empty()) server.user = "anonymous";
    ALEMBIC_ASSIGN(auto conn, Connection::open(server)); // check the login
    (void)conn;
    // An id that stays the same for the same host and user, so paths last.
    std::string base;
    for (char c : server.host + "-" + server.user)
        base += (std::isalnum(static_cast<unsigned char>(c)) ? c : '-');
    if (base.size() > 40) base.resize(40);
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(servers_, [&](const Server& s) { return s.id == base; });
    server.id = base;
    if (server.name.empty()) server.name = server.host;
    servers_.push_back(server);
    ALEMBIC_TRY(save());
    return server;
}

Status Servers::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(servers_, [&](const Server& s) { return s.id == id; });
    return save();
}

bool Servers::is_ftp(const std::string& path) { return path.rfind("/ftp/", 0) == 0 && path.size() > 5; }

Result<std::pair<Server, std::string>> Servers::split(const std::string& path) const {
    if (!is_ftp(path)) return make_error(Errc::invalid_argument, "not an FTP path: " + path);
    std::size_t slash = path.find('/', 5);
    std::string id = path.substr(5, slash == std::string::npos ? std::string::npos : slash - 5);
    std::string on_server = slash == std::string::npos ? "/" : path.substr(slash);
    while (on_server.size() > 1 && on_server.back() == '/') on_server.pop_back();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Server& s : servers_)
        if (s.id == id) return std::make_pair(s, on_server);
    return make_error(Errc::invalid_argument, "no such FTP server: " + id);
}

std::shared_ptr<const RandomAccess> Servers::open(const std::string& path) {
    if (!is_ftp(path)) return nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = open_.find(path); it != open_.end())
            if (auto f = it->second.lock()) return f;
    }
    auto where = split(path);
    if (!where.ok()) return nullptr;
    auto conn = Connection::open(where->first);
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
    ALEMBIC_ASSIGN(auto conn, Connection::open(where.first));
    return conn->list(where.second);
}

namespace {

// Makes each missing folder above `path` on the server (as mkdir -p does).
// Failures are ignored: an existing folder fails MKD too, and the command
// that follows reports a real problem.
void make_parents(Connection& conn, const std::string& path) {
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
        (void)conn.make_directory(path.substr(0, slash));
}

} // namespace

Status Servers::make_directory(const std::string& path) {
    ALEMBIC_ASSIGN(auto where, split(path));
    ALEMBIC_ASSIGN(auto conn, Connection::open(where.first));
    make_parents(*conn, where.second);
    return conn->make_directory(where.second);
}

bool Servers::exists(const std::string& path) {
    auto where = split(path);
    if (!where.ok()) return false;
    auto conn = Connection::open(where->first);
    if (!conn.ok()) return false;
    if ((*conn)->size(where->second).ok()) return true;
    // A folder: its parent lists it.
    std::size_t slash = where->second.rfind('/');
    std::string parent = slash == 0 ? "/" : where->second.substr(0, slash), name = where->second.substr(slash + 1);
    auto entries = (*conn)->list(parent);
    if (!entries.ok()) return false;
    for (const Entry& e : *entries)
        if (e.name == name) return true;
    return false;
}

Result<std::unique_ptr<convert::OutputStream>> Servers::create(const std::string& path, bool overwrite) {
    ALEMBIC_ASSIGN(auto where, split(path));
    if (!overwrite && exists(path)) return make_error(Errc::invalid_argument, "the output file already exists: " + path);
    ALEMBIC_ASSIGN(auto conn, Connection::open(where.first));
    std::string temp = where.second + ".alembic-part";
    make_parents(*conn, where.second);
    ALEMBIC_TRY(conn->start_upload(temp));
    return std::unique_ptr<convert::OutputStream>(new Upload(std::move(conn), temp, where.second, overwrite));
}

} // namespace alembic::engine::ftp

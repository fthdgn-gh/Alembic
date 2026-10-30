// SPDX-License-Identifier: GPL-3.0-or-later
#include "ftp.hpp"

#include "net.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace alembic::engine::ftp {

namespace {

// timegm: a UTC broken-down time to Unix seconds (Windows calls it _mkgmtime).
std::time_t portable_timegm(std::tm* t) {
#ifdef _WIN32
    return ::_mkgmtime(t);
#else
    return ::timegm(t);
#endif
}

Error net_error(const std::string& what) { return make_error(Errc::io, "FTP: " + what); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

Result<net::Socket> connect_to(const std::string& host, std::uint16_t port, int timeout_ms, std::string* peer = nullptr) {
    return netfs::tcp_connect(host, port, timeout_ms, "FTP", peer);
}

Status send_all(net::Socket fd, const char* data, std::size_t size) { return netfs::send_all(fd, data, size, "FTP"); }

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
            e.mtime = static_cast<std::int64_t>(portable_timegm(&tm));
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
    if (data_ != net::kNone) net::close(data_);
    if (control_ != net::kNone) {
        (void)send_all(control_, "QUIT\r\n", 6);
        net::close(control_);
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
            long n = net::recv(control_, chunk, sizeof chunk);
            if (n < 0 && net::should_retry()) continue;
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

Result<std::unique_ptr<Connection>> Connection::open(const netfs::Server& server, int timeout_ms) {
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
        net::close(data_);
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
        net::close(data_);
        data_ = -1;
        return make_error(Errc::invalid_argument, "FTP: cannot list " + directory + ": " + r.text);
    }
    std::string text;
    char chunk[16384];
    for (;;) {
        long n = net::recv(data_, chunk, sizeof chunk);
        if (n < 0 && net::should_retry()) continue;
        if (n < 0) return net_error("listing interrupted");
        if (n == 0) break;
        text.append(chunk, static_cast<std::size_t>(n));
    }
    net::close(data_);
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
        net::close(data_);
        data_ = -1;
        return make_error(Errc::io, "FTP: cannot read " + path + ": " + r.text);
    }
    return {};
}

Result<std::size_t> Connection::read(MutableBytes out) {
    if (data_ < 0 || data_ended_) return std::size_t{0};
    for (;;) {
        long n = net::recv(data_, out.data(), out.size());
        if (n < 0 && net::should_retry()) continue;
        if (n < 0) return net_error("download interrupted");
        if (n == 0) {
            data_ended_ = true;
            net::close(data_);
            data_ = -1;
            ALEMBIC_ASSIGN(Reply done, reply());
            if (done.code != 226 && done.code != 250) return net_error("download failed: " + done.text);
        }
        return static_cast<std::size_t>(n);
    }
}

Status Connection::stop_download() {
    if (data_ < 0) return {};
    // Dropping the data connection ends the transfer. The server then sends
    // its one final reply for it: 426, or 226 if it had already sent
    // everything. Nothing else is sent until that reply is read: servers
    // answer other commands (ABOR, NOOP) during a transfer, in either order
    // with that reply, which put every later reply one command behind.
    net::close(data_);
    data_ = -1;
    ALEMBIC_ASSIGN(Reply r, reply()); // fails after the receive timeout
    (void)r;
    return {};
}

Status Connection::start_upload(const std::string& path) {
    ALEMBIC_TRY(open_data());
    ALEMBIC_ASSIGN(Reply r, command("STOR " + path));
    if (r.code != 150 && r.code != 125) {
        net::close(data_);
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
    net::close(data_);
    data_ = -1;
    ALEMBIC_ASSIGN(Reply done, reply());
    if (done.code != 226 && done.code != 250) return net_error("upload failed: " + done.text);
    return {};
}

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server) {
    ALEMBIC_ASSIGN(auto c, Connection::open(server));
    return std::unique_ptr<netfs::Session>(std::move(c));
}

} // namespace alembic::engine::ftp

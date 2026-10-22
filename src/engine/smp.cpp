// SPDX-License-Identifier: GPL-3.0-or-later
#include "smp.hpp"

#include "alembic/io.hpp"
#include "net.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace alembic::engine::smp {

namespace {

std::string trim(std::string s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

bool parse_port(const std::string& text, std::uint16_t& out) {
    unsigned value = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec != std::errc() || r.ptr != text.data() + text.size() || value == 0 || value > 65535) return false;
    out = static_cast<std::uint16_t>(value);
    return true;
}

Error io_error(const std::string& what) { return make_error(Errc::io, "ShadowMount+ API: " + what); }

// libmicrohttpd may answer with chunked transfer encoding.
std::string dechunk(const std::string& body) {
    std::string out;
    std::size_t at = 0;
    while (at < body.size()) {
        std::size_t eol = body.find("\r\n", at);
        if (eol == std::string::npos) break;
        unsigned long size = std::strtoul(body.substr(at, eol - at).c_str(), nullptr, 16);
        at = eol + 2;
        if (size == 0 || at + size > body.size()) break;
        out.append(body, at, size);
        at += size + 2;
    }
    return out;
}

} // namespace

Endpoint read_endpoint(const std::string& config_ini) {
    Endpoint e;
    std::FILE* f = alembic::open_stdio(config_ini, "r");
    if (!f) return e;
    e.enabled = true; // api_enabled defaults to 1
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        std::size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(s.substr(0, eq))), value = trim(s.substr(eq + 1));
        if (key == "api_enabled") {
            std::string v = lower(value);
            e.enabled = !(v == "0" || v == "false" || v == "no" || v == "off");
        } else if (key == "api_bind_address") {
            // Every interface includes the loopback one.
            e.address = value.empty() || value == "0.0.0.0" ? "127.0.0.1" : value;
        } else if (key == "api_port") {
            std::uint16_t port = 0;
            if (parse_port(value, port)) e.port = port;
        }
    }
    std::fclose(f);
    return e;
}

std::optional<Endpoint> parse_endpoint(const std::string& text) {
    std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) return std::nullopt;
    Endpoint e;
    e.enabled = true;
    e.address = text.substr(0, colon);
    if (!parse_port(text.substr(colon + 1), e.port)) return std::nullopt;
    return e;
}

Result<JsonValue> post(const Endpoint& endpoint, const std::string& path, const JsonValue& body, int timeout_ms) {
    if (!endpoint.enabled) return make_error(Errc::unsupported, "ShadowMount+ API is not enabled");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    if (::inet_pton(AF_INET, endpoint.address.c_str(), &addr.sin_addr) != 1)
        return io_error("bad address " + endpoint.address);
    if (!net::startup()) return io_error("cannot start the network (Winsock)");
    std::string why;
    net::Socket fd = net::connect(addr, timeout_ms, true, &why);
    if (fd == net::kNone) return io_error(why == "connection timed out" ? why : "not reachable (" + why + ")");
    struct Closer {
        net::Socket fd;
        ~Closer() { net::close(fd); }
    } closer{fd};

    std::string payload = body.dump();
    std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + endpoint.address + ":" +
                          std::to_string(endpoint.port) +
                          "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(payload.size()) + "\r\n\r\n" + payload;
    for (std::size_t sent = 0; sent < request.size();) {
        if (!net::wait(fd, POLLOUT, timeout_ms)) return io_error("send timed out");
        long n = net::send(fd, request.data() + sent, request.size() - sent);
        if (n < 0 && net::should_retry()) continue;
        if (n <= 0) return io_error("send: " + net::error_text());
        sent += static_cast<std::size_t>(n);
    }
    std::string reply;
    char buffer[16384];
    for (;;) {
        if (!net::wait(fd, POLLIN, timeout_ms)) return io_error("no reply");
        long n = net::recv(fd, buffer, sizeof buffer);
        if (n < 0 && net::should_retry()) continue;
        if (n < 0) return io_error("recv: " + net::error_text());
        if (n == 0) break;
        reply.append(buffer, static_cast<std::size_t>(n));
        if (reply.size() > (16u << 20)) return io_error("reply too large");
    }
    std::size_t split = reply.find("\r\n\r\n");
    if (split == std::string::npos || reply.compare(0, 5, "HTTP/") != 0) return io_error("malformed reply");
    std::string head = lower(reply.substr(0, split)), content = reply.substr(split + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) content = dechunk(content);
    auto json = parse_json(content);
    if (!json.ok()) return io_error("reply is not JSON");
    if ((*json)["status"].integer(0) != 0) {
        std::string why = (*json)["error"].str();
        return make_error(Errc::io, "ShadowMount+: " + (why.empty() ? "request failed" : why));
    }
    return std::move(*json);
}

Result<std::optional<std::string>> game_source(const Endpoint& endpoint, const std::string& title_id) {
    JsonValue body = JsonValue::object();
    body.set("include_size", false);
    ALEMBIC_ASSIGN(JsonValue reply, post(endpoint, "/api/v1/games", body));
    for (const JsonValue& game : reply["games"].items()) {
        if (game["title_id"].str() != title_id) continue;
        if (game["installed_pkg"].boolean() || game["source_type"].str() == "pkg") return std::optional<std::string>();
        std::string path = game["path"].str();
        if (path.empty()) return std::optional<std::string>();
        return std::optional<std::string>(path);
    }
    return std::optional<std::string>();
}

Status rescan(const Endpoint& endpoint, bool reset_attempts) {
    JsonValue body = JsonValue::object();
    body.set("reset_attempts", reset_attempts);
    auto reply = post(endpoint, "/api/v1/scan", body);
    if (!reply.ok()) return reply.error();
    return {};
}

std::string hidden_path(const std::string& path) {
    std::size_t slash = path.rfind('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (!name.empty() && name[0] == '.') return dir + name.substr(1);
    return dir + "." + name;
}

bool is_hidden(const std::string& path) {
    std::size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return name.size() > 1 && name[0] == '.' && name != "..";
}

} // namespace alembic::engine::smp

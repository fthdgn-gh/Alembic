// SPDX-License-Identifier: GPL-3.0-or-later
#include "http.hpp"

#include "net.hpp"

#include <chrono>
#include <cstring>
#include <string_view>

namespace alembic::engine {

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 5 * 1024 * 1024; // a 4 MiB write from another engine, and some
constexpr int kMaxConnections = 32;

const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

bool send_all(net::Socket fd, const char* data, std::size_t size) {
    while (size) {
        long n = net::send(fd, data, size);
        if (n < 0 && net::should_retry()) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

void write_response(net::Socket fd, const HttpResponse& r) {
    std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    head += "Content-Type: " + r.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    head += "Connection: close\r\nCache-Control: no-store\r\n";
    for (const auto& [k, v] : r.headers) head += k + ": " + v + "\r\n";
    head += "\r\n";
    if (send_all(fd, head.data(), head.size())) send_all(fd, r.body.data(), r.body.size());
}

HttpResponse error_response(int status, const std::string& message) {
    HttpResponse r;
    r.status = status;
    r.body = "{\"error\":\"" + message + "\"}";
    return r;
}

} // namespace

std::string url_decode(std::string_view text, bool plus_is_space) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '+' && plus_is_space) {
            out += ' ';
        } else if (c == '%') {
            auto hex = [](char h) {
                return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
            };
            int hi = i + 1 < text.size() ? hex(text[i + 1]) : -1;
            int lo = i + 2 < text.size() ? hex(text[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                out += c;
                continue;
            }
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

HttpServer::~HttpServer() { stop(); }

Status HttpServer::start(const std::string& address, std::uint16_t port, HttpHandler handler) {
    handler_ = std::move(handler);
    if (!net::startup()) return make_error(Errc::io, "cannot start the network (Winsock)");
    net::Socket fd = net::tcp_socket();
    if (fd == net::kNone) return make_error(Errc::io, "socket: " + net::error_text());
    net::set_reuse_address(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        net::close(fd);
        return make_error(Errc::invalid_argument, "bad listen address " + address);
    }
    if (!net::bind(fd, addr)) {
        std::string e = net::error_text();
        net::close(fd);
        return make_error(Errc::io, "cannot listen on port " + std::to_string(port) + ": " + e);
    }
    if (!net::listen(fd, 16)) {
        std::string e = net::error_text();
        net::close(fd);
        return make_error(Errc::io, "listen: " + e);
    }
    port_ = net::local_port(fd);
    listen_fd_ = fd;
    stopping_ = false;
    thread_ = std::thread([this] { accept_loop(); });
    return {};
}

void HttpServer::stop() {
    if (listen_fd_ == net::kNone) return;
    stopping_ = true;
    net::shutdown(listen_fd_);
    net::close(listen_fd_);
    listen_fd_ = net::kNone;
    if (thread_.joinable()) thread_.join();
    // Connection threads are detached; wait briefly for them to finish.
    for (int i = 0; i < 200 && active_ > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void HttpServer::accept_loop() {
    while (!stopping_) {
        sockaddr_in peer{};
        net::Socket fd = net::accept(listen_fd_, &peer);
        if (fd == net::kNone) {
            if (net::should_retry()) continue;
            if (stopping_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        char ip[INET_ADDRSTRLEN] = "";
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        if (active_ >= kMaxConnections) {
            write_response(fd, error_response(503, "busy"));
            net::close(fd);
            continue;
        }
        ++active_;
        std::thread([this, fd, p = std::string(ip)] {
            serve(fd, p);
            net::close(fd);
            --active_;
        }).detach();
    }
}

void HttpServer::serve(net::Socket fd, std::string peer) {
    net::set_timeouts(fd, 10);

    std::string data;
    std::size_t header_end = std::string::npos;
    char buf[4096];
    while (header_end == std::string::npos) {
        long n = net::recv(fd, buf, sizeof buf);
        if (n < 0 && net::should_retry()) continue;
        if (n <= 0) return;
        data.append(buf, static_cast<std::size_t>(n));
        header_end = data.find("\r\n\r\n");
        if (header_end == std::string::npos && data.size() > kMaxHeaderBytes)
            return write_response(fd, error_response(413, "headers too large"));
    }

    HttpRequest req;
    req.peer = std::move(peer);
    std::size_t line_end = data.find("\r\n");
    std::string request_line = data.substr(0, line_end);
    std::size_t sp1 = request_line.find(' ');
    std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return write_response(fd, error_response(400, "bad request line"));
    req.method = request_line.substr(0, sp1);
    std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::size_t q = target.find('?');
    req.path = url_decode(target.substr(0, q), false);
    if (q != std::string::npos) {
        std::string query = target.substr(q + 1);
        std::size_t start = 0;
        while (start <= query.size()) {
            std::size_t amp = query.find('&', start);
            if (amp == std::string::npos) amp = query.size();
            std::string pair = query.substr(start, amp - start);
            if (!pair.empty()) {
                std::size_t eq = pair.find('=');
                std::string key = url_decode(pair.substr(0, eq), true);
                req.query[key] = eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1), true);
            }
            start = amp + 1;
        }
    }
    std::size_t pos = line_end + 2;
    while (pos < header_end) {
        std::size_t end = data.find("\r\n", pos);
        std::string line = data.substr(pos, end - pos);
        pos = end + 2;
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    std::size_t length = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
        for (char c : it->second) {
            if (c < '0' || c > '9') return write_response(fd, error_response(400, "bad content-length"));
            length = length * 10 + static_cast<std::size_t>(c - '0');
            if (length > kMaxBodyBytes) return write_response(fd, error_response(413, "body too large"));
        }
    }
    req.body = data.substr(header_end + 4);
    while (req.body.size() < length) {
        long n = net::recv(fd, buf, sizeof buf);
        if (n < 0 && net::should_retry()) continue;
        if (n <= 0) return;
        req.body.append(buf, static_cast<std::size_t>(n));
    }
    req.body.resize(length);
    write_response(fd, handler_(req));
}

} // namespace alembic::engine

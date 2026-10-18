// SPDX-License-Identifier: GPL-3.0-or-later
#include "http.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace alembic::engine {

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
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

bool send_all(int fd, const char* data, std::size_t size) {
    while (size) {
        ssize_t n = ::send(fd, data, size, 0);
        if (n < 0 && errno == EINTR) continue;
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

void write_response(int fd, const HttpResponse& r) {
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
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return make_error(Errc::io, std::string("socket: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return make_error(Errc::invalid_argument, "bad listen address " + address);
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        int e = errno;
        ::close(fd);
        return make_error(Errc::io, "cannot listen on port " + std::to_string(port) + ": " + std::strerror(e));
    }
    if (::listen(fd, 16) != 0) {
        int e = errno;
        ::close(fd);
        return make_error(Errc::io, std::string("listen: ") + std::strerror(e));
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
    listen_fd_ = fd;
    stopping_ = false;
    thread_ = std::thread([this] { accept_loop(); });
    return {};
}

void HttpServer::stop() {
    if (listen_fd_ < 0) return;
    stopping_ = true;
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
    if (thread_.joinable()) thread_.join();
    // Connection threads are detached; wait briefly for them to finish.
    for (int i = 0; i < 200 && active_ > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void HttpServer::accept_loop() {
    while (!stopping_) {
        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (stopping_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        char ip[INET_ADDRSTRLEN] = "";
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        if (active_ >= kMaxConnections) {
            write_response(fd, error_response(503, "busy"));
            ::close(fd);
            continue;
        }
        ++active_;
        std::thread([this, fd, p = std::string(ip)] {
            serve(fd, p);
            ::close(fd);
            --active_;
        }).detach();
    }
}

void HttpServer::serve(int fd, std::string peer) {
    timeval timeout{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    std::string data;
    std::size_t header_end = std::string::npos;
    char buf[4096];
    while (header_end == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
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
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        req.body.append(buf, static_cast<std::size_t>(n));
    }
    req.body.resize(length);
    write_response(fd, handler_(req));
}

} // namespace alembic::engine

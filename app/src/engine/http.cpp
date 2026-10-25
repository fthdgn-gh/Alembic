// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/http.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace alembic::app {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A connected TCP socket, or -1 with the reason in *error. The connect blocks:
// the app talks to an engine on the same machine, which answers or refuses
// at once (fcntl, for a non-blocking connect, is one more import a native
// title may not have).
int open_socket(const std::string& host, std::uint16_t port, int timeout_ms, std::string* error) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        *error = "bad address " + host;
        return -1;
    }
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        *error = std::strerror(errno);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
        *error = std::strerror(errno);
        ::close(s);
        return -1;
    }
    if (rc != 0) {
        pollfd p{s, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) {
            *error = "time-out";
            ::close(s);
            return -1;
        }
    }
    return s;
}

bool send_all(int s, const char* data, std::size_t size, int timeout_ms) {
    while (size > 0) {
        pollfd p{s, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return false;
        const long n = ::send(s, data, size, kSendFlags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

std::string lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

}  // namespace

namespace {
std::string& user_agent() {
    static std::string agent = "Alembic-app";
    return agent;
}
}  // namespace

void set_user_agent(const std::string& agent) { user_agent() = agent; }

HttpReply http_request(const std::string& host, std::uint16_t port, const std::string& method,
                       const std::string& path, const std::string& body, int timeout_ms, std::size_t max_body) {
    HttpReply reply;
    const int s = open_socket(host, port, timeout_ms, &reply.body);
    if (s < 0) return reply;
    std::string head = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                       "\r\nConnection: close\r\nUser-Agent: " + user_agent() + "\r\n";
    if (!body.empty() || method == "POST" || method == "PUT")
        head += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    head += "\r\n";
    if (!send_all(s, head.data(), head.size(), timeout_ms) || !send_all(s, body.data(), body.size(), timeout_ms)) {
        ::close(s);
        reply.body = "send failed";
        return reply;
    }
    std::string data;
    char buffer[16384];
    for (;;) {
        pollfd p{s, POLLIN, 0};
        const int ready = ::poll(&p, 1, timeout_ms);
        if (ready <= 0) {
            ::close(s);
            reply.body = "time-out";
            return reply;
        }
        const long n = ::recv(s, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        data.append(buffer, static_cast<std::size_t>(n));
        if (data.size() > max_body + 65536) break;
    }
    ::close(s);
    const std::size_t end = data.find("\r\n\r\n");
    if (data.compare(0, 5, "HTTP/") != 0 || end == std::string::npos) {
        reply.body = "bad reply";
        return reply;
    }
    const std::size_t sp = data.find(' ');
    const int status = std::atoi(data.c_str() + sp + 1);
    // Headers: Content-Type, and chunked transfer (the engine sends plain
    // bodies, but a proxy or another server might not).
    bool chunked = false;
    std::size_t line = data.find("\r\n") + 2;
    while (line < end) {
        const std::size_t next = data.find("\r\n", line);
        const std::string header = data.substr(line, next - line);
        const std::size_t colon = header.find(':');
        if (colon != std::string::npos) {
            const std::string name = lower(header.substr(0, colon));
            std::string value = header.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            if (name == "content-type") reply.type = value;
            if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked = true;
        }
        line = next + 2;
    }
    std::string payload = data.substr(end + 4);
    if (chunked) {
        std::string plain;
        std::size_t at = 0;
        for (;;) {
            const std::size_t eol = payload.find("\r\n", at);
            if (eol == std::string::npos) break;
            const std::size_t size = std::strtoul(payload.c_str() + at, nullptr, 16);
            if (size == 0 || eol + 2 + size > payload.size()) break;
            plain.append(payload, eol + 2, size);
            at = eol + 2 + size + 2;
        }
        payload.swap(plain);
    }
    if (payload.size() > max_body) payload.resize(max_body);
    reply.status = status;
    reply.body = std::move(payload);
    return reply;
}

bool send_bytes(const std::string& host, std::uint16_t port, const std::string& data, std::string* error) {
    const int s = open_socket(host, port, 3000, error);
    if (s < 0) return false;
    const bool ok = send_all(s, data.data(), data.size(), 10000);
    ::shutdown(s, SHUT_WR);
    ::close(s);
    if (!ok) *error = "send failed";
    return ok;
}

}  // namespace alembic::app

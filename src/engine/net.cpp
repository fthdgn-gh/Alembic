// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.hpp"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <mutex>
#else
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace alembic::engine::net {

#ifdef _WIN32

namespace {
SOCKET raw(Socket s) { return static_cast<SOCKET>(s); }
int last() { return WSAGetLastError(); }
std::string text_of(int code) {
    char* text = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
                             reinterpret_cast<char*>(&text), 0, nullptr);
    std::string out = n ? std::string(text, n) : "socket error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}
} // namespace

bool startup() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
}

Socket tcp_socket() {
    startup();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    return s == INVALID_SOCKET ? kNone : static_cast<Socket>(s);
}
void close(Socket s) {
    if (s != kNone) ::closesocket(raw(s));
}
void shutdown(Socket s) { ::shutdown(raw(s), SD_BOTH); }
long send(Socket s, const void* data, std::size_t size) {
    int n = static_cast<int>(size > (1u << 30) ? (1u << 30) : size);
    return ::send(raw(s), static_cast<const char*>(data), n, 0);
}
long recv(Socket s, void* data, std::size_t size) {
    int n = static_cast<int>(size > (1u << 30) ? (1u << 30) : size);
    return ::recv(raw(s), static_cast<char*>(data), n, 0);
}
bool should_retry() {
    int e = last();
    return e == WSAEINTR || e == WSAEWOULDBLOCK;
}
std::string error_text() { return text_of(last()); }

void set_reuse_address(Socket s) {
    BOOL one = TRUE;
    ::setsockopt(raw(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
}
void set_timeouts(Socket s, int seconds) {
    DWORD ms = static_cast<DWORD>(seconds) * 1000; // Winsock takes milliseconds, not a timeval
    ::setsockopt(raw(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    ::setsockopt(raw(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}
void set_no_delay(Socket s) {
    BOOL one = TRUE;
    ::setsockopt(raw(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}
void set_blocking(Socket s, bool blocking) {
    u_long mode = blocking ? 0 : 1;
    ::ioctlsocket(raw(s), FIONBIO, &mode);
}
bool wait(Socket s, short events, int timeout_ms) {
    WSAPOLLFD p{};
    p.fd = raw(s);
    p.events = events;
    return ::WSAPoll(&p, 1, timeout_ms) > 0;
}
bool bind(Socket s, const sockaddr_in& address) {
    return ::bind(raw(s), reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
}
bool listen(Socket s, int backlog) { return ::listen(raw(s), backlog) == 0; }
Socket accept(Socket s, sockaddr_in* peer) {
    int len = sizeof *peer;
    SOCKET c = ::accept(raw(s), reinterpret_cast<sockaddr*>(peer), &len);
    return c == INVALID_SOCKET ? kNone : static_cast<Socket>(c);
}
std::uint16_t local_port(Socket s) {
    sockaddr_in bound{};
    int len = sizeof bound;
    ::getsockname(raw(s), reinterpret_cast<sockaddr*>(&bound), &len);
    return ntohs(bound.sin_port);
}
Socket connect(const sockaddr_in& address, int timeout_ms, bool nonblocking, std::string* error) {
    Socket s = tcp_socket();
    if (s == kNone) {
        if (error) *error = "socket: " + error_text();
        return kNone;
    }
    set_blocking(s, false);
    if (::connect(raw(s), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        if (last() != WSAEWOULDBLOCK) {
            if (error) *error = error_text();
            close(s);
            return kNone;
        }
        // Winsock reports a refused connection as POLLERR/POLLHUP, not as writable.
        WSAPOLLFD p{};
        p.fd = raw(s);
        p.events = POLLOUT;
        int r = ::WSAPoll(&p, 1, timeout_ms);
        int err = 0;
        int len = sizeof err;
        if (r > 0) ::getsockopt(raw(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (r <= 0 || err != 0 || !(p.revents & POLLOUT)) {
            if (error) *error = r == 0 ? "connection timed out" : text_of(err ? err : WSAECONNREFUSED);
            close(s);
            return kNone;
        }
    }
    if (!nonblocking) set_blocking(s, true);
    return s;
}

#else // POSIX

namespace {
int raw(Socket s) { return static_cast<int>(s); }
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0; // macOS: SO_NOSIGPIPE on the socket instead
#endif
} // namespace

bool startup() { return true; }

Socket tcp_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return kNone;
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}
void close(Socket s) {
    if (s != kNone) ::close(raw(s));
}
void shutdown(Socket s) { ::shutdown(raw(s), SHUT_RDWR); }
long send(Socket s, const void* data, std::size_t size) { return ::send(raw(s), data, size, kSendFlags); }
long recv(Socket s, void* data, std::size_t size) { return ::recv(raw(s), data, size, 0); }
bool should_retry() { return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK; }
std::string error_text() { return std::strerror(errno); }

void set_reuse_address(Socket s) {
    int one = 1;
    ::setsockopt(raw(s), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}
void set_timeouts(Socket s, int seconds) {
    timeval tv{seconds, 0};
    ::setsockopt(raw(s), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(raw(s), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
void set_no_delay(Socket s) {
    int one = 1;
    ::setsockopt(raw(s), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
void set_blocking(Socket s, bool blocking) {
    int flags = ::fcntl(raw(s), F_GETFL, 0);
    ::fcntl(raw(s), F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
}
bool wait(Socket s, short events, int timeout_ms) {
    pollfd p{raw(s), events, 0};
    int r;
    do r = ::poll(&p, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
    return r > 0;
}
bool bind(Socket s, const sockaddr_in& address) {
    return ::bind(raw(s), reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
}
bool listen(Socket s, int backlog) { return ::listen(raw(s), backlog) == 0; }
Socket accept(Socket s, sockaddr_in* peer) {
    socklen_t len = sizeof *peer;
    int c = ::accept(raw(s), reinterpret_cast<sockaddr*>(peer), &len);
    return c < 0 ? kNone : c;
}
std::uint16_t local_port(Socket s) {
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    ::getsockname(raw(s), reinterpret_cast<sockaddr*>(&bound), &len);
    return ntohs(bound.sin_port);
}
Socket connect(const sockaddr_in& address, int timeout_ms, bool nonblocking, std::string* error) {
    Socket s = tcp_socket();
    if (s == kNone) {
        if (error) *error = "socket: " + error_text();
        return kNone;
    }
    set_blocking(s, false);
    if (::connect(raw(s), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        if (errno != EINPROGRESS) {
            if (error) *error = error_text();
            close(s);
            return kNone;
        }
        bool ready = wait(s, POLLOUT, timeout_ms);
        int err = 0;
        socklen_t len = sizeof err;
        if (ready) ::getsockopt(raw(s), SOL_SOCKET, SO_ERROR, &err, &len);
        if (!ready || err != 0) {
            if (error) *error = ready ? std::strerror(err) : "connection timed out";
            close(s);
            return kNone;
        }
    }
    if (!nonblocking) set_blocking(s, true);
    return s;
}

#endif

} // namespace alembic::engine::net

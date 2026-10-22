// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <iphlpapi.h>
#include <mutex>
#include <vector>
#else
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
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

std::string host_name() {
    wchar_t name[256];
    DWORD size = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size)) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), out.data(), n, nullptr, nullptr);
    return out;
}

namespace {
struct Interface {
    std::string ip, broadcast;
};
std::vector<Interface> interfaces() {
    std::vector<Interface> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 3 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            std::uint32_t ip = ntohl(in->sin_addr.s_addr);
            if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue; // loopback, link-local
            unsigned prefix = u->OnLinkPrefixLength;
            std::uint32_t mask = prefix == 0 ? 0 : prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
            in_addr b{};
            b.s_addr = htonl(ip | ~mask);
            char text[INET_ADDRSTRLEN] = {}, btext[INET_ADDRSTRLEN] = {};
            if (!::inet_ntop(AF_INET, &in->sin_addr, text, sizeof text)) continue;
            ::inet_ntop(AF_INET, &b, btext, sizeof btext);
            out.push_back({text, prefix > 0 && prefix < 31 ? btext : ""});
        }
    }
    return out;
}
} // namespace

std::vector<std::string> local_addresses() {
    std::vector<std::string> out;
    for (const Interface& i : interfaces()) out.push_back(i.ip);
    return out;
}

Socket udp_socket(std::uint16_t port) {
    startup();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return kNone;
    BOOL one = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one); // UDP: share the port
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        ::closesocket(s);
        return kNone;
    }
    return static_cast<Socket>(s);
}

bool send_to(Socket s, const std::string& ip, std::uint16_t port, const void* data, std::size_t size) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
    return ::sendto(raw(s), static_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&a),
                    sizeof a) == static_cast<int>(size);
}

long receive_from(Socket s, void* data, std::size_t size, int timeout_ms, std::string* from) {
    if (!wait(s, POLLIN, timeout_ms)) return 0;
    sockaddr_in a{};
    int len = sizeof a;
    int n = ::recvfrom(raw(s), static_cast<char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (n >= 0 && from) {
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &a.sin_addr, text, sizeof text);
        *from = text;
    }
    return n;
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

std::string host_name() {
    char name[256] = {};
    if (::gethostname(name, sizeof name - 1) != 0) return "";
    return name;
}

namespace {
struct Interface {
    std::string ip, broadcast;
};
std::vector<Interface> interfaces() {
    std::vector<Interface> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP)) continue;
        const auto* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        std::uint32_t ip = ntohl(in->sin_addr.s_addr);
        if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue; // loopback, link-local
        char text[INET_ADDRSTRLEN] = {}, btext[INET_ADDRSTRLEN] = {};
        if (!::inet_ntop(AF_INET, &in->sin_addr, text, sizeof text)) continue;
        if ((a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr && a->ifa_broadaddr->sa_family == AF_INET)
            ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(a->ifa_broadaddr)->sin_addr, btext, sizeof btext);
        out.push_back({text, btext});
    }
    ::freeifaddrs(list);
    return out;
}
} // namespace

std::vector<std::string> local_addresses() {
    std::vector<std::string> out;
    for (const Interface& i : interfaces())
        if (std::find(out.begin(), out.end(), i.ip) == out.end()) out.push_back(i.ip);
    return out;
}

Socket udp_socket(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return kNone;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one); // several engines on one computer
#endif
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return kNone;
    }
    return fd;
}

bool send_to(Socket s, const std::string& ip, std::uint16_t port, const void* data, std::size_t size) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
    return ::sendto(raw(s), data, size, 0, reinterpret_cast<const sockaddr*>(&a), sizeof a) == static_cast<long>(size);
}

long receive_from(Socket s, void* data, std::size_t size, int timeout_ms, std::string* from) {
    if (!wait(s, POLLIN, timeout_ms)) return 0;
    sockaddr_in a{};
    socklen_t len = sizeof a;
    long n = ::recvfrom(raw(s), data, size, 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (n >= 0 && from) {
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &a.sin_addr, text, sizeof text);
        *from = text;
    }
    return n;
}

#endif


// ------------------------------------------------------------------ shared

std::vector<std::string> broadcast_addresses() {
    std::vector<std::string> out;
    for (const Interface& i : interfaces())
        if (!i.broadcast.empty() && std::find(out.begin(), out.end(), i.broadcast) == out.end()) out.push_back(i.broadcast);
    out.push_back("255.255.255.255");
    return out;
}

Result<HttpReply> http_request(const std::string& host, std::uint16_t port, const std::string& method,
                               const std::string& path, const std::string& body, int timeout_ms, std::size_t max_body,
                               const Headers& headers) {
    if (!startup()) return make_error(Errc::io, "cannot start the network");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found)
            return make_error(Errc::io, "cannot find " + host);
        addr.sin_addr = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr;
        ::freeaddrinfo(found);
    }
    std::string why;
    Socket s = connect(addr, timeout_ms, true, &why);
    if (s == kNone) return make_error(Errc::io, host + ":" + std::to_string(port) + " is not reachable (" + why + ")");
    struct Closer {
        Socket s;
        ~Closer() { close(s); }
    } closer{s};
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                          "\r\nConnection: close\r\n";
    if (!body.empty() || method == "POST" || method == "PUT")
        request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& [name, value] : headers) request += name + ": " + value + "\r\n";
    request += "\r\n" + body;
    for (std::size_t sent = 0; sent < request.size();) {
        if (!wait(s, POLLOUT, timeout_ms)) return make_error(Errc::io, host + ": sending timed out");
        long n = send(s, request.data() + sent, request.size() - sent);
        if (n < 0 && should_retry()) continue;
        if (n <= 0) return make_error(Errc::io, host + ": " + error_text());
        sent += static_cast<std::size_t>(n);
    }
    std::string reply;
    char buffer[16384];
    for (;;) {
        if (!wait(s, POLLIN, timeout_ms)) return make_error(Errc::io, host + ": no reply");
        long n = recv(s, buffer, sizeof buffer);
        if (n < 0 && should_retry()) continue;
        if (n < 0) return make_error(Errc::io, host + ": " + error_text());
        if (n == 0) break;
        reply.append(buffer, static_cast<std::size_t>(n));
        if (reply.size() > max_body + 65536) return make_error(Errc::too_large, host + ": the reply is too large");
    }
    std::size_t split = reply.find("\r\n\r\n");
    if (split == std::string::npos || reply.compare(0, 5, "HTTP/") != 0) return make_error(Errc::invalid_data, host + ": not an HTTP reply");
    HttpReply r;
    std::size_t space = reply.find(' ');
    if (space == std::string::npos || space > split) return make_error(Errc::invalid_data, host + ": not an HTTP reply");
    r.status = std::atoi(reply.c_str() + space + 1);
    r.body = reply.substr(split + 4);
    if (r.body.size() > max_body) return make_error(Errc::too_large, host + ": the reply is too large");
    return r;
}

} // namespace alembic::engine::net

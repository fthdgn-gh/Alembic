// SPDX-License-Identifier: GPL-3.0-or-later
// TCP sockets on POSIX (macOS, Linux, the PS5) and Windows (Winsock): the few
// calls the engine's HTTP server, FTP client and ShadowMount+ client need,
// with the differences (socket type, error codes, time-out options, closing)
// kept here.
#pragma once

#include "alembic/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace alembic::engine::net {

// A socket; kNone when there is none (Windows' INVALID_SOCKET is -1 here too).
using Socket = std::intptr_t;
inline constexpr Socket kNone = -1;

// Winsock must be started once per process before any socket call; elsewhere
// this does nothing. Safe to call more than once.
bool startup();

Socket tcp_socket();
void close(Socket s);
void shutdown(Socket s); // both directions; wakes a thread blocked in accept
// Like send/recv: bytes moved, 0 at the end (recv), < 0 on failure. A
// closed peer never raises SIGPIPE.
long send(Socket s, const void* data, std::size_t size);
long recv(Socket s, void* data, std::size_t size);
// Whether the last failure only asks to try again (interrupted, would block).
bool should_retry();
// The last socket error as text.
std::string error_text();

// SO_REUSEADDR on POSIX; on Windows, where that would let another program
// take the port, SO_EXCLUSIVEADDRUSE.
void set_reuse_address(Socket s);
void set_timeouts(Socket s, int seconds); // receive and send
void set_no_delay(Socket s);
void set_blocking(Socket s, bool blocking);
// Waits up to `timeout_ms` for POLLIN or POLLOUT; true when ready.
bool wait(Socket s, short events, int timeout_ms);

bool bind(Socket s, const sockaddr_in& address);
bool listen(Socket s, int backlog);
Socket accept(Socket s, sockaddr_in* peer);
// The port a socket was bound to.
std::uint16_t local_port(Socket s);
// Connects with a time-out; the socket comes back blocking unless
// `nonblocking`. kNone on failure, with the reason in `error`.
Socket connect(const sockaddr_in& address, int timeout_ms, bool nonblocking, std::string* error);

// This computer's name ("" when unknown).
std::string host_name();
// Its IPv4 addresses that other devices can reach: not loopback (127.x) or
// link-local (169.254.x); best effort, empty when they cannot be read.
std::vector<std::string> local_addresses();
// Where a broadcast reaches each of those networks ("192.168.1.255"), then
// 255.255.255.255 (which some systems send on one interface only).
std::vector<std::string> broadcast_addresses();

// UDP: a socket bound to every interface on `port`, allowed to broadcast and
// to share the port with other programs (several engines on one computer).
Socket udp_socket(std::uint16_t port);
bool send_to(Socket s, const std::string& ip, std::uint16_t port, const void* data, std::size_t size);
// Waits up to `timeout_ms` for one datagram: its size, 0 on a time-out, < 0
// on failure; `from` gets the sender's address.
long receive_from(Socket s, void* data, std::size_t size, int timeout_ms, std::string* from);

// A plain HTTP/1.1 request ("Connection: close") with a time-out on each
// step; the reply body is at most `max_body` bytes.
struct HttpReply {
    int status = 0;
    std::string body;
};
using Headers = std::vector<std::pair<std::string, std::string>>;
Result<HttpReply> http_request(const std::string& host, std::uint16_t port, const std::string& method,
                               const std::string& path, const std::string& body = "", int timeout_ms = 3000,
                               std::size_t max_body = 4u << 20, const Headers& headers = {});

} // namespace alembic::engine::net

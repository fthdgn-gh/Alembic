// SPDX-License-Identifier: GPL-3.0-or-later
// TCP sockets on POSIX (macOS, Linux, the PS5) and Windows (Winsock): the few
// calls the engine's HTTP server, FTP client and ShadowMount+ client need,
// with the differences (socket type, error codes, time-out options, closing)
// kept here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

} // namespace alembic::engine::net

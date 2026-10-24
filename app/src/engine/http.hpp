// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal HTTP/1.1 client for talking to the engine on this console (or a
// PC during previews). Only socket, connect, poll, send, recv and close, which
// a native title has; the engine's own client (src/engine/net.cpp) uses more.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace alembic::app {

struct HttpReply {
    int status = 0;    // 0: no reply (could not connect, time-out, ...)
    std::string body;  // the reply body, or why there is no reply
    std::string type;  // Content-Type
};

// One request with "Connection: close"; host is an IPv4 address.
HttpReply http_request(const std::string& host, std::uint16_t port, const std::string& method,
                       const std::string& path, const std::string& body = "", int timeout_ms = 5000,
                       std::size_t max_body = 8u << 20);

// Sends raw bytes to host:port and closes (the ELF loader's protocol).
bool send_bytes(const std::string& host, std::uint16_t port, const std::string& data, std::string* error);

}  // namespace alembic::app

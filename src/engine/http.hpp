// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal HTTP/1.1 server for the engine API: one request per connection
// (Connection: close), small bodies, no TLS. Plain POSIX sockets, so it runs
// the same on the PS5 and on a PC.
#pragma once

#include "alembic/result.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace alembic::engine {

struct HttpRequest {
    std::string method;
    std::string path; // decoded, without the query
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
    std::string peer; // client IP
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::map<std::string, std::string> headers;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    HttpServer() = default;
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer();

    // Binds `address:port` ("0.0.0.0" for every interface) and starts the
    // accept thread. Fails when the port is taken.
    Status start(const std::string& address, std::uint16_t port, HttpHandler handler);
    void stop();
    std::uint16_t port() const { return port_; }

private:
    void accept_loop();
    void serve(int fd, std::string peer);

    HttpHandler handler_;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> active_{0};
    std::thread thread_;
};

// %XX and '+' decoding for paths and query strings.
std::string url_decode(std::string_view text, bool plus_is_space);

} // namespace alembic::engine

// SPDX-License-Identifier: GPL-3.0-or-later
// TLS for the network-server clients (FTPS, HTTPS WebDAV): a connection that
// starts plain and can switch to TLS (mbedTLS 3.6, third_party/mbedtls), on
// every platform the engine runs on.
//
// Trust is pinned on first use, like SSH: home servers mostly have
// self-signed certificates, so instead of certificate authorities the
// server's certificate is recognised by the SHA-256 of its DER bytes. The
// first connection is refused with a "not trusted yet" error that carries
// the fingerprint; once the user confirms it, the fingerprint is kept with
// the server and later connections must show the same certificate. TLS
// itself still proves the server holds that certificate's private key.
#pragma once

#include "alembic/result.hpp"
#include "net.hpp"

#include <memory>
#include <optional>
#include <string>

namespace alembic::engine::tls {

// One connection, plain or (after start_tls) TLS. Move-only; closes itself.
class Link {
public:
    Link() = default;
    explicit Link(net::Socket socket) : socket_(socket) {}
    Link(Link&& other) noexcept;
    Link& operator=(Link&& other) noexcept;
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    ~Link();

    bool is_open() const { return socket_ != net::kNone; }
    bool secure() const { return tls_ != nullptr; }
    net::Socket socket() const { return socket_; }

    // The TLS handshake on the open socket. `host` is sent as the server
    // name (not for an IP address); `pinned` is the trusted fingerprint
    // ("" when none yet). `resume_from` (FTPS data connections) offers that
    // connection's TLS session, which many FTPS servers insist on.
    Status start_tls(const std::string& host, const std::string& pinned, const Link* resume_from = nullptr);
    // The server certificate's fingerprint, once the handshake ran.
    const std::string& fingerprint() const { return fingerprint_; }

    Status send_all(const void* data, std::size_t size, const char* label);
    // Like net::recv: bytes read, 0 when the other side closed, < 0 on error.
    long recv(void* data, std::size_t size);
    // Ends TLS politely (close_notify), then closes the socket.
    void close();

private:
    struct Tls;
    struct TlsDeleter {
        void operator()(Tls* t) const;
    };
    net::Socket socket_ = net::kNone;
    std::unique_ptr<Tls, TlsDeleter> tls_;
    std::string fingerprint_;
};

// "AB:CD:...": SHA-256 of DER bytes, as start_tls compares and shows them.
std::string fingerprint_of(const unsigned char* der, std::size_t size);

// The fingerprint a "not trusted yet" or "changed" error carries, if `error`
// is one: what the page shows to ask whether to trust the server.
std::optional<std::string> untrusted_fingerprint(const Error& error);

} // namespace alembic::engine::tls

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tls.hpp"

#include "alembic/crypto.hpp"
#include "platform.hpp"

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <cstdio>
#include <cstring>
#include <mutex>

// mbedTLS's random bytes (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG): the system's.
extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t*, std::uint8_t* output,
                                                        std::size_t output_size, std::size_t* output_length) {
    if (!alembic::engine::platform::secure_random(output, output_size).ok()) return PSA_ERROR_INSUFFICIENT_ENTROPY;
    *output_length = output_size;
    return PSA_SUCCESS;
}

namespace alembic::engine::tls {

namespace {

const char kUntrusted[] = "the server's certificate is not trusted yet (SHA-256 ";
const char kChanged[] = "the server's certificate changed (SHA-256 ";

int random_for_tls(void*, unsigned char* out, std::size_t size) {
    return platform::secure_random(out, size).ok() ? 0 : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int bio_send(void* ctx, const unsigned char* data, std::size_t size) {
    const net::Socket s = *static_cast<net::Socket*>(ctx);
    const long n = net::send(s, data, size);
    return n >= 0 ? static_cast<int>(n) : MBEDTLS_ERR_NET_SEND_FAILED;
}

int bio_recv(void* ctx, unsigned char* data, std::size_t size) {
    const net::Socket s = *static_cast<net::Socket*>(ctx);
    const long n = net::recv(s, data, size);
    if (n > 0) return static_cast<int>(n);
    return n == 0 ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
}

std::string mbed_error(int code) {
    char text[160];
    mbedtls_strerror(code, text, sizeof text);
    return text;
}

bool is_ip_address(const std::string& host) {
    if (host.find(':') != std::string::npos) return true; // IPv6
    for (char c : host)
        if (!(c == '.' || (c >= '0' && c <= '9'))) return false;
    return !host.empty();
}

// psa_crypto_init once for the process (TLS 1.3 runs on PSA).
Status init_once() {
    static std::once_flag once;
    static psa_status_t status = PSA_ERROR_BAD_STATE;
    std::call_once(once, [] { status = psa_crypto_init(); });
    if (status != PSA_SUCCESS) return make_error(Errc::io, "TLS: the crypto library did not start (" + std::to_string(status) + ")");
    return {};
}

} // namespace

struct Link::Tls {
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    net::Socket socket; // what the I/O callbacks read
    // This connection's session, for others to resume (FTPS data
    // connections): mbedTLS hands each session (TLS 1.2) or ticket (1.3)
    // out only once, so a copy is kept and given to every resumer.
    mbedtls_ssl_session saved;
    bool has_saved = false;
    Tls() {
        mbedtls_ssl_config_init(&config);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_session_init(&saved);
    }
    ~Tls() {
        mbedtls_ssl_session_free(&saved);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
    }
    // Takes the session (or the newest ticket) when there is a new one.
    void keep_session() {
        mbedtls_ssl_session fresh;
        mbedtls_ssl_session_init(&fresh);
        if (mbedtls_ssl_get_session(&ssl, &fresh) != 0) {
            mbedtls_ssl_session_free(&fresh);
            return;
        }
        mbedtls_ssl_session_free(&saved);
        saved = fresh; // takes over what `fresh` owns
        has_saved = true;
    }
};

void Link::TlsDeleter::operator()(Tls* t) const { delete t; }

Link::Link(Link&& other) noexcept
    : socket_(other.socket_), tls_(std::move(other.tls_)), fingerprint_(std::move(other.fingerprint_)) {
    other.socket_ = net::kNone;
    if (tls_) tls_->socket = socket_;
}

Link& Link::operator=(Link&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = other.socket_;
        tls_ = std::move(other.tls_);
        fingerprint_ = std::move(other.fingerprint_);
        other.socket_ = net::kNone;
        if (tls_) tls_->socket = socket_;
    }
    return *this;
}

Link::~Link() { close(); }

Status Link::start_tls(const std::string& host, const std::string& pinned, const Link* resume_from) {
    if (socket_ == net::kNone) return make_error(Errc::io, "TLS: no connection");
    ALEMBIC_TRY(init_once());
    std::unique_ptr<Tls, TlsDeleter> t(new Tls());
    t->socket = socket_;
    int rc = mbedtls_ssl_config_defaults(&t->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) return make_error(Errc::io, "TLS: " + mbed_error(rc));
    // The certificate is checked against the pinned fingerprint below, not
    // against certificate authorities; the handshake still proves the
    // server holds its key.
    mbedtls_ssl_conf_authmode(&t->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&t->config, random_for_tls, nullptr);
    mbedtls_ssl_conf_min_tls_version(&t->config, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    mbedtls_ssl_conf_session_tickets(&t->config, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    // TLS 1.3 tickets come after the handshake; without this mbedTLS drops them.
    mbedtls_ssl_conf_tls13_enable_signal_new_session_tickets(&t->config, MBEDTLS_SSL_TLS1_3_SIGNAL_NEW_SESSION_TICKETS_ENABLED);
#endif
#endif
    if ((rc = mbedtls_ssl_setup(&t->ssl, &t->config)) != 0) return make_error(Errc::io, "TLS: " + mbed_error(rc));
    if (!is_ip_address(host) && (rc = mbedtls_ssl_set_hostname(&t->ssl, host.c_str())) != 0)
        return make_error(Errc::io, "TLS: " + mbed_error(rc));
    if (resume_from && resume_from->tls_ && resume_from->tls_->has_saved)
        (void)mbedtls_ssl_set_session(&t->ssl, &resume_from->tls_->saved); // copies it
    mbedtls_ssl_set_bio(&t->ssl, &t->socket, bio_send, bio_recv, nullptr);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return make_error(Errc::io, "TLS handshake with " + host + " failed: " + mbed_error(rc));
    }
    const mbedtls_x509_crt* cert = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (!cert) return make_error(Errc::io, "TLS: " + host + " sent no certificate");
    fingerprint_ = fingerprint_of(cert->raw.p, cert->raw.len);
    if (pinned.empty()) return make_error(Errc::unsupported, kUntrusted + fingerprint_ + ")");
    if (pinned != fingerprint_)
        return make_error(Errc::unsupported, kChanged + fingerprint_ + "; trusted was " + pinned +
                                                 "): if you did not replace it, someone may be in between");
    t->keep_session(); // TLS 1.2: the session now; 1.3: tickets arrive later (recv)
    tls_ = std::move(t);
    return {};
}

Status Link::send_all(const void* data, std::size_t size, const char* label) {
    const auto* p = static_cast<const unsigned char*>(data);
    while (size > 0) {
        long n;
        if (tls_) {
            const int rc = mbedtls_ssl_write(&tls_->ssl, p, size);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc < 0) return make_error(Errc::io, std::string(label) + ": sending failed (TLS: " + mbed_error(rc) + ")");
            n = rc;
        } else {
            n = net::send(socket_, p, size);
            if (n < 0 && net::should_retry()) continue;
            if (n <= 0) return make_error(Errc::io, std::string(label) + ": sending failed (" + net::error_text() + ")");
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return {};
}

long Link::recv(void* data, std::size_t size) {
    if (!tls_) return net::recv(socket_, data, size);
    for (;;) {
        const int rc = mbedtls_ssl_read(&tls_->ssl, static_cast<unsigned char*>(data), size);
        if (rc > 0) return rc;
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) { // TLS 1.3: kept for resumption
            tls_->keep_session();
            continue;
        }
#endif
        // A server that closes without close_notify (common for FTPS data).
        if (rc == MBEDTLS_ERR_NET_CONN_RESET) return 0;
        return -1;
    }
}

void Link::close() {
    if (tls_) {
        (void)mbedtls_ssl_close_notify(&tls_->ssl);
        tls_.reset();
    }
    if (socket_ != net::kNone) {
        net::close(socket_);
        socket_ = net::kNone;
    }
}

std::string fingerprint_of(const unsigned char* der, std::size_t size) {
    const crypto::Digest d = crypto::sha256(Bytes(der, size));
    std::string out;
    char hex[4];
    for (std::size_t i = 0; i < d.size(); ++i) {
        std::snprintf(hex, sizeof hex, i ? ":%02X" : "%02X", d[i]);
        out += hex;
    }
    return out;
}

std::optional<std::string> untrusted_fingerprint(const Error& error) {
    for (const char* prefix : {kUntrusted, kChanged}) {
        const std::size_t at = error.message.find(prefix);
        if (at == std::string::npos) continue;
        const std::size_t start = at + std::strlen(prefix);
        const std::size_t end = error.message.find_first_of(");", start);
        if (end != std::string::npos) return error.message.substr(start, end - start);
    }
    return std::nullopt;
}

} // namespace alembic::engine::tls

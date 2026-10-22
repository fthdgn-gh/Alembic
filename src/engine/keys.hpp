// SPDX-License-Identifier: GPL-3.0-or-later
// The cryptography pairing needs: X25519 (RFC 7748) key agreement and
// HMAC-SHA256 (RFC 2104). The field arithmetic follows TweetNaCl (public
// domain, Bernstein et al.): small and constant-time, fast enough for a key
// exchange now and then.
#pragma once

#include "alembic/io.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace alembic::engine::keys {

using Key = std::array<std::uint8_t, 32>;

// X25519(scalar, point); the scalar is clamped as RFC 7748 says.
Key x25519(const Key& scalar, const Key& point);
// X25519(scalar, 9): the public key for a private one.
Key x25519_base(const Key& scalar);

Key hmac_sha256(Bytes key, Bytes message);
inline Key hmac_sha256(const Key& key, const std::string& message) {
    return hmac_sha256(Bytes(key.data(), key.size()), Bytes(reinterpret_cast<const std::uint8_t*>(message.data()), message.size()));
}
Key sha256(Bytes data);
inline Key sha256(const std::string& s) { return sha256(Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size())); }

// Compares without stopping at the first difference.
bool equal(Bytes a, Bytes b);

std::string to_hex(Bytes data);
inline std::string to_hex(const Key& k) { return to_hex(Bytes(k.data(), k.size())); }
// Exactly `out.size()` bytes of hex, or false.
bool from_hex(const std::string& hex, MutableBytes out);

} // namespace alembic::engine::keys

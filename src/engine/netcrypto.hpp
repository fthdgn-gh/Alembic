// SPDX-License-Identifier: GPL-3.0-or-later
// The hashes network logins need, written from their standards: MD4
// (RFC 1320, the NT password hash), MD5 (RFC 1321, NTLMv2 and HTTP Digest),
// HMAC-MD5 (RFC 2104), SHA-512 (FIPS 180-4, SMB 3.1.1's pre-authentication
// hash) and AES-128-CMAC (RFC 4493, SMB 3 signing), plus the two ciphers
// SMB 3 encryption uses: AES-128-GCM (NIST SP 800-38D) and AES-128-CCM
// (SP 800-38C). They protect traffic only, never anything stored.
#pragma once

#include "alembic/io.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace alembic::engine::netcrypto {

using Md = std::array<std::uint8_t, 16>;
using Sha512Digest = std::array<std::uint8_t, 64>;

Md md4(Bytes data);

class Md5 {
public:
    void update(Bytes data);
    Md finish();

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::uint8_t buffer_[64] = {};
    std::uint64_t length_ = 0;
};
Md md5(Bytes data);
Md hmac_md5(Bytes key, Bytes message);
std::string hex(Bytes data); // lower case

class Sha512 {
public:
    Sha512();
    void update(Bytes data);
    Sha512Digest finish();

private:
    void block(const std::uint8_t* p);
    std::uint64_t h_[8];
    std::uint8_t buffer_[128] = {};
    std::uint64_t length_ = 0;
};
Sha512Digest sha512(Bytes data);

// AES-128-CMAC (RFC 4493) of `message`.
Md aes_cmac(const std::uint8_t key[16], Bytes message);

// AES-128-GCM with a 96-bit nonce and a 16-byte tag, `data` in place.
// Decrypting checks the tag first and leaves `data` untouched when it fails.
void aes_gcm_encrypt(const std::uint8_t key[16], const std::uint8_t nonce[12], Bytes aad, MutableBytes data,
                     std::uint8_t tag[16]);
bool aes_gcm_decrypt(const std::uint8_t key[16], const std::uint8_t nonce[12], Bytes aad, MutableBytes data,
                     const std::uint8_t tag[16]);

// AES-128-CCM with a 7..13-byte nonce and a 4..16-byte (even) tag, `data` in
// place; the same tag rule as GCM. (SMB 3: 11-byte nonce, 16-byte tag.)
void aes_ccm_encrypt(const std::uint8_t key[16], Bytes nonce, Bytes aad, MutableBytes data, MutableBytes tag);
bool aes_ccm_decrypt(const std::uint8_t key[16], Bytes nonce, Bytes aad, MutableBytes data, Bytes tag);

} // namespace alembic::engine::netcrypto

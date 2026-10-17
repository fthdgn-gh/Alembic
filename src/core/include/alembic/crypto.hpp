// SPDX-License-Identifier: GPL-3.0-or-later
// The crypto primitives FPKG needs, written from the standards: AES-128
// (FIPS-197) with XTS (IEEE 1619) and CBC modes, SHA3-256 (FIPS-202) and
// HMAC-SHA256 (RFC 2104). No external crypto library, so the core builds
// unchanged for the PS5.
#pragma once

#include "alembic/io.hpp"

#include <array>
#include <cstdint>

namespace alembic::crypto {

using Digest = std::array<std::uint8_t, 32>;

class Sha3_256 {
public:
    Sha3_256() = default;
    void update(Bytes data);
    Digest finish(); // the object must not be reused afterwards

private:
    void absorb_block();
    std::uint64_t state_[25] = {};
    std::uint8_t buffer_[136] = {};
    std::size_t fill_ = 0;
};

Digest sha3_256(Bytes data);
Digest sha256(Bytes data);
Digest hmac_sha256(Bytes key, Bytes message);

class Aes128 {
public:
    explicit Aes128(const std::uint8_t key[16]);
    void encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const;
    void decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const;

private:
    std::uint32_t enc_[44];
    std::uint32_t dec_[44];
};

// One XTS data unit, in place. `data.size()` must be a multiple of 16; the
// tweak is the little-endian 128-bit `sector`.
void xts_decrypt(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data);
void xts_encrypt(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data);

// CBC without padding, in place. `data.size()` must be a multiple of 16.
void cbc_decrypt(const Aes128& key, const std::uint8_t iv[16], MutableBytes data);
void cbc_encrypt(const Aes128& key, const std::uint8_t iv[16], MutableBytes data);

} // namespace alembic::crypto

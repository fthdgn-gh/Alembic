// SPDX-License-Identifier: GPL-3.0-or-later
// The crypto primitives FPKG needs, written from the standards: AES-128
// (FIPS-197) with XTS (IEEE 1619) and CBC modes, SHA3-256 (FIPS-202) and
// HMAC-SHA256 (RFC 2104). No external crypto library, so the core builds
// unchanged for the PS5.
#pragma once

#include "alembic/io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace alembic::crypto {

using Digest = std::array<std::uint8_t, 32>;

class Sha3_256 {
public:
    Sha3_256() = default;
    void update(Bytes data);
    Digest finish(); // the object must not be reused afterwards
    // The running state as bytes (for checkpoints), and back.
    static constexpr std::size_t kStateBytes = 25 * 8 + 136 + 8;
    std::array<std::uint8_t, kStateBytes> save() const;
    static Sha3_256 restore(const std::uint8_t* state);

private:
    void absorb(const std::uint8_t* block); // one 136-byte rate block
    void absorb_block();
    std::uint64_t state_[25] = {};
    std::uint8_t buffer_[136] = {};
    std::size_t fill_ = 0;
};

Digest sha3_256(Bytes data);

// Feeds a Sha3_256 on a helper thread, in order, so the caller can read the
// next piece meanwhile. update() copies the data (up to `piece` bytes per
// call); wait() returns once everything handed over is absorbed. Without a
// helper (`background` false) it hashes in place.
class BackgroundSha3 {
public:
    explicit BackgroundSha3(bool background, std::size_t piece = 1 << 20);
    ~BackgroundSha3();
    BackgroundSha3(const BackgroundSha3&) = delete;
    BackgroundSha3& operator=(const BackgroundSha3&) = delete;
    void update(Sha3_256& hash, Bytes data);
    void wait();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
Digest sha256(Bytes data);
Digest hmac_sha256(Bytes key, Bytes message);

class Aes128 {
public:
    explicit Aes128(const std::uint8_t key[16]);
    void encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const;
    void decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const;
    // ECB over `count` 16-byte blocks (in == out is fine): with the CPU's AES
    // instructions when the build targets them, several blocks at a time.
    void encrypt_blocks(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const;
    void decrypt_blocks(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const;
    // The table implementation (the fallback, and the reference in tests).
    void portable_encrypt(const std::uint8_t in[16], std::uint8_t out[16]) const;
    void portable_decrypt(const std::uint8_t in[16], std::uint8_t out[16]) const;
    static bool hardware(); // AES instructions are used (they passed a self-test)

private:
    void hw_encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const;
    void hw_decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const;
    std::uint32_t enc_[44];
    std::uint32_t dec_[44];
    alignas(16) std::uint8_t ek_[176]; // enc_ / dec_ as bytes
    alignas(16) std::uint8_t dk_[176];
};

// One XTS data unit, in place. `data.size()` must be a multiple of 16; the
// tweak is the little-endian 128-bit `sector`.
void xts_decrypt(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data);
void xts_encrypt(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data);

// CRC-32C (Castagnoli), continuing from `crc` (0 to start): with the CPU's
// CRC32 instructions when the build targets them.
std::uint32_t crc32c(Bytes data, std::uint32_t crc = 0);

// CBC without padding, in place. `data.size()` must be a multiple of 16.
void cbc_decrypt(const Aes128& key, const std::uint8_t iv[16], MutableBytes data);
void cbc_encrypt(const Aes128& key, const std::uint8_t iv[16], MutableBytes data);

} // namespace alembic::crypto

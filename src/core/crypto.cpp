// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/crypto.hpp"

#include "alembic/sha256.hpp"

#include <cstring>

namespace alembic::crypto {

// ---------------------------------------------------------------- SHA3-256

namespace {

constexpr std::uint64_t kRoundConstants[24] = {
    0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808Aull, 0x8000000080008000ull,
    0x000000000000808Bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
    0x000000000000008Aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000Aull,
    0x000000008000808Bull, 0x800000000000008Bull, 0x8000000000008089ull, 0x8000000000008003ull,
    0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800Aull, 0x800000008000000Aull,
    0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull,
};
// Rotation offsets and lane order of the combined rho and pi steps.
constexpr int kRho[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
constexpr int kPi[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

inline std::uint64_t rotl64(std::uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

void keccak_f1600(std::uint64_t a[25]) {
    for (int round = 0; round < 24; ++round) {
        std::uint64_t c[5];
        for (int x = 0; x < 5; ++x) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (int x = 0; x < 5; ++x) {
            std::uint64_t d = c[(x + 4) % 5] ^ rotl64(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) a[y + x] ^= d;
        }
        std::uint64_t t = a[1];
        for (int i = 0; i < 24; ++i) {
            std::uint64_t next = a[kPi[i]];
            a[kPi[i]] = rotl64(t, kRho[i]);
            t = next;
        }
        for (int y = 0; y < 25; y += 5) {
            std::uint64_t row[5];
            for (int x = 0; x < 5; ++x) row[x] = a[y + x];
            for (int x = 0; x < 5; ++x) a[y + x] = row[x] ^ (~row[(x + 1) % 5] & row[(x + 2) % 5]);
        }
        a[0] ^= kRoundConstants[round];
    }
}

} // namespace

void Sha3_256::absorb_block() {
    for (int i = 0; i < 17; ++i) {
        std::uint64_t lane = 0;
        for (int b = 7; b >= 0; --b) lane = (lane << 8) | buffer_[i * 8 + b];
        state_[i] ^= lane;
    }
    keccak_f1600(state_);
    fill_ = 0;
}

void Sha3_256::update(Bytes data) {
    for (std::uint8_t byte : data) {
        buffer_[fill_++] = byte;
        if (fill_ == sizeof buffer_) absorb_block();
    }
}

Digest Sha3_256::finish() {
    std::memset(buffer_ + fill_, 0, sizeof buffer_ - fill_);
    buffer_[fill_] ^= 0x06; // SHA3 domain bits, then pad10*1
    buffer_[sizeof buffer_ - 1] ^= 0x80;
    absorb_block();
    Digest digest;
    for (int i = 0; i < 32; ++i) digest[i] = static_cast<std::uint8_t>(state_[i / 8] >> (8 * (i % 8)));
    return digest;
}

Digest sha3_256(Bytes data) {
    Sha3_256 h;
    h.update(data);
    return h.finish();
}

Digest sha256(Bytes data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

Digest hmac_sha256(Bytes key, Bytes message) {
    std::uint8_t block[64] = {};
    if (key.size() > sizeof block) {
        Digest k = sha256(key);
        std::memcpy(block, k.data(), k.size());
    } else {
        std::memcpy(block, key.data(), key.size());
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5C;
    }
    Sha256 inner;
    inner.update(Bytes(ipad, 64));
    inner.update(message);
    Sha256::Digest inner_digest = inner.finish();
    Sha256 outer;
    outer.update(Bytes(opad, 64));
    outer.update(Bytes(inner_digest.data(), inner_digest.size()));
    return outer.finish();
}

// ---------------------------------------------------------------- AES-128

namespace {

constexpr std::uint8_t xtime(std::uint8_t x) {
    return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

constexpr std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

constexpr std::uint32_t ror8(std::uint32_t x) { return (x >> 8) | (x << 24); }

struct Tables {
    std::uint8_t sbox[256] = {};
    std::uint8_t inv_sbox[256] = {};
    std::uint32_t te[4][256] = {};
    std::uint32_t td[4][256] = {};
};

constexpr Tables make_tables() {
    Tables t;
    for (int x = 0; x < 256; ++x) {
        // Multiplicative inverse as x^254, then the affine transform.
        std::uint8_t inv = 1, base = static_cast<std::uint8_t>(x);
        for (int e = 254; e; e >>= 1) {
            if (e & 1) inv = gmul(inv, base);
            base = gmul(base, base);
        }
        if (x == 0) inv = 0;
        std::uint8_t s = inv;
        for (int r = 1; r <= 4; ++r) s ^= static_cast<std::uint8_t>((inv << r) | (inv >> (8 - r)));
        s ^= 0x63;
        t.sbox[x] = s;
        t.inv_sbox[s] = static_cast<std::uint8_t>(x);
    }
    for (int x = 0; x < 256; ++x) {
        std::uint8_t s = t.sbox[x];
        std::uint32_t e = (std::uint32_t(gmul(s, 2)) << 24) | (std::uint32_t(s) << 16) | (std::uint32_t(s) << 8) |
                          gmul(s, 3);
        std::uint8_t si = t.inv_sbox[x];
        std::uint32_t d = (std::uint32_t(gmul(si, 14)) << 24) | (std::uint32_t(gmul(si, 9)) << 16) |
                          (std::uint32_t(gmul(si, 13)) << 8) | gmul(si, 11);
        for (int k = 0; k < 4; ++k) {
            t.te[k][x] = e;
            t.td[k][x] = d;
            e = ror8(e);
            d = ror8(d);
        }
    }
    return t;
}

constexpr Tables kT = make_tables();

inline std::uint32_t load_be(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
inline void store_be(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

} // namespace

Aes128::Aes128(const std::uint8_t key[16]) {
    constexpr std::uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
    for (int i = 0; i < 4; ++i) enc_[i] = load_be(key + 4 * i);
    for (int i = 4; i < 44; ++i) {
        std::uint32_t t = enc_[i - 1];
        if (i % 4 == 0) {
            t = (std::uint32_t(kT.sbox[(t >> 16) & 0xFF]) << 24) | (std::uint32_t(kT.sbox[(t >> 8) & 0xFF]) << 16) |
                (std::uint32_t(kT.sbox[t & 0xFF]) << 8) | kT.sbox[t >> 24];
            t ^= std::uint32_t(kRcon[i / 4 - 1]) << 24;
        }
        enc_[i] = enc_[i - 4] ^ t;
    }
    // Equivalent inverse cipher: round keys in reverse order, InvMixColumns on the middle ones.
    for (int round = 0; round <= 10; ++round)
        for (int c = 0; c < 4; ++c) dec_[round * 4 + c] = enc_[(10 - round) * 4 + c];
    for (int i = 4; i < 40; ++i) {
        std::uint32_t w = dec_[i];
        dec_[i] = kT.td[0][kT.sbox[w >> 24]] ^ kT.td[1][kT.sbox[(w >> 16) & 0xFF]] ^
                  kT.td[2][kT.sbox[(w >> 8) & 0xFF]] ^ kT.td[3][kT.sbox[w & 0xFF]];
    }
}

void Aes128::encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const std::uint32_t* rk = enc_;
    std::uint32_t s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1];
    std::uint32_t s2 = load_be(in + 8) ^ rk[2], s3 = load_be(in + 12) ^ rk[3];
    for (int round = 1; round < 10; ++round) {
        rk += 4;
        std::uint32_t t0 = kT.te[0][s0 >> 24] ^ kT.te[1][(s1 >> 16) & 0xFF] ^ kT.te[2][(s2 >> 8) & 0xFF] ^ kT.te[3][s3 & 0xFF] ^ rk[0];
        std::uint32_t t1 = kT.te[0][s1 >> 24] ^ kT.te[1][(s2 >> 16) & 0xFF] ^ kT.te[2][(s3 >> 8) & 0xFF] ^ kT.te[3][s0 & 0xFF] ^ rk[1];
        std::uint32_t t2 = kT.te[0][s2 >> 24] ^ kT.te[1][(s3 >> 16) & 0xFF] ^ kT.te[2][(s0 >> 8) & 0xFF] ^ kT.te[3][s1 & 0xFF] ^ rk[2];
        std::uint32_t t3 = kT.te[0][s3 >> 24] ^ kT.te[1][(s0 >> 16) & 0xFF] ^ kT.te[2][(s1 >> 8) & 0xFF] ^ kT.te[3][s2 & 0xFF] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    auto last = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t k) {
        return ((std::uint32_t(kT.sbox[a >> 24]) << 24) | (std::uint32_t(kT.sbox[(b >> 16) & 0xFF]) << 16) |
                (std::uint32_t(kT.sbox[(c >> 8) & 0xFF]) << 8) | kT.sbox[d & 0xFF]) ^ k;
    };
    store_be(out, last(s0, s1, s2, s3, rk[0]));
    store_be(out + 4, last(s1, s2, s3, s0, rk[1]));
    store_be(out + 8, last(s2, s3, s0, s1, rk[2]));
    store_be(out + 12, last(s3, s0, s1, s2, rk[3]));
}

void Aes128::decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const std::uint32_t* rk = dec_;
    std::uint32_t s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1];
    std::uint32_t s2 = load_be(in + 8) ^ rk[2], s3 = load_be(in + 12) ^ rk[3];
    for (int round = 1; round < 10; ++round) {
        rk += 4;
        std::uint32_t t0 = kT.td[0][s0 >> 24] ^ kT.td[1][(s3 >> 16) & 0xFF] ^ kT.td[2][(s2 >> 8) & 0xFF] ^ kT.td[3][s1 & 0xFF] ^ rk[0];
        std::uint32_t t1 = kT.td[0][s1 >> 24] ^ kT.td[1][(s0 >> 16) & 0xFF] ^ kT.td[2][(s3 >> 8) & 0xFF] ^ kT.td[3][s2 & 0xFF] ^ rk[1];
        std::uint32_t t2 = kT.td[0][s2 >> 24] ^ kT.td[1][(s1 >> 16) & 0xFF] ^ kT.td[2][(s0 >> 8) & 0xFF] ^ kT.td[3][s3 & 0xFF] ^ rk[2];
        std::uint32_t t3 = kT.td[0][s3 >> 24] ^ kT.td[1][(s2 >> 16) & 0xFF] ^ kT.td[2][(s1 >> 8) & 0xFF] ^ kT.td[3][s0 & 0xFF] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    auto last = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t k) {
        return ((std::uint32_t(kT.inv_sbox[a >> 24]) << 24) | (std::uint32_t(kT.inv_sbox[(b >> 16) & 0xFF]) << 16) |
                (std::uint32_t(kT.inv_sbox[(c >> 8) & 0xFF]) << 8) | kT.inv_sbox[d & 0xFF]) ^ k;
    };
    store_be(out, last(s0, s3, s2, s1, rk[0]));
    store_be(out + 4, last(s1, s0, s3, s2, rk[1]));
    store_be(out + 8, last(s2, s1, s0, s3, rk[2]));
    store_be(out + 12, last(s3, s2, s1, s0, rk[3]));
}

// ---------------------------------------------------------------- modes

namespace {

// Multiplies the little-endian 128-bit tweak by x in GF(2^128).
void tweak_times_x(std::uint8_t t[16]) {
    std::uint8_t carry = 0;
    for (int i = 0; i < 16; ++i) {
        std::uint8_t next = t[i] >> 7;
        t[i] = static_cast<std::uint8_t>((t[i] << 1) | carry);
        carry = next;
    }
    if (carry) t[0] ^= 0x87;
}

void xts(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data, bool decrypt) {
    std::uint8_t tweak[16] = {};
    for (int i = 0; i < 8; ++i) tweak[i] = static_cast<std::uint8_t>(sector >> (8 * i));
    tweak_key.encrypt_block(tweak, tweak);
    std::uint8_t block[16];
    for (std::size_t off = 0; off + 16 <= data.size(); off += 16) {
        std::uint8_t* p = data.data() + off;
        for (int i = 0; i < 16; ++i) block[i] = p[i] ^ tweak[i];
        if (decrypt) data_key.decrypt_block(block, block);
        else data_key.encrypt_block(block, block);
        for (int i = 0; i < 16; ++i) p[i] = block[i] ^ tweak[i];
        tweak_times_x(tweak);
    }
}

} // namespace

void xts_decrypt(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data) {
    xts(data_key, tweak_key, sector, data, true);
}

void xts_encrypt(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data) {
    xts(data_key, tweak_key, sector, data, false);
}

void cbc_decrypt(const Aes128& key, const std::uint8_t iv[16], MutableBytes data) {
    std::uint8_t previous[16], saved[16];
    std::memcpy(previous, iv, 16);
    for (std::size_t off = 0; off + 16 <= data.size(); off += 16) {
        std::uint8_t* p = data.data() + off;
        std::memcpy(saved, p, 16);
        key.decrypt_block(p, p);
        for (int i = 0; i < 16; ++i) p[i] ^= previous[i];
        std::memcpy(previous, saved, 16);
    }
}

void cbc_encrypt(const Aes128& key, const std::uint8_t iv[16], MutableBytes data) {
    const std::uint8_t* previous = iv;
    for (std::size_t off = 0; off + 16 <= data.size(); off += 16) {
        std::uint8_t* p = data.data() + off;
        for (int i = 0; i < 16; ++i) p[i] ^= previous[i];
        key.encrypt_block(p, p);
        previous = p;
    }
}

} // namespace alembic::crypto

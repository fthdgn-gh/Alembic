// SPDX-License-Identifier: GPL-3.0-or-later
#include "netcrypto.hpp"

#include "alembic/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace alembic::engine::netcrypto {

namespace {

std::uint32_t rol(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
std::uint64_t ror64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// The 64-byte block padding MD4 and MD5 share: 0x80, zeros, the bit length (LE).
std::vector<std::uint8_t> md_padded(Bytes data) {
    std::vector<std::uint8_t> m(data.begin(), data.end());
    std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 0; i < 8; ++i) m.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    return m;
}

Md md_out(const std::uint32_t h[4]) {
    Md out;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (8 * j));
    return out;
}

} // namespace

Md md4(Bytes data) {
    std::uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::vector<std::uint8_t> m = md_padded(data);
    for (std::size_t at = 0; at < m.size(); at += 64) {
        std::uint32_t x[16];
        for (int i = 0; i < 16; ++i) x[i] = le32(&m[at + 4 * i]);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        auto f = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) | (~x & z); };
        auto g = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) | (x & z) | (y & z); };
        auto hh = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return x ^ y ^ z; };
        static const int s1[4] = {3, 7, 11, 19}, s2[4] = {3, 5, 9, 13}, s3[4] = {3, 9, 11, 15};
        for (int i = 0; i < 16; ++i) {
            std::uint32_t t = rol(a + f(b, c, d) + x[i], s1[i % 4]);
            a = d; d = c; c = b; b = t;
        }
        static const int o2[16] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
        for (int i = 0; i < 16; ++i) {
            std::uint32_t t = rol(a + g(b, c, d) + x[o2[i]] + 0x5a827999u, s2[i % 4]);
            a = d; d = c; c = b; b = t;
        }
        static const int o3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        for (int i = 0; i < 16; ++i) {
            std::uint32_t t = rol(a + hh(b, c, d) + x[o3[i]] + 0x6ed9eba1u, s3[i % 4]);
            a = d; d = c; c = b; b = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    return md_out(h);
}

// ---------------------------------------------------------------- MD5

void Md5::block(const std::uint8_t* p) {
    static const std::uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = le32(p + 4 * i);
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        std::uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + k[i] + m[g], s[i]);
        a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
}

void Md5::update(Bytes data) {
    std::size_t fill = static_cast<std::size_t>(length_ % 64);
    length_ += data.size();
    std::size_t i = 0;
    while (i < data.size()) {
        std::size_t n = std::min<std::size_t>(64 - fill, data.size() - i);
        std::memcpy(buffer_ + fill, data.data() + i, n);
        fill += n;
        i += n;
        if (fill == 64) {
            block(buffer_);
            fill = 0;
        }
    }
}

Md Md5::finish() {
    std::uint64_t bits = length_ * 8;
    std::uint8_t pad[72] = {0x80};
    std::size_t fill = static_cast<std::size_t>(length_ % 64);
    std::size_t n = (fill < 56 ? 56 : 120) - fill;
    update(Bytes(pad, n));
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    update(Bytes(len, 8));
    return md_out(h_);
}

Md md5(Bytes data) {
    Md5 m;
    m.update(data);
    return m.finish();
}

Md hmac_md5(Bytes key, Bytes message) {
    std::uint8_t k[64] = {};
    if (key.size() > 64) {
        Md d = md5(key);
        std::memcpy(k, d.data(), d.size());
    } else {
        std::memcpy(k, key.data(), key.size());
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Md5 inner;
    inner.update(Bytes(ipad, 64));
    inner.update(message);
    Md in = inner.finish();
    Md5 outer;
    outer.update(Bytes(opad, 64));
    outer.update(Bytes(in.data(), in.size()));
    return outer.finish();
}

std::string hex(Bytes data) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : data) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

// ---------------------------------------------------------------- SHA-512

Sha512::Sha512()
    : h_{0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
         0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull} {}

void Sha512::block(const std::uint8_t* p) {
    static const std::uint64_t k[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
        0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
        0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
        0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
        0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
        0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
        0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
        0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
        0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
        0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
        0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
        0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
        0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
        0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
    std::uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; ++i) {
        std::uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        std::uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 80; ++i) {
        std::uint64_t s1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        std::uint64_t ch = (e & f) ^ (~e & g);
        std::uint64_t t1 = h + s1 + ch + k[i] + w[i];
        std::uint64_t s0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint64_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha512::update(Bytes data) {
    std::size_t fill = static_cast<std::size_t>(length_ % 128);
    length_ += data.size();
    std::size_t i = 0;
    while (i < data.size()) {
        std::size_t n = std::min<std::size_t>(128 - fill, data.size() - i);
        std::memcpy(buffer_ + fill, data.data() + i, n);
        fill += n;
        i += n;
        if (fill == 128) {
            block(buffer_);
            fill = 0;
        }
    }
}

Sha512Digest Sha512::finish() {
    std::uint64_t bits = length_ * 8;
    std::uint8_t pad[144] = {0x80};
    std::size_t fill = static_cast<std::size_t>(length_ % 128);
    std::size_t n = (fill < 112 ? 112 : 240) - fill;
    update(Bytes(pad, n));
    std::uint8_t len[16] = {};
    for (int i = 0; i < 8; ++i) len[15 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    update(Bytes(len, 16));
    Sha512Digest out;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) out[i * 8 + j] = static_cast<std::uint8_t>(h_[i] >> (56 - 8 * j));
    return out;
}

Sha512Digest sha512(Bytes data) {
    Sha512 s;
    s.update(data);
    return s.finish();
}

// ---------------------------------------------------------------- AES-CMAC

Md aes_cmac(const std::uint8_t key[16], Bytes message) {
    crypto::Aes128 aes(key);
    auto shift = [](const std::uint8_t in[16], std::uint8_t out[16]) {
        std::uint8_t carry = 0;
        for (int i = 15; i >= 0; --i) {
            std::uint8_t next = in[i] >> 7;
            out[i] = static_cast<std::uint8_t>((in[i] << 1) | carry);
            carry = next;
        }
        if (in[0] & 0x80) out[15] ^= 0x87;
    };
    std::uint8_t zero[16] = {}, l[16], k1[16], k2[16];
    aes.encrypt_block(zero, l);
    shift(l, k1);
    shift(k1, k2);
    std::size_t n = (message.size() + 15) / 16;
    bool complete = n > 0 && message.size() % 16 == 0;
    if (n == 0) n = 1;
    std::uint8_t x[16] = {}, last[16] = {};
    std::size_t tail = message.size() - (n - 1) * 16;
    if (complete) {
        for (int i = 0; i < 16; ++i) last[i] = message[(n - 1) * 16 + i] ^ k1[i];
    } else {
        for (std::size_t i = 0; i < tail; ++i) last[i] = message[(n - 1) * 16 + i];
        last[tail] = 0x80;
        for (int i = 0; i < 16; ++i) last[i] ^= k2[i];
    }
    for (std::size_t b = 0; b + 1 < n; ++b) {
        for (int i = 0; i < 16; ++i) x[i] ^= message[b * 16 + i];
        aes.encrypt_block(x, x);
    }
    for (int i = 0; i < 16; ++i) x[i] ^= last[i];
    Md out;
    aes.encrypt_block(x, out.data());
    return out;
}

// ---------------------------------------------------------------- AES-GCM and AES-CCM

namespace {

// XORs the CTR keystream from `counter` (its last `inc_bytes` bytes count up,
// big-endian) into `data`: many blocks per call, for the AES instructions.
void ctr_xor(const crypto::Aes128& aes, std::uint8_t counter[16], int inc_bytes, MutableBytes data) {
    constexpr std::size_t kBatch = 64;
    std::uint8_t blocks[kBatch * 16];
    for (std::size_t at = 0; at < data.size();) {
        const std::size_t n = std::min(kBatch, (data.size() - at + 15) / 16);
        for (std::size_t b = 0; b < n; ++b) {
            std::memcpy(blocks + b * 16, counter, 16);
            for (int i = 15; i >= 16 - inc_bytes; --i)
                if (++counter[i] != 0) break;
        }
        aes.encrypt_blocks(blocks, blocks, n);
        const std::size_t len = std::min(n * 16, data.size() - at);
        for (std::size_t i = 0; i < len; ++i) data[at + i] ^= blocks[i];
        at += len;
    }
}

bool same_tag(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    std::uint8_t diff = 0; // no early exit: the time does not tell where they differ
    for (std::size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

// GHASH (SP 800-38D 6.4) with Shoup's 4-bit tables.
class Ghash {
public:
    explicit Ghash(const std::uint8_t h[16]) {
        std::uint64_t vh = be64(h), vl = be64(h + 8);
        hh_[8] = vh;
        hl_[8] = vl;
        for (int i = 4; i > 0; i >>= 1) {
            const std::uint64_t t = (vl & 1) ? 0xe100000000000000ULL : 0;
            vl = (vh << 63) | (vl >> 1);
            vh = (vh >> 1) ^ t;
            hh_[i] = vh;
            hl_[i] = vl;
        }
        for (int i = 2; i <= 8; i *= 2)
            for (int j = 1; j < i; ++j) {
                hh_[i + j] = hh_[i] ^ hh_[j];
                hl_[i + j] = hl_[i] ^ hl_[j];
            }
    }
    // Absorbs `data`, zero-padded to whole blocks.
    void update(Bytes data) {
        for (std::size_t at = 0; at < data.size(); at += 16) {
            const std::size_t n = std::min<std::size_t>(16, data.size() - at);
            for (std::size_t i = 0; i < n; ++i) y_[i] ^= data[at + i];
            multiply();
        }
    }
    void finish(std::uint64_t aad_bytes, std::uint64_t text_bytes, std::uint8_t out[16]) {
        std::uint8_t lengths[16];
        for (int i = 0; i < 8; ++i) {
            lengths[i] = static_cast<std::uint8_t>((aad_bytes * 8) >> (56 - 8 * i));
            lengths[8 + i] = static_cast<std::uint8_t>((text_bytes * 8) >> (56 - 8 * i));
        }
        update(Bytes(lengths, 16));
        std::memcpy(out, y_, 16);
    }

private:
    void multiply() {
        static const std::uint64_t last4[16] = {0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
                                                0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0};
        int lo = y_[15] & 0xf;
        std::uint64_t zh = hh_[lo], zl = hl_[lo];
        for (int i = 15; i >= 0; --i) {
            lo = y_[i] & 0xf;
            const int hi = (y_[i] >> 4) & 0xf;
            if (i != 15) {
                const int rem = static_cast<int>(zl & 0xf);
                zl = (zh << 60) | (zl >> 4);
                zh = (zh >> 4) ^ (last4[rem] << 48) ^ hh_[lo];
                zl ^= hl_[lo];
            }
            const int rem = static_cast<int>(zl & 0xf);
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4) ^ (last4[rem] << 48) ^ hh_[hi];
            zl ^= hl_[hi];
        }
        for (int i = 0; i < 8; ++i) {
            y_[i] = static_cast<std::uint8_t>(zh >> (56 - 8 * i));
            y_[8 + i] = static_cast<std::uint8_t>(zl >> (56 - 8 * i));
        }
    }
    std::uint64_t hh_[16] = {}, hl_[16] = {};
    std::uint8_t y_[16] = {};
};

// The GCM tag over `aad` and the ciphertext `text`.
void gcm_tag(const crypto::Aes128& aes, const std::uint8_t nonce[12], Bytes aad, Bytes text, std::uint8_t tag[16]) {
    std::uint8_t h[16] = {}, j0[16] = {};
    aes.encrypt_block(h, h);
    Ghash g(h);
    g.update(aad);
    g.update(text);
    g.finish(aad.size(), text.size(), tag);
    std::memcpy(j0, nonce, 12);
    j0[15] = 1;
    aes.encrypt_block(j0, j0);
    for (int i = 0; i < 16; ++i) tag[i] ^= j0[i];
}

void gcm_ctr(const crypto::Aes128& aes, const std::uint8_t nonce[12], MutableBytes data) {
    std::uint8_t counter[16] = {};
    std::memcpy(counter, nonce, 12);
    counter[15] = 2; // inc32(J0)
    ctr_xor(aes, counter, 4, data);
}

// The unencrypted CCM tag T (SP 800-38C A.2): CBC-MAC over B0, the encoded
// associated data and the payload.
void ccm_mac(const crypto::Aes128& aes, Bytes nonce, Bytes aad, Bytes text, std::size_t t, std::uint8_t mac[16]) {
    const std::size_t q = 15 - nonce.size();
    std::uint8_t x[16] = {};
    x[0] = static_cast<std::uint8_t>((aad.empty() ? 0 : 0x40) | (((t - 2) / 2) << 3) | (q - 1));
    std::memcpy(x + 1, nonce.data(), nonce.size());
    for (std::size_t i = 0; i < q; ++i) x[15 - i] = static_cast<std::uint8_t>(i < 8 ? std::uint64_t(text.size()) >> (8 * i) : 0);
    aes.encrypt_block(x, x);
    std::uint8_t block[16];
    std::size_t fill = 0;
    auto absorb = [&](const std::uint8_t* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            block[fill++] = p[i];
            if (fill == 16) {
                for (int k = 0; k < 16; ++k) x[k] ^= block[k];
                aes.encrypt_block(x, x);
                fill = 0;
            }
        }
    };
    auto flush = [&] {
        if (fill == 0) return;
        std::memset(block + fill, 0, 16 - fill);
        for (int k = 0; k < 16; ++k) x[k] ^= block[k];
        aes.encrypt_block(x, x);
        fill = 0;
    };
    if (!aad.empty()) {
        std::uint8_t len[6];
        std::size_t n;
        if (aad.size() < 0xFF00) {
            len[0] = static_cast<std::uint8_t>(aad.size() >> 8);
            len[1] = static_cast<std::uint8_t>(aad.size());
            n = 2;
        } else {
            len[0] = 0xFF;
            len[1] = 0xFE;
            for (int i = 0; i < 4; ++i) len[2 + i] = static_cast<std::uint8_t>(std::uint64_t(aad.size()) >> (24 - 8 * i));
            n = 6;
        }
        absorb(len, n);
        absorb(aad.data(), aad.size());
        flush();
    }
    absorb(text.data(), text.size());
    flush();
    std::memcpy(mac, x, 16);
}

// Counter block i: flags (q - 1), the nonce, i in q bytes.
void ccm_counter(Bytes nonce, std::uint8_t counter[16]) {
    std::memset(counter, 0, 16);
    counter[0] = static_cast<std::uint8_t>(15 - nonce.size() - 1);
    std::memcpy(counter + 1, nonce.data(), nonce.size());
}

bool ccm_shape_ok(Bytes nonce, std::size_t t) {
    return nonce.size() >= 7 && nonce.size() <= 13 && t >= 4 && t <= 16 && t % 2 == 0;
}

} // namespace

void aes_gcm_encrypt(const std::uint8_t key[16], const std::uint8_t nonce[12], Bytes aad, MutableBytes data,
                     std::uint8_t tag[16]) {
    crypto::Aes128 aes(key);
    gcm_ctr(aes, nonce, data);
    gcm_tag(aes, nonce, aad, Bytes(data.data(), data.size()), tag);
}

bool aes_gcm_decrypt(const std::uint8_t key[16], const std::uint8_t nonce[12], Bytes aad, MutableBytes data,
                     const std::uint8_t tag[16]) {
    crypto::Aes128 aes(key);
    std::uint8_t expected[16];
    gcm_tag(aes, nonce, aad, Bytes(data.data(), data.size()), expected);
    if (!same_tag(expected, tag, 16)) return false;
    gcm_ctr(aes, nonce, data);
    return true;
}

void aes_ccm_encrypt(const std::uint8_t key[16], Bytes nonce, Bytes aad, MutableBytes data, MutableBytes tag) {
    if (!ccm_shape_ok(nonce, tag.size())) return;
    crypto::Aes128 aes(key);
    std::uint8_t mac[16], counter[16], s0[16];
    ccm_mac(aes, nonce, aad, Bytes(data.data(), data.size()), tag.size(), mac);
    ccm_counter(nonce, counter);
    aes.encrypt_block(counter, s0);
    counter[15] = 1;
    ctr_xor(aes, counter, static_cast<int>(15 - nonce.size()), data);
    for (std::size_t i = 0; i < tag.size(); ++i) tag[i] = mac[i] ^ s0[i];
}

bool aes_ccm_decrypt(const std::uint8_t key[16], Bytes nonce, Bytes aad, MutableBytes data, Bytes tag) {
    if (!ccm_shape_ok(nonce, tag.size())) return false;
    crypto::Aes128 aes(key);
    std::uint8_t mac[16], counter[16], s0[16];
    ccm_counter(nonce, counter);
    aes.encrypt_block(counter, s0);
    // CCM authenticates the plaintext: decrypt a copy, check, then keep it.
    std::vector<std::uint8_t> plain(data.begin(), data.end());
    counter[15] = 1;
    ctr_xor(aes, counter, static_cast<int>(15 - nonce.size()), MutableBytes(plain.data(), plain.size()));
    ccm_mac(aes, nonce, aad, Bytes(plain.data(), plain.size()), tag.size(), mac);
    std::uint8_t expected[16];
    for (std::size_t i = 0; i < tag.size(); ++i) expected[i] = mac[i] ^ s0[i];
    if (!same_tag(expected, tag.data(), tag.size())) return false;
    if (!plain.empty()) std::memcpy(data.data(), plain.data(), plain.size());
    return true;
}

} // namespace alembic::engine::netcrypto

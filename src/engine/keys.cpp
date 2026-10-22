// SPDX-License-Identifier: GPL-3.0-or-later
// X25519 after TweetNaCl's crypto_scalarmult (public domain): a field element
// is 16 limbs of 16 bits in int64_t, reduced lazily (car25519); the ladder
// swaps with masks, never with branches on secret bits.
#include "keys.hpp"

#include "alembic/sha256.hpp"

#include <cstring>

namespace alembic::engine::keys {

namespace {

using Gf = std::int64_t[16];

const Gf k121665 = {0xDB41, 1};

void set(Gf o, const Gf a) {
    for (int i = 0; i < 16; ++i) o[i] = a[i];
}

void carry(Gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (std::int64_t(1) << 16);
        std::int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

// Swaps p and q when b is 1, in constant time.
void swap(Gf p, Gf q, int b) {
    std::int64_t mask = ~(std::int64_t(b) - 1);
    for (int i = 0; i < 16; ++i) {
        std::int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(std::uint8_t* o, const Gf n) {
    Gf m, t;
    set(t, n);
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        swap(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<std::uint8_t>(t[i] >> 8);
    }
}

void unpack(Gf o, const std::uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (std::int64_t(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

void add(Gf o, const Gf a, const Gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

void sub(Gf o, const Gf a, const Gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

void mul(Gf o, const Gf a, const Gf b) {
    std::int64_t t[31] = {};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    carry(o);
    carry(o);
}

void square(Gf o, const Gf a) { mul(o, a, a); }

// a^(p-2): the inverse.
void invert(Gf o, const Gf i) {
    Gf c;
    set(c, i);
    for (int a = 253; a >= 0; --a) {
        square(c, c);
        if (a != 2 && a != 4) mul(c, c, i);
    }
    set(o, c);
}

} // namespace

Key x25519(const Key& scalar, const Key& point) {
    std::uint8_t z[32];
    std::memcpy(z, scalar.data(), 32);
    z[31] = static_cast<std::uint8_t>((z[31] & 127) | 64);
    z[0] &= 248;
    Gf x, a = {1}, b, c = {}, d = {1}, e, f;
    unpack(x, point.data());
    set(b, x);
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        swap(a, b, r);
        swap(c, d, r);
        add(e, a, c);
        sub(a, a, c);
        add(c, b, d);
        sub(b, b, d);
        square(d, e);
        square(f, a);
        mul(a, c, a);
        mul(c, b, e);
        add(e, a, c);
        sub(a, a, c);
        square(b, a);
        sub(c, d, f);
        mul(a, c, k121665);
        add(a, a, d);
        mul(c, c, a);
        mul(a, d, f);
        mul(d, b, x);
        square(b, e);
        swap(a, b, r);
        swap(c, d, r);
    }
    invert(c, c);
    mul(a, a, c);
    Key out;
    pack(out.data(), a);
    std::memset(z, 0, sizeof z);
    return out;
}

Key x25519_base(const Key& scalar) {
    Key nine{};
    nine[0] = 9;
    return x25519(scalar, nine);
}

Key sha256(Bytes data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

Key hmac_sha256(Bytes key, Bytes message) {
    std::uint8_t block[64] = {};
    if (key.size() > 64) {
        Key k = sha256(key);
        std::memcpy(block, k.data(), k.size());
    } else {
        std::memcpy(block, key.data(), key.size());
    }
    std::uint8_t pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x36;
    Sha256 inner;
    inner.update(Bytes(pad, 64));
    inner.update(message);
    Key ih = inner.finish();
    for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x5c;
    Sha256 outer;
    outer.update(Bytes(pad, 64));
    outer.update(Bytes(ih.data(), ih.size()));
    return outer.finish();
}

bool equal(Bytes a, Bytes b) {
    if (a.size() != b.size()) return false;
    std::uint8_t d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= a[i] ^ b[i];
    return d == 0;
}

std::string to_hex(Bytes data) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (std::uint8_t b : data) {
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    return s;
}

bool from_hex(const std::string& hex, MutableBytes out) {
    if (hex.size() != out.size() * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return true;
}

} // namespace alembic::engine::keys

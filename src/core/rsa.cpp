// SPDX-License-Identifier: GPL-3.0-or-later
// See rsa.hpp. The Montgomery arithmetic and MT19937 were first written for
// the PS5 writer (fpkg_keys.cpp) and are shared here for any modulus size.
#include "alembic/rsa.hpp"

#include <algorithm>
#include <cstring>

namespace alembic::rsa {

namespace {

// Little-endian 32-bit limbs.
using Big = std::vector<std::uint32_t>;

Big from_be(Bytes bytes, std::size_t limbs) {
    Big out(limbs, 0);
    std::size_t n = bytes.size();
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t bit = (n - 1 - i) * 8;
        if (bit / 32 < limbs) out[bit / 32] |= static_cast<std::uint32_t>(bytes[i]) << (bit % 32);
    }
    return out;
}

std::vector<std::uint8_t> to_be(const Big& v, std::size_t length) {
    std::vector<std::uint8_t> out(length);
    for (std::size_t i = 0; i < length; ++i) {
        std::size_t bit = (length - 1 - i) * 8;
        out[i] = static_cast<std::uint8_t>(v[bit / 32] >> (bit % 32));
    }
    return out;
}

bool geq(const Big& a, const Big& b) {
    for (std::size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}

void sub_in_place(Big& a, const Big& b) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t d = static_cast<std::uint64_t>(a[i]) - b[i] - borrow;
        a[i] = static_cast<std::uint32_t>(d);
        borrow = (d >> 63) & 1;
    }
}

// Montgomery arithmetic modulo an odd n (CIOS).
class Montgomery {
public:
    explicit Montgomery(const Big& n) : n_(n), k_(n.size()) {
        // n0inv = -n^-1 mod 2^32 (Newton iteration).
        std::uint32_t inv = 1;
        for (int i = 0; i < 5; ++i) inv *= 2 - n[0] * inv;
        n0inv_ = ~inv + 1;
        // R^2 mod n with R = 2^(32k): double 1 modulo n 2*32*k times.
        r2_.assign(k_, 0);
        r2_[0] = 1;
        for (std::size_t i = 0; i < 2 * 32 * k_; ++i) {
            std::uint32_t carry = 0;
            for (std::size_t j = 0; j < k_; ++j) {
                std::uint32_t next = r2_[j] >> 31;
                r2_[j] = (r2_[j] << 1) | carry;
                carry = next;
            }
            if (carry || geq(r2_, n_)) sub_in_place(r2_, n_);
        }
    }

    Big mul(const Big& a, const Big& b) const {
        std::vector<std::uint32_t> t(k_ + 2, 0);
        for (std::size_t i = 0; i < k_; ++i) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < k_; ++j) {
                std::uint64_t s = static_cast<std::uint64_t>(t[j]) + static_cast<std::uint64_t>(a[j]) * b[i] + carry;
                t[j] = static_cast<std::uint32_t>(s);
                carry = s >> 32;
            }
            std::uint64_t s = static_cast<std::uint64_t>(t[k_]) + carry;
            t[k_] = static_cast<std::uint32_t>(s);
            t[k_ + 1] = static_cast<std::uint32_t>(s >> 32);
            std::uint32_t m = t[0] * n0inv_;
            s = static_cast<std::uint64_t>(t[0]) + static_cast<std::uint64_t>(m) * n_[0];
            carry = s >> 32;
            for (std::size_t j = 1; j < k_; ++j) {
                s = static_cast<std::uint64_t>(t[j]) + static_cast<std::uint64_t>(m) * n_[j] + carry;
                t[j - 1] = static_cast<std::uint32_t>(s);
                carry = s >> 32;
            }
            s = static_cast<std::uint64_t>(t[k_]) + carry;
            t[k_ - 1] = static_cast<std::uint32_t>(s);
            t[k_] = t[k_ + 1] + static_cast<std::uint32_t>(s >> 32);
        }
        Big r(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(k_));
        if (t[k_] || geq(r, n_)) sub_in_place(r, n_);
        return r;
    }

    // value^exponent mod n, left to right; `exponent` is big-endian bytes.
    Big pow(const Big& value, Bytes exponent) const {
        Big x = mul(value, r2_); // to Montgomery form
        Big one(k_, 0);
        one[0] = 1;
        Big acc = mul(one, r2_); // 1 in Montgomery form
        for (std::uint8_t byte : exponent)
            for (int bit = 7; bit >= 0; --bit) {
                acc = mul(acc, acc);
                if ((byte >> bit) & 1) acc = mul(acc, x);
            }
        return mul(acc, one); // out of Montgomery form
    }

private:
    Big n_;
    std::size_t k_;
    std::uint32_t n0inv_ = 0;
    Big r2_;
};

class MersenneTwister {
public:
    explicit MersenneTwister(const std::vector<std::uint32_t>& seed) {
        init(19650218u);
        std::size_t i = 1, j = 0;
        for (std::size_t k = std::max<std::size_t>(kN, seed.size()); k > 0; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + seed[j] + static_cast<std::uint32_t>(j);
            ++i;
            ++j;
            if (i >= kN) {
                mt_[0] = mt_[kN - 1];
                i = 1;
            }
            if (j >= seed.size()) j = 0;
        }
        for (std::size_t k = kN - 1; k > 0; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - static_cast<std::uint32_t>(i);
            ++i;
            if (i >= kN) {
                mt_[0] = mt_[kN - 1];
                i = 1;
            }
        }
        mt_[0] = 0x80000000u;
    }

    std::uint32_t next() {
        if (index_ >= kN) {
            std::size_t kk = 0;
            for (; kk < kN - kM; ++kk) {
                std::uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7FFFFFFFu);
                mt_[kk] = mt_[kk + kM] ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0u);
            }
            for (; kk < kN - 1; ++kk) {
                std::uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7FFFFFFFu);
                mt_[kk] = mt_[kk + kM - kN] ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0u);
            }
            std::uint32_t y = (mt_[kN - 1] & 0x80000000u) | (mt_[0] & 0x7FFFFFFFu);
            mt_[kN - 1] = mt_[kM - 1] ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0u);
            index_ = 0;
        }
        std::uint32_t x = mt_[index_++];
        x ^= x >> 11;
        x ^= (x << 7) & 0x9D2C5680u;
        x ^= (x << 15) & 0xEFC60000u;
        x ^= x >> 18;
        return x;
    }

private:
    static constexpr std::size_t kN = 624, kM = 397;
    void init(std::uint32_t seed) {
        mt_[0] = seed;
        for (std::size_t i = 1; i < kN; ++i)
            mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + static_cast<std::uint32_t>(i);
        index_ = kN;
    }
    std::uint32_t mt_[kN] = {};
    std::size_t index_ = kN;
};

} // namespace

std::vector<std::uint8_t> power(Bytes modulus, Bytes exponent, Bytes value) {
    std::size_t limbs = modulus.size() / 4;
    Montgomery m(from_be(modulus, limbs));
    return to_be(m.pow(from_be(value, limbs), exponent), modulus.size());
}

std::vector<std::uint8_t> public_op(Bytes modulus, Bytes value) {
    static const std::uint8_t kE[] = {0x01, 0x00, 0x01};
    return power(modulus, Bytes(kE, sizeof kE), value);
}

std::vector<std::uint8_t> mt_wrap(Bytes modulus, Bytes data) {
    const std::size_t size = modulus.size();
    const std::size_t n = data.size();
    std::vector<std::uint8_t> block(size, 0);
    block[1] = 0x02;
    std::size_t pad = size - n - 3;
    std::vector<std::uint8_t> seed_source(modulus.begin(), modulus.end());
    seed_source.insert(seed_source.end(), data.begin(), data.end());
    auto h1 = crypto::sha256(Bytes(seed_source.data(), seed_source.size()));
    auto h2 = crypto::sha256(Bytes(h1.data(), h1.size()));
    std::vector<std::uint32_t> words(8);
    for (int i = 0; i < 8; ++i)
        words[i] = (static_cast<std::uint32_t>(h2[i * 4]) << 24) | (static_cast<std::uint32_t>(h2[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(h2[i * 4 + 2]) << 8) | h2[i * 4 + 3];
    MersenneTwister rng(words);
    std::size_t pos = 2, end = 2 + pad;
    while (pos < end) {
        std::uint8_t w[48];
        for (int j = 0; j < 12; ++j) {
            std::uint32_t v = rng.next();
            w[j * 4] = static_cast<std::uint8_t>(v >> 24);
            w[j * 4 + 1] = static_cast<std::uint8_t>(v >> 16);
            w[j * 4 + 2] = static_cast<std::uint8_t>(v >> 8);
            w[j * 4 + 3] = static_cast<std::uint8_t>(v);
        }
        auto h = crypto::sha256(Bytes(w, sizeof w));
        for (std::uint8_t b : h) {
            if (b == 0) continue;
            block[pos++] = b;
            if (pos == end) break;
        }
    }
    block[end] = 0;
    if (n) std::memcpy(&block[end + 1], data.data(), n);
    return public_op(modulus, block);
}

std::vector<std::uint8_t> sign_sha256(Bytes modulus, Bytes private_exponent, const crypto::Digest& digest) {
    // EMSA-PKCS1-v1_5: 00 01 FF..FF 00 DigestInfo(SHA-256) digest.
    static const std::uint8_t kInfo[] = {0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                         0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
    const std::size_t size = modulus.size();
    std::vector<std::uint8_t> block(size, 0xFF);
    block[0] = 0x00;
    block[1] = 0x01;
    std::size_t t = sizeof kInfo + digest.size();
    block[size - t - 1] = 0x00;
    std::memcpy(&block[size - t], kInfo, sizeof kInfo);
    std::memcpy(&block[size - digest.size()], digest.data(), digest.size());
    return power(modulus, private_exponent, block);
}

} // namespace alembic::rsa

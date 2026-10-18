// SPDX-License-Identifier: GPL-3.0-or-later
// The RSA-3072 key wraps of a debug package. Ported from
// pearlxcore/ProsperoPkgTool (MIT) Crypto/ProsperoRsaKeys.cs and
// Crypto/MersenneTwister.cs. Only public-key operations (exponent 65537)
// under public moduli; the moduli are not part of Alembic and are loaded from
// files the user supplies (the engine's Components page downloads them,
// pinned by SHA-256).
#include "alembic/fpkg_build.hpp"

#include "alembic/bytes.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/sha256.hpp"

#include <cstring>

namespace alembic::fpkg::build {

namespace {

constexpr std::size_t kModulus = 384;
constexpr std::size_t kLimbs = kModulus / 4;

// Little-endian 32-bit limbs.
using Big = std::vector<std::uint32_t>;

Big from_be(const std::uint8_t* p, std::size_t n) {
    Big out(kLimbs, 0);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t bit = (n - 1 - i) * 8;
        out[bit / 32] |= static_cast<std::uint32_t>(p[i]) << (bit % 32);
    }
    return out;
}

std::vector<std::uint8_t> to_be(const Big& v) {
    std::vector<std::uint8_t> out(kModulus);
    for (std::size_t i = 0; i < kModulus; ++i) {
        std::size_t bit = (kModulus - 1 - i) * 8;
        out[i] = static_cast<std::uint8_t>(v[bit / 32] >> (bit % 32));
    }
    return out;
}

bool geq(const Big& a, const Big& b) {
    for (std::size_t i = kLimbs; i-- > 0;)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}

void sub_in_place(Big& a, const Big& b) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        std::uint64_t d = static_cast<std::uint64_t>(a[i]) - b[i] - borrow;
        a[i] = static_cast<std::uint32_t>(d);
        borrow = (d >> 63) & 1;
    }
}

// Montgomery arithmetic modulo an odd n (CIOS).
class Montgomery {
public:
    explicit Montgomery(const Big& n) : n_(n) {
        // n0inv = -n^-1 mod 2^32 (Newton iteration).
        std::uint32_t inv = 1;
        for (int i = 0; i < 5; ++i) inv *= 2 - n[0] * inv;
        n0inv_ = ~inv + 1;
        // R^2 mod n with R = 2^(32*kLimbs): double 1 modulo n 2*32*kLimbs times.
        r2_.assign(kLimbs, 0);
        r2_[0] = 1;
        for (std::size_t i = 0; i < 2 * 32 * kLimbs; ++i) {
            std::uint32_t carry = 0;
            for (std::size_t j = 0; j < kLimbs; ++j) {
                std::uint32_t next = r2_[j] >> 31;
                r2_[j] = (r2_[j] << 1) | carry;
                carry = next;
            }
            if (carry || geq(r2_, n_)) sub_in_place(r2_, n_);
        }
    }

    Big mul(const Big& a, const Big& b) const {
        std::vector<std::uint32_t> t(kLimbs + 2, 0);
        for (std::size_t i = 0; i < kLimbs; ++i) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < kLimbs; ++j) {
                std::uint64_t s = static_cast<std::uint64_t>(t[j]) + static_cast<std::uint64_t>(a[j]) * b[i] + carry;
                t[j] = static_cast<std::uint32_t>(s);
                carry = s >> 32;
            }
            std::uint64_t s = static_cast<std::uint64_t>(t[kLimbs]) + carry;
            t[kLimbs] = static_cast<std::uint32_t>(s);
            t[kLimbs + 1] = static_cast<std::uint32_t>(s >> 32);
            std::uint32_t m = t[0] * n0inv_;
            s = static_cast<std::uint64_t>(t[0]) + static_cast<std::uint64_t>(m) * n_[0];
            carry = s >> 32;
            for (std::size_t j = 1; j < kLimbs; ++j) {
                s = static_cast<std::uint64_t>(t[j]) + static_cast<std::uint64_t>(m) * n_[j] + carry;
                t[j - 1] = static_cast<std::uint32_t>(s);
                carry = s >> 32;
            }
            s = static_cast<std::uint64_t>(t[kLimbs]) + carry;
            t[kLimbs - 1] = static_cast<std::uint32_t>(s);
            t[kLimbs] = t[kLimbs + 1] + static_cast<std::uint32_t>(s >> 32);
        }
        Big r(t.begin(), t.begin() + kLimbs);
        if (t[kLimbs] || geq(r, n_)) sub_in_place(r, n_);
        return r;
    }

    // value^65537 mod n
    Big pow65537(const Big& value) const {
        Big x = mul(value, r2_); // to Montgomery form
        Big acc = x;
        for (int i = 0; i < 16; ++i) acc = mul(acc, acc);
        acc = mul(acc, x);
        Big one(kLimbs, 0);
        one[0] = 1;
        return mul(acc, one); // out of Montgomery form
    }

private:
    Big n_;
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

// EME-PKCS#1 v1.5 with padding from MT19937 seeded by SHA-256(SHA-256(modulus || data)).
std::vector<std::uint8_t> rsa_wrap(const std::uint8_t* modulus, const std::uint8_t* data, std::size_t n) {
    std::vector<std::uint8_t> block(kModulus, 0);
    block[1] = 0x02;
    std::size_t pad = kModulus - n - 3;
    std::vector<std::uint8_t> seed_source(modulus, modulus + kModulus);
    seed_source.insert(seed_source.end(), data, data + n);
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
    std::memcpy(&block[end + 1], data, n);
    Montgomery m(from_be(modulus, kModulus));
    return to_be(m.pow65537(from_be(block.data(), kModulus)));
}

struct KeyFile {
    const char* name;
    std::size_t size;
    const char* sha256;
};
// The files the engine downloads (src/engine/deps.cpp, component fpkg_keys).
constexpr KeyFile kKeyFiles[] = {
    {"passcode.bin", 7 * kModulus, "81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e"},
    {"mount_image.bin", kModulus, "4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0"},
};

} // namespace

Result<PackageKeys> load_package_keys(const std::string& folder) {
    PackageKeys keys;
    for (const KeyFile& k : kKeyFiles) {
        std::string path = folder + "/" + k.name;
        auto file = File::open(path, File::Mode::read);
        if (!file.ok())
            return make_error(Errc::invalid_argument, "missing FPKG key file " + path +
                                                          " (get the FPKG key files in Components, or pass --keys DIR)");
        ALEMBIC_ASSIGN(std::uint64_t size, file->size());
        if (size != k.size) return make_error(Errc::invalid_data, "the key file " + path + " has the wrong size");
        std::vector<std::uint8_t> data(k.size);
        ALEMBIC_TRY(file->read_at(0, data));
        if (alembic::Sha256::hex(crypto::sha256(Bytes(data.data(), data.size()))) != k.sha256)
            return make_error(Errc::invalid_data, "the key file " + path + " is not the expected one (SHA-256 differs)");
        if (std::string(k.name) == "passcode.bin") keys.passcode_moduli = std::move(data);
        else keys.mount_image_modulus = std::move(data);
    }
    return keys;
}

std::vector<std::uint8_t> build_entry_keys(const PackageKeys& keys, const std::string& content_id,
                                           const std::string& passcode) {
    std::vector<std::uint8_t> entry;
    std::uint8_t cid[48] = {};
    std::memcpy(cid, content_id.data(), std::min<std::size_t>(content_id.size(), 48));
    crypto::Digest seed = crypto::sha3_256(Bytes(cid, 48));
    entry.insert(entry.end(), seed.begin(), seed.end());
    for (std::uint32_t i = 0; i < 7; ++i) {
        crypto::Digest key = compute_keys(content_id, passcode, i);
        crypto::Digest d = crypto::sha3_256(Bytes(key.data(), 32));
        for (int j = 0; j < 32; ++j) d[j] ^= key[j];
        entry.insert(entry.end(), d.begin(), d.end());
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        std::vector<std::uint8_t> wrap;
        if (i == 0) {
            wrap = rsa_wrap(&keys.passcode_moduli[0], reinterpret_cast<const std::uint8_t*>(passcode.data()), passcode.size());
        } else {
            crypto::Digest key = compute_keys(content_id, passcode, i);
            wrap = rsa_wrap(&keys.passcode_moduli[i * kModulus], key.data(), 32);
        }
        entry.insert(entry.end(), wrap.begin(), wrap.end());
    }
    return entry;
}

std::vector<std::uint8_t> build_image_key(const PackageKeys& keys, const crypto::Digest& ekpfs) {
    std::vector<std::uint8_t> entry;
    std::vector<std::uint8_t> wrap = rsa_wrap(keys.mount_image_modulus.data(), ekpfs.data(), 32);
    while (entry.size() < 0x800) {
        std::size_t take = std::min<std::size_t>(kModulus, 0x800 - entry.size());
        entry.insert(entry.end(), wrap.begin(), wrap.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return entry;
}

std::vector<std::uint8_t> build_cnt_header_wrap(const PackageKeys& keys, const std::uint8_t* header4096) {
    crypto::Digest d = crypto::sha3_256(Bytes(header4096, 4096));
    return rsa_wrap(&keys.passcode_moduli[3 * kModulus], d.data(), 32);
}

} // namespace alembic::fpkg::build

// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/crypto.hpp"

#include "alembic/sha256.hpp"

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "cpu_features.hpp"

// x86-64: the AES and CRC32 code is compiled for those instructions whatever
// the build targets (target attributes) and runs only on CPUs that have them
// (CPUID; a build that targets them, the PS5, skips the check). ARMv8: only
// when the build targets them (Apple silicon does).
#if defined(__x86_64__)
#define ALEMBIC_AES_X86 1
#define ALEMBIC_CRC_X86 1
#include <immintrin.h>
#define ALEMBIC_TARGET_AES __attribute__((target("aes,sse2")))
#define ALEMBIC_TARGET_CRC __attribute__((target("sse4.2")))
#else
#define ALEMBIC_TARGET_AES
#define ALEMBIC_TARGET_CRC
#endif
#if defined(__ARM_FEATURE_AES) && defined(__aarch64__)
#define ALEMBIC_AES_ARM 1
#include <arm_neon.h>
#endif
#if defined(__ARM_FEATURE_CRC32) && defined(__aarch64__)
#define ALEMBIC_CRC_ARM 1
#include <arm_acle.h>
#endif

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
// Rotation offsets r[x + 5y] (FIPS-202 rho).
constexpr int kRho[25] = {0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14};
// Where pi moves lane x + 5y: to y + 5((2x + 3y) mod 5).
constexpr int pi_target(int i) { return i / 5 + 5 * ((2 * (i % 5) + 3 * (i / 5)) % 5); }

template <int N>
inline std::uint64_t rotl64(std::uint64_t x) {
    if constexpr (N == 0) return x;
    else return (x << N) | (x >> (64 - N));
}

// One round, written with constant indices only so the compiler keeps the
// 25 lanes in registers.
template <int I>
inline void rho_pi(const std::uint64_t* a, const std::uint64_t* d, std::uint64_t* b) {
    b[pi_target(I)] = rotl64<kRho[I]>(a[I] ^ d[I % 5]);
    if constexpr (I + 1 < 25) rho_pi<I + 1>(a, d, b);
}

inline void keccak_round(std::uint64_t* a, std::uint64_t rc) {
    std::uint64_t c[5], d[5], b[25];
    for (int x = 0; x < 5; ++x) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
    d[0] = c[4] ^ rotl64<1>(c[1]);
    d[1] = c[0] ^ rotl64<1>(c[2]);
    d[2] = c[1] ^ rotl64<1>(c[3]);
    d[3] = c[2] ^ rotl64<1>(c[4]);
    d[4] = c[3] ^ rotl64<1>(c[0]);
    rho_pi<0>(a, d, b);
    for (int y = 0; y < 25; y += 5) {
        a[y + 0] = b[y + 0] ^ (~b[y + 1] & b[y + 2]);
        a[y + 1] = b[y + 1] ^ (~b[y + 2] & b[y + 3]);
        a[y + 2] = b[y + 2] ^ (~b[y + 3] & b[y + 4]);
        a[y + 3] = b[y + 3] ^ (~b[y + 4] & b[y + 0]);
        a[y + 4] = b[y + 4] ^ (~b[y + 0] & b[y + 1]);
    }
    a[0] ^= rc;
}

void keccak_f1600(std::uint64_t a[25]) {
    for (int round = 0; round < 24; ++round) keccak_round(a, kRoundConstants[round]);
}

inline std::uint64_t load_le64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (std::endian::native == std::endian::big) v = __builtin_bswap64(v);
    return v;
}

} // namespace

void Sha3_256::absorb(const std::uint8_t* block) {
    for (int i = 0; i < 17; ++i) state_[i] ^= load_le64(block + i * 8);
    keccak_f1600(state_);
}

void Sha3_256::absorb_block() {
    absorb(buffer_);
    fill_ = 0;
}

void Sha3_256::update(Bytes data) {
    const std::uint8_t* p = data.data();
    std::size_t n = data.size();
    if (fill_) {
        std::size_t take = std::min(n, sizeof buffer_ - fill_);
        std::memcpy(buffer_ + fill_, p, take);
        fill_ += take;
        p += take;
        n -= take;
        if (fill_ < sizeof buffer_) return;
        absorb_block();
    }
    for (; n >= sizeof buffer_; p += sizeof buffer_, n -= sizeof buffer_) absorb(p);
    if (n) {
        std::memcpy(buffer_, p, n);
        fill_ = n;
    }
}

std::array<std::uint8_t, Sha3_256::kStateBytes> Sha3_256::save() const {
    std::array<std::uint8_t, kStateBytes> out{};
    for (int i = 0; i < 25; ++i)
        for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<std::uint8_t>(state_[i] >> (8 * b));
    std::memcpy(out.data() + 200, buffer_, 136);
    for (int b = 0; b < 8; ++b) out[336 + b] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(fill_) >> (8 * b));
    return out;
}

Sha3_256 Sha3_256::restore(const std::uint8_t* state) {
    Sha3_256 h;
    for (int i = 0; i < 25; ++i) h.state_[i] = load_le64(state + i * 8);
    std::memcpy(h.buffer_, state + 200, 136);
    h.fill_ = static_cast<std::size_t>(load_le64(state + 336) % 136);
    return h;
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

struct BackgroundSha3::Impl {
    static constexpr std::size_t kSlots = 4;
    std::vector<std::uint8_t> slots[kSlots];
    std::size_t used[kSlots] = {};
    Sha3_256* target[kSlots] = {};
    std::size_t head = 0, tail = 0, queued = 0; // ring of filled slots
    bool stop = false;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread thread;

    explicit Impl(std::size_t piece) {
        for (auto& s : slots) s.resize(piece);
        thread = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        changed.notify_all();
        thread.join();
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stop || queued > 0; });
            if (queued == 0) return;
            std::size_t slot = tail;
            lock.unlock();
            target[slot]->update(Bytes(slots[slot].data(), used[slot]));
            lock.lock();
            tail = (tail + 1) % kSlots;
            --queued;
            changed.notify_all();
        }
    }
};

BackgroundSha3::BackgroundSha3(bool background, std::size_t piece) {
    if (background) impl_ = new Impl(piece);
}

BackgroundSha3::~BackgroundSha3() { delete impl_; }

void BackgroundSha3::update(Sha3_256& hash, Bytes data) {
    if (!impl_) return hash.update(data);
    while (!data.empty()) {
        std::size_t n = std::min(data.size(), impl_->slots[0].size());
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->changed.wait(lock, [&] { return impl_->queued < Impl::kSlots; });
        std::size_t slot = impl_->head;
        lock.unlock();
        std::memcpy(impl_->slots[slot].data(), data.data(), n);
        impl_->used[slot] = n;
        impl_->target[slot] = &hash;
        lock.lock();
        impl_->head = (impl_->head + 1) % Impl::kSlots;
        ++impl_->queued;
        impl_->changed.notify_all();
        data = data.subspan(n);
    }
}

void BackgroundSha3::wait() {
    if (!impl_) return;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->changed.wait(lock, [&] { return impl_->queued == 0; });
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
    // The same schedules as bytes, as the AES instructions take them.
    for (int i = 0; i < 44; ++i) {
        store_be(ek_ + 4 * i, enc_[i]);
        store_be(dk_ + 4 * i, dec_[i]);
    }
}

// ---- AES instructions: AES-NI (x86-64, the PS5) or ARMv8 AES. Eight blocks
// are in flight at a time so the units stay busy.
#if defined(ALEMBIC_AES_X86)
namespace {
ALEMBIC_TARGET_AES inline __m128i k128(const std::uint8_t* p) { return _mm_load_si128(reinterpret_cast<const __m128i*>(p)); }
}
ALEMBIC_TARGET_AES void Aes128::hw_encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    __m128i k[11];
    for (int r = 0; r < 11; ++r) k[r] = k128(ek_ + 16 * r);
    for (; count >= 8; count -= 8, in += 128, out += 128) {
        __m128i b[8];
        for (int j = 0; j < 8; ++j) b[j] = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * j)), k[0]);
        for (int r = 1; r < 10; ++r)
            for (int j = 0; j < 8; ++j) b[j] = _mm_aesenc_si128(b[j], k[r]);
        for (int j = 0; j < 8; ++j) _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16 * j), _mm_aesenclast_si128(b[j], k[10]));
    }
    for (; count; --count, in += 16, out += 16) {
        __m128i b = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)), k[0]);
        for (int r = 1; r < 10; ++r) b = _mm_aesenc_si128(b, k[r]);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_aesenclast_si128(b, k[10]));
    }
}
ALEMBIC_TARGET_AES void Aes128::hw_decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    __m128i k[11];
    for (int r = 0; r < 11; ++r) k[r] = k128(dk_ + 16 * r);
    for (; count >= 8; count -= 8, in += 128, out += 128) {
        __m128i b[8];
        for (int j = 0; j < 8; ++j) b[j] = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * j)), k[0]);
        for (int r = 1; r < 10; ++r)
            for (int j = 0; j < 8; ++j) b[j] = _mm_aesdec_si128(b[j], k[r]);
        for (int j = 0; j < 8; ++j) _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16 * j), _mm_aesdeclast_si128(b[j], k[10]));
    }
    for (; count; --count, in += 16, out += 16) {
        __m128i b = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)), k[0]);
        for (int r = 1; r < 10; ++r) b = _mm_aesdec_si128(b, k[r]);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_aesdeclast_si128(b, k[10]));
    }
}
#elif defined(ALEMBIC_AES_ARM)
// AESE/AESD add the round key first, so the last key is a plain XOR.
void Aes128::hw_encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    uint8x16_t k[11];
    for (int r = 0; r < 11; ++r) k[r] = vld1q_u8(ek_ + 16 * r);
    for (; count >= 8; count -= 8, in += 128, out += 128) {
        uint8x16_t b[8];
        for (int j = 0; j < 8; ++j) b[j] = vld1q_u8(in + 16 * j);
        for (int r = 0; r < 9; ++r)
            for (int j = 0; j < 8; ++j) b[j] = vaesmcq_u8(vaeseq_u8(b[j], k[r]));
        for (int j = 0; j < 8; ++j) vst1q_u8(out + 16 * j, veorq_u8(vaeseq_u8(b[j], k[9]), k[10]));
    }
    for (; count; --count, in += 16, out += 16) {
        uint8x16_t b = vld1q_u8(in);
        for (int r = 0; r < 9; ++r) b = vaesmcq_u8(vaeseq_u8(b, k[r]));
        vst1q_u8(out, veorq_u8(vaeseq_u8(b, k[9]), k[10]));
    }
}
void Aes128::hw_decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    uint8x16_t k[11];
    for (int r = 0; r < 11; ++r) k[r] = vld1q_u8(dk_ + 16 * r);
    for (; count >= 8; count -= 8, in += 128, out += 128) {
        uint8x16_t b[8];
        for (int j = 0; j < 8; ++j) b[j] = vld1q_u8(in + 16 * j);
        for (int r = 0; r < 9; ++r)
            for (int j = 0; j < 8; ++j) b[j] = vaesimcq_u8(vaesdq_u8(b[j], k[r]));
        for (int j = 0; j < 8; ++j) vst1q_u8(out + 16 * j, veorq_u8(vaesdq_u8(b[j], k[9]), k[10]));
    }
    for (; count; --count, in += 16, out += 16) {
        uint8x16_t b = vld1q_u8(in);
        for (int r = 0; r < 9; ++r) b = vaesimcq_u8(vaesdq_u8(b, k[r]));
        vst1q_u8(out, veorq_u8(vaesdq_u8(b, k[9]), k[10]));
    }
}
#else
void Aes128::hw_encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    for (; count; --count, in += 16, out += 16) portable_encrypt(in, out);
}
void Aes128::hw_decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    for (; count; --count, in += 16, out += 16) portable_decrypt(in, out);
}
#endif

// The AES instructions are used only after they pass the FIPS-197 example
// (and a multi-block round trip) once; otherwise the table code runs.
bool Aes128::hardware() {
#if defined(ALEMBIC_AES_X86) || defined(ALEMBIC_AES_ARM)
    static const bool ok = [] {
#if defined(ALEMBIC_AES_X86) && !defined(__AES__)
        if (!cpu::features().aes) return false;
#endif
        const std::uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        const std::uint8_t expect[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                         0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
        Aes128 aes(key);
        std::uint8_t blocks[16 * 9];
        for (int j = 0; j < 9; ++j)
            for (int i = 0; i < 16; ++i) blocks[16 * j + i] = static_cast<std::uint8_t>(i * 0x11);
        std::uint8_t out[16 * 9];
        aes.hw_encrypt(blocks, out, 9);
        for (int j = 0; j < 9; ++j)
            if (std::memcmp(out + 16 * j, expect, 16) != 0) return false;
        aes.hw_decrypt(out, out, 9);
        return std::memcmp(out, blocks, sizeof out) == 0;
    }();
    return ok;
#else
    return false;
#endif
}

void Aes128::encrypt_blocks(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    if (hardware()) return hw_encrypt(in, out, count);
    for (; count; --count, in += 16, out += 16) portable_encrypt(in, out);
}

void Aes128::decrypt_blocks(const std::uint8_t* in, std::uint8_t* out, std::size_t count) const {
    if (hardware()) return hw_decrypt(in, out, count);
    for (; count; --count, in += 16, out += 16) portable_decrypt(in, out);
}

void Aes128::encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const { encrypt_blocks(in, out, 1); }
void Aes128::decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const { decrypt_blocks(in, out, 1); }

void Aes128::portable_encrypt(const std::uint8_t in[16], std::uint8_t out[16]) const {
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

void Aes128::portable_decrypt(const std::uint8_t in[16], std::uint8_t out[16]) const {
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

// XTS in pieces of up to 32 blocks: the tweaks of a piece are made first
// (each is the previous one times x in GF(2^128), little-endian), then the
// piece is whitened, run through the cipher in one call and whitened again.
void xts(const Aes128& data_key, const Aes128& tweak_key, std::uint64_t sector, MutableBytes data, bool decrypt) {
    static_assert(std::endian::native == std::endian::little, "the tweak words are XORed as host integers");
    std::uint8_t first[16] = {};
    for (int i = 0; i < 8; ++i) first[i] = static_cast<std::uint8_t>(sector >> (8 * i));
    tweak_key.encrypt_block(first, first);
    std::uint64_t lo = load_le64(first), hi = load_le64(first + 8);
    constexpr std::size_t kPiece = 32;
    alignas(16) std::uint64_t tweaks[kPiece * 2];
    std::uint8_t* p = data.data();
    std::size_t blocks = data.size() / 16;
    while (blocks) {
        std::size_t n = std::min(blocks, kPiece);
        for (std::size_t j = 0; j < n; ++j) {
            tweaks[2 * j] = lo;
            tweaks[2 * j + 1] = hi;
            std::uint64_t carry = hi >> 63;
            hi = (hi << 1) | (lo >> 63);
            lo = (lo << 1) ^ (carry * 0x87);
        }
        auto whiten = [&] {
            for (std::size_t j = 0; j < n; ++j) {
                std::uint64_t w[2];
                std::memcpy(w, p + 16 * j, 16);
                w[0] ^= tweaks[2 * j];
                w[1] ^= tweaks[2 * j + 1];
                std::memcpy(p + 16 * j, w, 16);
            }
        };
        whiten();
        if (decrypt) data_key.decrypt_blocks(p, p, n);
        else data_key.encrypt_blocks(p, p, n);
        whiten();
        p += 16 * n;
        blocks -= n;
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
    // Blocks decrypt independently; each is then XORed with the ciphertext before it.
    constexpr std::size_t kPiece = 32;
    std::uint8_t previous[16], saved[kPiece * 16];
    std::memcpy(previous, iv, 16);
    std::uint8_t* p = data.data();
    for (std::size_t blocks = data.size() / 16; blocks;) {
        std::size_t n = std::min(blocks, kPiece);
        std::memcpy(saved, p, 16 * n);
        key.decrypt_blocks(p, p, n);
        for (std::size_t j = 0; j < n; ++j) {
            const std::uint8_t* x = j == 0 ? previous : saved + 16 * (j - 1);
            for (int i = 0; i < 16; ++i) p[16 * j + i] ^= x[i];
        }
        std::memcpy(previous, saved + 16 * (n - 1), 16);
        p += 16 * n;
        blocks -= n;
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

// ---------------------------------------------------------------- CRC-32C

namespace {

std::uint32_t crc32c_table(const std::uint8_t* p, std::size_t n, std::uint32_t c) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0x82F63B78u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
        return t;
    }();
    for (; n; --n, ++p) c = table[(c ^ *p) & 0xFF] ^ (c >> 8);
    return c;
}

#if defined(ALEMBIC_CRC_X86) || defined(ALEMBIC_CRC_ARM)
ALEMBIC_TARGET_CRC std::uint32_t crc32c_hw(const std::uint8_t* p, std::size_t n, std::uint32_t c) {
    for (; n >= 8; n -= 8, p += 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
#if defined(ALEMBIC_CRC_X86)
        c = static_cast<std::uint32_t>(_mm_crc32_u64(c, v));
#else
        c = __crc32cd(c, v);
#endif
    }
    for (; n; --n, ++p)
#if defined(ALEMBIC_CRC_X86)
        c = _mm_crc32_u8(c, *p);
#else
        c = __crc32cb(c, *p);
#endif
    return c;
}
#endif

} // namespace

namespace {
bool crc_hardware();
}

// The CRC32 instructions are used only after they give the check value once.
std::uint32_t crc32c(Bytes data, std::uint32_t crc) {
    if (crc_hardware()) {
#if defined(ALEMBIC_CRC_X86) || defined(ALEMBIC_CRC_ARM)
        return ~crc32c_hw(data.data(), data.size(), ~crc);
#endif
    }
    return ~crc32c_table(data.data(), data.size(), ~crc);
}

namespace {
bool crc_hardware() {
#if defined(ALEMBIC_CRC_X86) || defined(ALEMBIC_CRC_ARM)
    static const bool hw_ok = [] {
#if defined(ALEMBIC_CRC_X86) && !defined(__SSE4_2__)
        if (!cpu::features().sse42) return false;
#endif
        const char* check = "123456789";
        return ~crc32c_hw(reinterpret_cast<const std::uint8_t*>(check), 9, ~0u) == 0xE3069283u;
    }();
    return hw_ok;
#else
    return false;
#endif
}
} // namespace

std::string accelerations() {
    std::string out;
    auto add = [&](const char* name) { out += (out.empty() ? "" : ", ") + std::string(name); };
    if (Aes128::hardware()) add("AES");
    if (crc_hardware()) add("CRC32C");
    if (Sha256::hardware()) add("SHA-256");
    return out.empty() ? "none" : out;
}

} // namespace alembic::crypto

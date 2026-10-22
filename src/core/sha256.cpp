// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/sha256.hpp"

#include "cpu_features.hpp"

#include <algorithm>
#include <cstring>

// SHA-NI on x86-64 (compiled for it whatever the build targets, used when
// CPUID reports it); the ARMv8 SHA-256 instructions when the build targets
// them (Apple silicon). Both follow the usual structure of these
// instructions (as in Intel's SHA extensions paper and ARM's reference
// manual): four rounds per step, the message schedule four words at a time.
#if defined(__x86_64__)
#define ALEMBIC_SHA_X86 1
#include <immintrin.h>
#elif defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
#define ALEMBIC_SHA_ARM 1
#include <arm_neon.h>
#endif

namespace alembic {

namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

#if defined(ALEMBIC_SHA_X86)
__attribute__((target("sha,sse4.1,ssse3"))) void blocks_x86(std::uint32_t state[8], const std::uint8_t* p, std::size_t count) {
    const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL); // big-endian words
    // The instructions keep the state as ABEF and CDGH.
    __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state)), 0xB1);      // CDAB
    __m128i state1 = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4)), 0x1B); // EFGH
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);                                                      // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);                                                           // CDGH
    for (; count; --count, p += 64) {
        const __m128i abef = state0, cdgh = state1;
        __m128i w[4];
        for (int g = 0; g < 16; ++g) {
            if (g < 4) w[g] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * g)), swap);
            __m128i m = _mm_add_epi32(w[g & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(kK + 4 * g)));
            state1 = _mm_sha256rnds2_epu32(state1, state0, m);
            if (g >= 3 && g < 15) { // the next four schedule words
                __m128i& next = w[(g + 1) & 3];
                next = _mm_add_epi32(next, _mm_alignr_epi8(w[g & 3], w[(g + 3) & 3], 4));
                next = _mm_sha256msg2_epu32(next, w[g & 3]);
            }
            state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(m, 0x0E));
            if (g >= 1 && g <= 12) w[(g + 3) & 3] = _mm_sha256msg1_epu32(w[(g + 3) & 3], w[g & 3]);
        }
        state0 = _mm_add_epi32(state0, abef);
        state1 = _mm_add_epi32(state1, cdgh);
    }
    tmp = _mm_shuffle_epi32(state0, 0x1B);    // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xB1); // DCHG
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), _mm_blend_epi16(tmp, state1, 0xF0)); // DCBA
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), _mm_alignr_epi8(state1, tmp, 8)); // HGFE
}
#elif defined(ALEMBIC_SHA_ARM)
void blocks_arm(std::uint32_t state[8], const std::uint8_t* p, std::size_t count) {
    uint32x4_t state0 = vld1q_u32(state), state1 = vld1q_u32(state + 4);
    for (; count; --count, p += 64) {
        const uint32x4_t abcd = state0, efgh = state1;
        uint32x4_t w[4];
        for (int i = 0; i < 4; ++i) w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
        for (int g = 0; g < 16; ++g) {
            uint32x4_t m = vaddq_u32(w[g & 3], vld1q_u32(kK + 4 * g));
            if (g < 12) w[g & 3] = vsha256su0q_u32(w[g & 3], w[(g + 1) & 3]);
            uint32x4_t a = state0;
            state0 = vsha256hq_u32(state0, state1, m);
            state1 = vsha256h2q_u32(state1, a, m);
            if (g < 12) w[g & 3] = vsha256su1q_u32(w[g & 3], w[(g + 2) & 3], w[(g + 3) & 3]);
        }
        state0 = vaddq_u32(state0, abcd);
        state1 = vaddq_u32(state1, efgh);
    }
    vst1q_u32(state, state0);
    vst1q_u32(state + 4, state1);
}
#endif

bool hardware_ok();

} // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

namespace {
// One 64-byte block, portable.
void compress(std::uint32_t h_[8], const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) | (std::uint32_t(p[i * 4 + 2]) << 8) |
               p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}
} // namespace

void Sha256::block(const std::uint8_t* p) { compress(h_, p); }

void Sha256::blocks(const std::uint8_t* p, std::size_t count) {
#if defined(ALEMBIC_SHA_X86) || defined(ALEMBIC_SHA_ARM)
    if (!portable_only_ && hardware_ok()) {
#if defined(ALEMBIC_SHA_X86)
        return blocks_x86(h_, p, count);
#else
        return blocks_arm(h_, p, count);
#endif
    }
#endif
    for (; count; --count, p += 64) block(p);
}

void Sha256::update(Bytes data) {
    length_ += data.size();
    const std::uint8_t* p = data.data();
    std::size_t n = data.size();
    if (fill_ > 0) {
        std::size_t take = std::min(n, sizeof buffer_ - fill_);
        std::memcpy(buffer_ + fill_, p, take);
        fill_ += take;
        p += take;
        n -= take;
        if (fill_ < sizeof buffer_) return;
        blocks(buffer_, 1);
        fill_ = 0;
    }
    blocks(p, n / 64);
    p += n / 64 * 64;
    n %= 64;
    std::memcpy(buffer_, p, n);
    fill_ = n;
}

Sha256::Digest Sha256::finish() {
    std::uint64_t bits = length_ * 8;
    std::uint8_t pad[72] = {0x80};
    std::size_t pad_length = (fill_ < 56 ? 56 : 120) - fill_;
    for (int i = 0; i < 8; ++i) pad[pad_length + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(Bytes(pad, pad_length + 8));
    Digest digest;
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) digest[i * 4 + k] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * k));
    return digest;
}

Sha256::Digest Sha256::portable(Bytes data) {
    Sha256 h;
    h.portable_only_ = true;
    h.update(data);
    return h.finish();
}

namespace {
// The instructions run only after they agree with the portable code on a
// few blocks of data.
bool hardware_ok() {
#if defined(ALEMBIC_SHA_X86) || defined(ALEMBIC_SHA_ARM)
    static const bool ok = [] {
#if defined(ALEMBIC_SHA_X86) && !defined(__SHA__)
        const auto& f = cpu::features();
        if (!f.sha || !f.sse41 || !f.ssse3) return false;
#endif
        std::uint8_t data[64 * 5 + 7];
        for (std::size_t i = 0; i < sizeof data; ++i) data[i] = static_cast<std::uint8_t>(i * 131 + 7);
        const std::uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::uint32_t a[8], b[8];
        std::memcpy(a, initial, sizeof a);
        std::memcpy(b, initial, sizeof b);
#if defined(ALEMBIC_SHA_X86)
        blocks_x86(a, data, 5);
#else
        blocks_arm(a, data, 5);
#endif
        for (int i = 0; i < 5; ++i) compress(b, data + 64 * i);
        return std::memcmp(a, b, sizeof a) == 0;
    }();
    return ok;
#else
    return false;
#endif
}
} // namespace

bool Sha256::hardware() { return hardware_ok(); }

std::string Sha256::hex(const Digest& digest) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

} // namespace alembic

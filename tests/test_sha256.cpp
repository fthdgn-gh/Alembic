// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/crypto.hpp"
#include "alembic/sha256.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <cstring>

using namespace alembic;

namespace {
std::string sha(const std::string& text) {
    Sha256 h;
    h.update(Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    return Sha256::hex(h.finish());
}
} // namespace

TEST_CASE(sha256_vectors) {
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million 'a's, fed in uneven pieces to exercise buffering.
    Sha256 h;
    std::string chunk(997, 'a');
    std::size_t left = 1000000;
    while (left > 0) {
        std::size_t n = std::min(left, chunk.size());
        h.update(Bytes(reinterpret_cast<const std::uint8_t*>(chunk.data()), n));
        left -= n;
    }
    CHECK(Sha256::hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// The CPU's SHA-256 instructions (when there) against the portable code, for
// every length around the block size and a larger message in odd pieces.
TEST_CASE(sha256_fast_path_matches_portable) {
#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
    CHECK(Sha256::hardware()); // the self-test passed, so the comparisons below cover the instructions
#endif
    std::fprintf(stderr, "  (CPU instructions in use: %s)\n", crypto::accelerations().c_str());
    std::vector<std::uint8_t> data(1 << 20);
    std::uint32_t x = 99;
    for (auto& b : data) b = static_cast<std::uint8_t>((x = x * 1103515245u + 12345u) >> 16);
    for (std::size_t n = 0; n <= 200; ++n) {
        Sha256 h;
        h.update(Bytes(data.data(), n));
        CHECK(h.finish() == Sha256::portable(Bytes(data.data(), n)));
    }
    Sha256 pieces;
    for (std::size_t at = 0; at < data.size();) {
        std::size_t n = std::min<std::size_t>(data.size() - at, 1 + (at * 7919) % 100003);
        pieces.update(Bytes(data.data() + at, n));
        at += n;
    }
    CHECK(pieces.finish() == Sha256::portable(data));
}

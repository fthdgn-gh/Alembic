// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/sha256.hpp"
#include "test.hpp"

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

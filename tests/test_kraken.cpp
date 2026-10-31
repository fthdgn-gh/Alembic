// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/kraken.hpp"
#include "test.hpp"

#include <random>
#include <string>

using namespace alembic;

namespace {

// Encodes `data`, decodes it back and compares. False when the encoder declined.
bool round_trips(const std::vector<std::uint8_t>& data, bool& same) {
    std::optional<kraken::EncodedBlock> e = kraken::encode_block(Bytes(data.data(), data.size()));
    if (!e) return false;
    std::vector<std::uint8_t> back(data.size());
    kraken::DecodeStatus s = kraken::decode_block(Bytes(e->payload.data(), e->payload.size()), e->flags,
                                                  e->first_chunk_size, MutableBytes(back.data(), back.size()));
    same = s == kraken::DecodeStatus::success && back == data && e->payload.size() < data.size();
    return true;
}

std::vector<std::uint8_t> text(std::size_t size, unsigned seed) {
    static const char* words[] = {"alembic ", "package ", "kraken ", "block ", "the ", "of ", "image ", "\n",
                                  "{\"title\": ", "0x40000, ", "folder/", ".bin "};
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> out;
    while (out.size() < size) {
        std::string w = words[rng() % 12];
        out.insert(out.end(), w.begin(), w.end());
        if (rng() % 7 == 0) out.push_back(static_cast<std::uint8_t>(rng()));
    }
    out.resize(size);
    return out;
}

} // namespace

TEST_CASE(kraken_encode_round_trips) {
    const std::size_t sizes[] = {8, 9, 40, 1000, 0x1FFFF, 0x20000, 0x20001, 0x30000, 0x3FFFF, 0x40000};
    for (std::size_t size : sizes) {
        bool same = false;
        // Text compresses: the encoder must accept it, and it must decode back.
        if (size >= 1000) {
            CHECK(round_trips(text(size, static_cast<unsigned>(size)), same));
            CHECK(same);
        }
        // Zeros: long matches through the length escapes.
        std::vector<std::uint8_t> zeros(size, 0);
        if (round_trips(zeros, same)) CHECK(same);
        else CHECK(size < 64);
    }
}

TEST_CASE(kraken_encode_mixed_and_noise) {
    std::mt19937 rng(7);
    // Noise does not shrink: declined.
    std::vector<std::uint8_t> noise(0x40000);
    for (auto& b : noise) b = static_cast<std::uint8_t>(rng());
    CHECK(!kraken::encode_block(Bytes(noise.data(), noise.size())));
    // Text, a long run, noise and a repeat of the first part (a far match across sub-chunks).
    std::vector<std::uint8_t> mixed = text(0x14000, 3);
    mixed.insert(mixed.end(), 0x9000, 0xAB);
    for (int i = 0; i < 0x8000; ++i) mixed.push_back(static_cast<std::uint8_t>(rng()));
    std::vector<std::uint8_t> head(mixed.begin(), mixed.begin() + 0x13000);
    mixed.insert(mixed.end(), head.begin(), head.end());
    mixed.resize(0x40000, 0x11);
    bool same = false;
    CHECK(round_trips(mixed, same));
    CHECK(same);
    // Every byte value, so the Huffman tables use their widest form.
    std::vector<std::uint8_t> all;
    for (int r = 0; r < 600; ++r)
        for (int v = 0; v < 256; ++v) all.push_back(static_cast<std::uint8_t>(r % 3 == 0 ? v : (v * 7) & 0x3F));
    all.resize(0x25000);
    CHECK(round_trips(all, same));
    CHECK(same);
    // Too short or too long: declined.
    std::vector<std::uint8_t> tiny(7, 0), big(0x40001, 0);
    CHECK(!kraken::encode_block(Bytes(tiny.data(), tiny.size())));
    CHECK(!kraken::encode_block(Bytes(big.data(), big.size())));
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/bytes.hpp"
#include "alembic/pfsc.hpp"
#include "test.hpp"

#include <cstring>
#include <random>

using namespace alembic;

namespace {

class VectorSink final : public ByteSink {
public:
    Status write(Bytes data) override {
        bytes.insert(bytes.end(), data.begin(), data.end());
        return {};
    }
    std::vector<std::uint8_t> bytes;
};

// Mixed content: compressible text, random noise, and zeros.
std::vector<std::uint8_t> sample(std::size_t size) {
    std::vector<std::uint8_t> data(size);
    std::mt19937 rng(1234);
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t region = (i / pfsc::kBlockSize) % 3;
        data[i] = region == 0 ? static_cast<std::uint8_t>("alembic "[i % 8])
                : region == 1 ? static_cast<std::uint8_t>(rng())
                              : 0;
    }
    return data;
}

} // namespace

TEST_CASE(pfsc_header_size) {
    CHECK_EQ(pfsc::header_size_for(0), std::uint64_t(0x10000));
    CHECK_EQ(pfsc::header_size_for(1), std::uint64_t(0x10000));
    // 0xFC00 bytes of table space hold 8064 offsets, so 8063 blocks fit.
    CHECK_EQ(pfsc::header_size_for(8063ull * pfsc::kBlockSize), std::uint64_t(0x10000));
    CHECK_EQ(pfsc::header_size_for(8064ull * pfsc::kBlockSize), std::uint64_t(0x20000));
}

TEST_CASE(pfsc_round_trip) {
    std::string dir = test::make_temp_dir();
    std::vector<std::uint8_t> data = sample(5 * pfsc::kBlockSize + 1234);
    test::write_file(dir + "/in.bin", data);

    auto in = File::open(dir + "/in.bin", File::Mode::read);
    REQUIRE_OK(in);
    auto out = File::open(dir + "/out.pfsc", File::Mode::create_new);
    REQUIRE_OK(out);
    auto result = pfsc::encode_file(*in, *out, 0);
    REQUIRE_OK(result);
    CHECK_EQ(result->block_count, 6u);
    // Blocks 0 and 3 (text) and 2, 5 (zeros, final block zero-padded) compress; 1 and 4 are noise.
    CHECK_EQ(result->compressed_block_count, 4u);
    CHECK_EQ(result->padded_logical_length, 6ull * pfsc::kBlockSize);

    auto info = pfsc::inspect(*out);
    REQUIRE_OK(info);
    CHECK_EQ(info->data_offset, std::uint64_t(0x10000));
    CHECK_EQ(info->block_count(), 6u);
    CHECK_EQ(info->block_offsets[2] - info->block_offsets[1], std::uint64_t(pfsc::kBlockSize)); // raw

    VectorSink decoded;
    REQUIRE_OK(pfsc::decode(*out, decoded, data.size()));
    CHECK(decoded.bytes == data);

    VectorSink padded;
    REQUIRE_OK(pfsc::decode(*out, padded));
    CHECK_EQ(padded.bytes.size(), std::size_t(6 * pfsc::kBlockSize));
}

TEST_CASE(pfsc_empty_input) {
    std::string dir = test::make_temp_dir();
    test::write_file(dir + "/in.bin", {});
    auto in = File::open(dir + "/in.bin", File::Mode::read);
    REQUIRE_OK(in);
    auto out = File::open(dir + "/out.pfsc", File::Mode::create_new);
    REQUIRE_OK(out);
    auto result = pfsc::encode_file(*in, *out, 0);
    REQUIRE_OK(result);
    CHECK_EQ(result->block_count, 0u);
    CHECK_EQ(result->stored_length, std::uint64_t(0x10000));
    auto info = pfsc::inspect(*out);
    REQUIRE_OK(info);
    CHECK_EQ(info->block_count(), 0u);
}

TEST_CASE(pfsc_encoder_rejects_wrong_length) {
    std::string dir = test::make_temp_dir();
    auto out = File::open(dir + "/out.pfsc", File::Mode::create_new);
    REQUIRE_OK(out);
    pfsc::Encoder encoder(*out, 0, 10);
    REQUIRE_OK(encoder.begin());
    std::uint8_t bytes[20] = {};
    CHECK(!encoder.write(Bytes(bytes, 20)).ok());
    REQUIRE_OK(encoder.write(Bytes(bytes, 5)));
    CHECK(!encoder.finish().ok());
}

TEST_CASE(pfsc_inspect_rejects_corruption) {
    std::string dir = test::make_temp_dir();
    test::write_file(dir + "/in.bin", sample(3 * pfsc::kBlockSize));
    auto in = File::open(dir + "/in.bin", File::Mode::read);
    REQUIRE_OK(in);
    auto out = File::open(dir + "/out.pfsc", File::Mode::create_new);
    REQUIRE_OK(out);
    REQUIRE_OK(pfsc::encode_file(*in, *out, 0));

    std::uint8_t bad_magic[4] = {'X', 'X', 'X', 'X'};
    REQUIRE_OK(out->write_at(0, Bytes(bad_magic, 4)));
    CHECK(!pfsc::inspect(*out).ok());
    std::uint8_t magic[4];
    put_u32(magic, pfsc::kMagic);
    REQUIRE_OK(out->write_at(0, Bytes(magic, 4)));
    REQUIRE_OK(pfsc::inspect(*out));

    // Point block 1 past the end of the file.
    std::uint8_t offset[8];
    put_u64(offset, 1ull << 40);
    REQUIRE_OK(out->write_at(pfsc::kOffsetTableOffset + 8, Bytes(offset, 8)));
    CHECK(!pfsc::inspect(*out).ok());
}

TEST_CASE(pfsc_reader_read_ahead) {
    std::string dir = test::make_temp_dir();
    const std::uint32_t blocks = 600; // more than the read-ahead window
    std::vector<std::uint8_t> data = sample(std::size_t(blocks) * pfsc::kBlockSize - 777);
    test::write_file(dir + "/in.bin", data);
    {
        auto in = File::open(dir + "/in.bin", File::Mode::read);
        REQUIRE_OK(in);
        auto out = File::open(dir + "/out.pfsc", File::Mode::create_new);
        REQUIRE_OK(out);
        REQUIRE_OK(pfsc::encode_file(*in, *out, 0));
    }
    auto open = [&](bool ahead) -> std::shared_ptr<pfsc::Reader> {
        auto f = File::open(dir + "/out.pfsc", File::Mode::read);
        if (!f.ok()) return nullptr;
        auto r = pfsc::Reader::open(std::make_shared<File>(std::move(*f)), 0, std::nullopt, data.size());
        if (!r.ok()) return nullptr;
        (*r)->set_read_ahead(ahead);
        return *r;
    };

    for (bool ahead : {true, false}) {
        auto reader = open(ahead);
        CHECK(reader);
        if (!(reader)) return;
        // Front to back in odd-sized pieces that straddle blocks.
        std::vector<std::uint8_t> got(data.size());
        for (std::size_t at = 0; at < data.size();) {
            std::size_t n = std::min<std::size_t>(100003, data.size() - at);
            REQUIRE_OK(reader->read_at(at, MutableBytes(got.data() + at, n)));
            at += n;
        }
        CHECK(got == data);
        // Jumps back and forth, then forward again from the middle.
        std::mt19937 rng(7);
        for (int i = 0; i < 200; ++i) {
            std::size_t at = rng() % (data.size() - 4096);
            std::uint8_t piece[4096];
            REQUIRE_OK(reader->read_at(at, MutableBytes(piece, sizeof piece)));
            CHECK(std::memcmp(piece, data.data() + at, sizeof piece) == 0);
        }
        std::size_t middle = 250 * pfsc::kBlockSize + 5;
        std::vector<std::uint8_t> tail(data.size() - middle);
        REQUIRE_OK(reader->read_at(middle, tail));
        CHECK(std::memcmp(tail.data(), data.data() + middle, tail.size()) == 0);
    }

    // A damaged block fails with its own index; the blocks before it read.
    auto raw = File::open(dir + "/out.pfsc", File::Mode::read);
    REQUIRE_OK(raw);
    auto info = pfsc::inspect(*raw);
    REQUIRE_OK(info);
    const std::uint32_t bad = 300; // text: compressed
    CHECK(info->block_offsets[bad + 1] - info->block_offsets[bad] < pfsc::kBlockSize);
        if (!(info->block_offsets[bad + 1] - info->block_offsets[bad] < pfsc::kBlockSize)) return;
    {
        auto f = File::open(dir + "/out.pfsc", File::Mode::read_write);
        REQUIRE_OK(f);
        std::uint8_t junk[16];
        std::memset(junk, 0xA5, sizeof junk);
        REQUIRE_OK(f->write_at(info->block_offsets[bad] + 4, Bytes(junk, sizeof junk)));
    }
    std::string messages[2];
    for (bool ahead : {true, false}) {
        auto reader = open(ahead);
        CHECK(reader);
        if (!(reader)) return;
        std::vector<std::uint8_t> block(pfsc::kBlockSize);
        std::uint32_t i = 0;
        Status status;
        for (; i < blocks; ++i) {
            status = reader->read_at(std::uint64_t(i) * pfsc::kBlockSize, MutableBytes(block.data(), 4096));
            if (!status.ok()) break;
        }
        CHECK_EQ(i, bad);
        CHECK(!status.ok());
        if (!(!status.ok())) return;
        messages[ahead] = status.error().message;
    }
    CHECK(messages[0] == messages[1]);
    CHECK(messages[1].find("block 300") != std::string::npos);
}

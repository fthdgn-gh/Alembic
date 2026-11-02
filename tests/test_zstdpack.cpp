// SPDX-License-Identifier: GPL-3.0-or-later
// zstd asset packs. Byte parity with ps5-dump-forge is for
// tools/zstd-oracle/compare.sh; these tests check the pieces against
// ampr-emu-ng's docs/zstd-format.md on their own (header fields read back at
// their documented offsets, CRCs from zlib, frames looked at with libzstd).
#include "alembic/convert.hpp"
#include "alembic/zstdpack.hpp"
#include "test.hpp"

#define ZSTD_STATIC_LINKING_ONLY // ZSTD_getFrameHeader
#include "zstd.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <string>

using namespace alembic;
using namespace alembic::zstdpack;

namespace {

constexpr std::size_t kChunk = 262144;

std::uint32_t crc_of(const std::uint8_t* p, std::size_t n) {
    return static_cast<std::uint32_t>(::crc32(::crc32(0, nullptr, 0), p, static_cast<uInt>(n)));
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint64_t le64(const std::vector<std::uint8_t>& b, std::size_t at) {
    return le32(b, at) | (static_cast<std::uint64_t>(le32(b, at + 4)) << 32);
}

std::vector<std::uint8_t> text(std::size_t size, unsigned seed) {
    static const char* words[] = {"mesh ", "level ", "texture ", "0.25 ", "{\n", "}\n", "alpha "};
    std::vector<std::uint8_t> out;
    std::uint32_t x = seed * 2654435761u + 1;
    while (out.size() < size) {
        x = x * 1103515245u + 12345u;
        for (const char* c = words[(x >> 16) % 7]; *c && out.size() < size; ++c) out.push_back(static_cast<std::uint8_t>(*c));
    }
    return out;
}

std::vector<std::uint8_t> noise(std::size_t size, unsigned seed) {
    std::vector<std::uint8_t> out(size);
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 7;
    for (auto& b : out) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        b = static_cast<std::uint8_t>(x);
    }
    return out;
}

std::vector<std::uint8_t> concat(std::vector<std::uint8_t> a, const std::vector<std::uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Three chunks of noise and a fourth that starts with `zeros` zero bytes:
// ps5-dump-forge's edge files around the 1% store-only line.
std::vector<std::uint8_t> edge(std::size_t zeros) {
    std::vector<std::uint8_t> last = noise(kChunk, 9);
    std::fill(last.begin(), last.begin() + static_cast<std::ptrdiff_t>(zeros), 0);
    return concat(noise(3 * kChunk, 8), last);
}

std::string make_game(const std::string& parent) {
    std::string root = parent + "/game";
    for (const char* d : {"", "/sce_sys", "/fakelib", "/data", "/data/sub", "/save"}) test::make_dir(root + d);
    test::write_file(root + "/eboot.bin", concat(noise(70000, 1), {'l', 'i', 'b', 'S', 'c', 'e', 'A', 'm', 'p', 'r'}));
    test::write_file(root + "/sce_sys/param.json", text(300, 2));
    test::write_file(root + "/fakelib/libSceAmpr.sprx", noise(5000, 3));
    test::write_file(root + "/data/text.bin", text(6 * kChunk + 77, 4));
    test::write_file(root + "/data/noise.bin", noise(3 * kChunk + 5, 5));
    test::write_file(root + "/data/exact.bin", text(kChunk, 6));
    test::write_file(root + "/data/plus1.bin", text(kChunk + 1, 7));
    test::write_file(root + "/data/small.bin", text(1000, 8));
    test::write_file(root + "/data/empty.bin", {});
    test::write_file(root + "/data/sub/edge-store.bin", edge(4 << 10));
    test::write_file(root + "/data/sub/edge-packed.bin", edge(32 << 10));
    test::write_file(root + "/data/conf.json", text(2000, 9)); // keep-loose suffix
    test::write_file(root + "/save/x.bin", text(2000, 10));    // protected folder
    test::write_file(root + "/root.bin", text(3000, 11));      // root files stay loose
    return root;
}

Result<std::vector<std::uint8_t>> read(const tree::Source& source, const std::string& path) {
    const tree::Node* node = tree::find(source.root(), path);
    if (!node) return make_error(Errc::invalid_argument, "missing " + path);
    ALEMBIC_ASSIGN(auto file, source.open(*node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node->size));
    ALEMBIC_TRY(file->read_at(0, data));
    return data;
}

constexpr ArchiveId kId = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

PackOptions options_for(unsigned workers = 3) {
    PackOptions o;
    o.archive_id = kId;
    o.timestamp = 1700000000;
    o.workers = workers;
    return o;
}

const char* kPackedFiles[] = {"data/text.bin",  "data/noise.bin",          "data/exact.bin",
                              "data/plus1.bin", "data/small.bin",          "data/sub/edge-store.bin",
                              "data/sub/edge-packed.bin"};

} // namespace

TEST_CASE(zstd_path_hash_is_fnv1a_64) {
    CHECK(path_hash("") == 14695981039346656037ull);
    CHECK(path_hash("a") == 0xAF63DC4C8601EC8Cull);
    CHECK(path_hash("data/level1.bin") != path_hash("Data/level1.bin")); // case kept
    CHECK(valid_path("a/b.bin"));
    CHECK(!valid_path("/a"));
    CHECK(!valid_path("a//b"));
    CHECK(!valid_path("a/./b"));
    CHECK(!valid_path("a/../b"));
    CHECK(!valid_path("a\\b"));
    CHECK(volume_name(0) == "ampr_zstd-000.pak");
    CHECK(volume_name(12) == "ampr_zstd-012.pak");
    CHECK(volume_name(1234) == "ampr_zstd-1234.pak");
}

TEST_CASE(zstd_chunk_record_layout) {
    Chunk c{0x12345678, 0xABC, static_cast<std::uint32_t>(kChunk), Codec::zstd};
    auto word = pack_chunk(c);
    REQUIRE_OK(word);
    CHECK((*word & 0xFFFFFFFFull) == 0x12345678ull);
    CHECK(((*word >> 32) & 0xFFF) == 0xABC);
    CHECK(((*word >> 44) & 0x3FFFF) == kChunk - 1);
    CHECK((*word >> 62) == 1);
    auto back = unpack_chunk(*word);
    REQUIRE_OK(back);
    CHECK(*back == c);
    CHECK(!unpack_chunk((*word & ~(3ull << 62)) | (2ull << 62)).ok()); // codec 2
    CHECK(!pack_chunk({64, 0, static_cast<std::uint32_t>(kChunk) + 1, Codec::raw}).ok());
    CHECK(!pack_chunk({63, 0, 1, Codec::raw}).ok()); // inside the header
    CHECK(!pack_chunk({64, 4096, 1, Codec::raw}).ok());
}

TEST_CASE(zstd_volume_header_fields) {
    VolumeHeader h{2, 3, kId, 1234};
    auto bytes = encode_volume_header(h);
    REQUIRE_OK(bytes);
    std::vector<std::uint8_t> b(bytes->begin(), bytes->end());
    CHECK(std::memcmp(b.data(), "AMPRZDT1", 8) == 0);
    CHECK_EQ(le32(b, 8), 1u);
    CHECK_EQ(le32(b, 12), 64u);
    CHECK_EQ(le32(b, 16), 2u);
    CHECK_EQ(le32(b, 20), 3u);
    CHECK(std::equal(kId.begin(), kId.end(), b.begin() + 24));
    CHECK_EQ(le64(b, 40), 1234ull);
    CHECK(std::all_of(b.begin() + 48, b.begin() + 60, [](std::uint8_t x) { return x == 0; }));
    std::vector<std::uint8_t> z = b;
    std::fill(z.begin() + 60, z.end(), 0);
    CHECK_EQ(le32(b, 60), crc_of(z.data(), 64));
    auto back = parse_volume_header(b);
    REQUIRE_OK(back);
    CHECK(*back == h);
    std::vector<std::uint8_t> bad = b;
    bad[30] ^= 1;
    CHECK(!parse_volume_header(bad).ok()); // CRC
    CHECK(!encode_volume_header({3, 3, kId, 1}).ok());
    CHECK(!encode_volume_header({0, 0, kId, 1}).ok());
}

TEST_CASE(zstd_index_fields_and_damage) {
    // Two files in (hash, path) order: a store-only one of two raw chunks and
    // a zstd one, in one volume.
    Index ix;
    ix.archive_id = kId;
    ix.level = 3;
    FileRecord a{"raw.bin", kChunk + 5, 0, true}, z{"zstd/packed.bin", 1000, 0, false};
    std::vector<FileRecord> files{a, z};
    std::sort(files.begin(), files.end(), [](const FileRecord& x, const FileRecord& y) {
        return path_hash(x.path) != path_hash(y.path) ? path_hash(x.path) < path_hash(y.path) : x.path < y.path;
    });
    std::uint32_t cursor = 64;
    for (FileRecord& f : files) {
        f.first_chunk = static_cast<std::uint32_t>(ix.chunks.size());
        if (f.store_only) {
            ix.chunks.push_back({cursor, 0, static_cast<std::uint32_t>(kChunk), Codec::raw});
            cursor += kChunk;
            ix.chunks.push_back({cursor, 0, 5, Codec::raw});
            cursor += 5;
        } else {
            ix.chunks.push_back({cursor, 0, 40, Codec::zstd});
            cursor += 40;
        }
        ix.files.push_back(f);
    }
    ix.volume_sizes = {cursor};
    auto bytes = encode_index(ix);
    REQUIRE_OK(bytes);
    const std::vector<std::uint8_t>& b = *bytes;
    CHECK(std::memcmp(b.data(), "AMPRZPK1", 8) == 0);
    CHECK_EQ(le32(b, 8), 1u);
    CHECK_EQ(le32(b, 12), 128u);
    CHECK_EQ(int{b[16]}, 18);
    CHECK_EQ(int{b[17]}, 3);
    CHECK_EQ(le32(b, 20), 1u);
    CHECK(std::equal(kId.begin(), kId.end(), b.begin() + 24));
    CHECK_EQ(le64(b, 40), 2ull);
    CHECK_EQ(le64(b, 48), 3ull);
    CHECK_EQ(le64(b, 56), 128ull);
    CHECK_EQ(le64(b, 64), 128ull + 64);
    CHECK_EQ(le64(b, 72), 128ull + 64 + 24);
    CHECK_EQ(le64(b, 80), 128ull + 64 + 24 + 8);
    CHECK_EQ(le64(b, 88), 22ull);
    CHECK_EQ(le64(b, 96), kChunk + 5 + 1000);
    CHECK_EQ(b.size(), 128u + 64 + 24 + 8 + 22);
    CHECK_EQ(le32(b, 112), crc_of(b.data() + 128, b.size() - 128));
    std::vector<std::uint8_t> h(b.begin(), b.begin() + 128);
    std::fill(h.begin() + 112, h.begin() + 120, 0);
    CHECK_EQ(le32(b, 116), crc_of(h.data(), 128));
    // File records: hash, size, first chunk, path offset/length, flags.
    for (std::size_t i = 0; i < 2; ++i) {
        std::size_t r = 128 + 32 * i;
        CHECK(le64(b, r) == path_hash(files[i].path));
        CHECK(le64(b, r + 8) == files[i].size);
        CHECK_EQ(le32(b, r + 16), files[i].first_chunk);
        CHECK_EQ(b[r + 24] | (b[r + 25] << 8), static_cast<int>(files[i].path.size()));
        CHECK_EQ(b[r + 26], files[i].store_only ? 1 : 0);
        std::string path(b.begin() + 224 + le32(b, r + 20), b.begin() + 224 + le32(b, r + 20) + files[i].path.size());
        CHECK(path == files[i].path);
    }
    CHECK(le64(b, 216) == cursor);
    auto back = parse_index(b);
    REQUIRE_OK(back);
    CHECK(*back == ix);

    // Damage, each with both CRCs made right again: the checks, not only the
    // CRCs, refuse it.
    auto fix = [](std::vector<std::uint8_t>& d) {
        std::uint32_t p = crc_of(d.data() + 128, d.size() - 128);
        for (int i = 0; i < 4; ++i) d[112 + i] = static_cast<std::uint8_t>(p >> (8 * i));
        std::vector<std::uint8_t> hh(d.begin(), d.begin() + 128);
        std::fill(hh.begin() + 112, hh.begin() + 120, 0);
        std::uint32_t c = crc_of(hh.data(), 128);
        for (int i = 0; i < 4; ++i) d[116 + i] = static_cast<std::uint8_t>(c >> (8 * i));
    };
    for (std::size_t at : {std::size_t{128}, std::size_t{128 + 26}, std::size_t{128 + 28}, std::size_t{96}, std::size_t{16},
                           std::size_t{18}, std::size_t{104}, std::size_t{120}, std::size_t{56}, std::size_t{88}}) {
        std::vector<std::uint8_t> d = b;
        d[at] ^= 2;
        fix(d);
        CHECK(!parse_index(d).ok());
    }
    std::vector<std::uint8_t> level0 = b;
    level0[17] = 0;
    fix(level0);
    CHECK(!parse_index(level0).ok());
    std::vector<std::uint8_t> stale = b;
    stale[30] ^= 1; // a header byte, CRCs left as they were
    CHECK(!parse_index(stale).ok());
    stale = b;
    stale.back() ^= 1; // a path byte: payload CRC
    CHECK(!parse_index(stale).ok());
    // A zstd chunk as large as its data, or out of order files, are refused.
    Index bigger = ix;
    for (Chunk& c : bigger.chunks)
        if (c.codec == Codec::zstd) c.stored_size = 1000;
    bigger.volume_sizes[0] += 960;
    CHECK(!encode_index(bigger).ok());
    Index swapped = ix;
    std::swap(swapped.files[0], swapped.files[1]);
    CHECK(!encode_index(swapped).ok());
}

TEST_CASE(zstd_chunk_frames) {
    Encoder encoder;
    Decoder decoder;
    std::vector<std::uint8_t> frame, back;
    for (std::size_t size : {std::size_t{1000}, kChunk, kChunk / 2 + 3}) {
        std::vector<std::uint8_t> raw = text(size, 1);
        for (int level : {1, 5, 19, 5}) {
            auto smaller = encoder.compress(raw, level, frame);
            REQUIRE_OK(smaller);
            CHECK(*smaller);
            CHECK(frame.size() < raw.size());
            // One frame: content size, checksum, no dictionary, window <= 256 KiB.
            ZSTD_frameHeader h;
            CHECK_EQ(ZSTD_getFrameHeader(&h, frame.data(), frame.size()), 0u);
            CHECK(h.frameContentSize == size);
            CHECK(h.checksumFlag == 1);
            CHECK(h.dictID == 0);
            CHECK(h.windowSize <= kChunk);
            CHECK(ZSTD_findFrameCompressedSize(frame.data(), frame.size()) == frame.size());
            back.assign(size, 0);
            CHECK(ZSTD_decompress(back.data(), back.size(), frame.data(), frame.size()) == size);
            CHECK(back == raw);
            REQUIRE_OK(decoder.decode(Codec::zstd, frame, back));
            CHECK(back == raw);
        }
    }
    // Noise does not get smaller: stored raw.
    auto smaller = encoder.compress(noise(kChunk, 2), 5, frame);
    REQUIRE_OK(smaller);
    CHECK(!*smaller);
    // The decoder refuses a frame with a wrong checksum.
    std::vector<std::uint8_t> raw = text(5000, 3);
    REQUIRE_OK(encoder.compress(raw, 5, frame));
    frame.back() ^= 1;
    back.assign(raw.size(), 0);
    CHECK(!decoder.decode(Codec::zstd, frame, back).ok());
}

TEST_CASE(zstd_default_selection) {
    auto f = [](const char* p) { return default_selected(p, 10); };
    CHECK(f("data/a.bin"));
    CHECK(f("a/b/c.bin"));
    CHECK(!f("root.bin"));
    CHECK(!f("data/a.PNG"));
    CHECK(!f("data/a.json"));
    CHECK(!f("data/a.utoc"));
    CHECK(!f("data/system/a.bin"));
    CHECK(!f("system/a.bin"));
    CHECK(!f("decrypted/a.bin"));
    CHECK(f("data/decrypted/a.bin"));
    CHECK(f("data/_DUPLEX_/a.bin"));
    CHECK(!f("_DUPLEX_/a.bin"));
    CHECK(!f("data/eboot.bin"));
    CHECK(!f("d/a.bik"));
    CHECK(!f("sb/plugins/x.UPluginManifest"));
    CHECK(!f("sce_sys/x.bin"));
    CHECK(!f("fakelib2/x.bin"));
    CHECK(!f("data/x.SPRX"));
    CHECK(!default_selected("data/a.bin", 0));
    CHECK(always_loose("ampr_zstd.index"));
    CHECK(always_loose("AMPR_ZSTD-003.PAK"));
    CHECK(always_loose("ampr_emu.index"));
    CHECK(!always_loose("data/ampr_zstd.index"));
}

TEST_CASE(zstd_pack_round_trip) {
    auto dir = test::make_temp_dir();
    auto folder = tree::open_folder(make_game(dir));
    REQUIRE_OK(folder);
    auto packed = pack(**folder, options_for());
    REQUIRE_OK(packed);
    const PackStats& st = (*packed)->stats();
    CHECK_EQ(st.files_packed, 7ull);
    CHECK_EQ(st.volumes, 1u);
    const tree::Node& root = (*packed)->root();
    for (const char* p : kPackedFiles) CHECK(tree::find(root, p) == nullptr);
    for (const char* p : {"eboot.bin", "sce_sys/param.json", "data/empty.bin", "data/conf.json", "save/x.bin", "root.bin",
                          "ampr_emu.index", "ampr_zstd.index", "ampr_zstd-000.pak", "fakelib/libSceAmpr.sprx"})
        CHECK(tree::find(root, p) != nullptr);

    // The index, read back here, and the files it describes.
    auto index_bytes = read(**packed, kIndexName);
    REQUIRE_OK(index_bytes);
    auto ix = parse_index(*index_bytes);
    REQUIRE_OK(ix);
    CHECK(ix->archive_id == kId);
    CHECK_EQ(int{ix->level}, kDefaultLevel);
    auto record = [&](const std::string& path) -> const FileRecord* {
        for (const FileRecord& f : ix->files)
            if (f.path == path) return &f;
        return nullptr;
    };
    const FileRecord* exact = record("data/exact.bin");
    const FileRecord* plus1 = record("data/plus1.bin");
    const FileRecord* noise_file = record("data/noise.bin");
    const FileRecord* store = record("data/sub/edge-store.bin");
    const FileRecord* packed_edge = record("data/sub/edge-packed.bin");
    REQUIRE_OK(exact && plus1 && noise_file && store && packed_edge ? Status{} : Status(make_error(Errc::invalid_data, "records")));
    // 256 KiB is one chunk, 256 KiB + 1 two; the 1-byte tail is raw.
    CHECK(ix->chunks[exact->first_chunk].codec == Codec::zstd);
    CHECK(exact->first_chunk + 1 == (exact == &ix->files.back() ? ix->chunks.size() : (exact + 1)->first_chunk));
    CHECK(ix->chunks[plus1->first_chunk].codec == Codec::zstd);
    CHECK(ix->chunks[plus1->first_chunk + 1].codec == Codec::raw);
    CHECK_EQ(ix->chunks[plus1->first_chunk + 1].stored_size, 1u);
    // Incompressible: store-only, every chunk raw at its own size.
    CHECK(noise_file->store_only);
    for (std::uint32_t k = 0; k < 4; ++k) {
        const Chunk& c = ix->chunks[noise_file->first_chunk + k];
        CHECK(c.codec == Codec::raw);
        CHECK_EQ(c.stored_size, k < 3 ? kChunk : 5u);
    }
    // The 1% line (ps5-dump-forge's edge files).
    CHECK(store->store_only);
    CHECK(!packed_edge->store_only);
    CHECK(ix->chunks[packed_edge->first_chunk + 3].codec == Codec::zstd);
    // Chunks back to back from offset 64.
    std::uint64_t at = 64;
    for (const Chunk& c : ix->chunks) {
        CHECK_EQ(std::uint64_t{c.offset}, at);
        at += c.stored_size;
    }
    CHECK_EQ(ix->volume_sizes[0], at);

    // The volume: its header, and every chunk at its place decodes to the file.
    auto volume = read(**packed, "ampr_zstd-000.pak");
    REQUIRE_OK(volume);
    CHECK_EQ(volume->size(), at);
    auto header = parse_volume_header(*volume);
    REQUIRE_OK(header);
    CHECK(header->archive_id == kId);
    CHECK_EQ(header->id, 0u);
    CHECK_EQ(header->count, 1u);
    CHECK_EQ(header->payload_bytes, at - 64);
    Decoder decoder;
    for (const FileRecord& f : ix->files) {
        auto original = read(**folder, f.path);
        REQUIRE_OK(original);
        CHECK_EQ(original->size(), f.size);
        for (std::uint64_t k = 0; k < chunk_count(f.size); ++k) {
            const Chunk& c = ix->chunks[f.first_chunk + k];
            std::vector<std::uint8_t> raw(static_cast<std::size_t>(chunk_raw_size(f.size, k)));
            REQUIRE_OK(decoder.decode(c.codec, Bytes(volume->data() + c.offset, c.stored_size), raw));
            CHECK(std::equal(raw.begin(), raw.end(), original->begin() + static_cast<std::ptrdiff_t>(k * kChunk)));
        }
    }
    // Reading from the middle gives the same bytes as reading it whole.
    const tree::Node* vnode = tree::find(root, "ampr_zstd-000.pak");
    auto vfile = (*packed)->open(*vnode);
    REQUIRE_OK(vfile);
    std::vector<std::uint8_t> part(300000);
    REQUIRE_OK((*vfile)->read_at(40, part));
    CHECK(std::equal(part.begin(), part.end(), volume->begin() + 40));

    auto verified = verify(**packed);
    REQUIRE_OK(verified);
    CHECK_EQ(verified->files, 7ull);
    auto unpacked = unpack(**packed);
    REQUIRE_OK(unpacked);
    for (const char* p : kPackedFiles) {
        auto a = read(**folder, p);
        auto b = read(**unpacked, p);
        REQUIRE_OK(a);
        REQUIRE_OK(b);
        CHECK(*a == *b);
    }
    CHECK(tree::find((*unpacked)->root(), kIndexName) == nullptr);
    CHECK(tree::find((*unpacked)->root(), "ampr_zstd-000.pak") == nullptr);
}

TEST_CASE(zstd_pack_empty_files_and_a_chosen_set) {
    auto dir = test::make_temp_dir();
    auto folder = tree::open_folder(make_game(dir));
    REQUIRE_OK(folder);
    PackOptions o = options_for();
    o.packed_set = std::vector<std::string>{"data/empty.bin", "root.bin", "data/small.bin", "eboot.bin"};
    auto packed = pack(**folder, o);
    REQUIRE_OK(packed);
    const Index& ix = (*packed)->index();
    CHECK_EQ(ix.files.size(), 3u); // eboot.bin stays loose whatever the set says
    for (const FileRecord& f : ix.files)
        if (f.path == "data/empty.bin") CHECK_EQ(f.size, 0ull);
    CHECK_EQ(ix.chunks.size(), 2u);
    CHECK(tree::find((*packed)->root(), "eboot.bin") != nullptr);
    CHECK(tree::find((*packed)->root(), "data/empty.bin") == nullptr);
    auto unpacked = unpack(**packed);
    REQUIRE_OK(unpacked);
    auto empty = read(**unpacked, "data/empty.bin");
    REQUIRE_OK(empty);
    CHECK(empty->empty());
    auto root_file = read(**unpacked, "root.bin");
    auto original = read(**folder, "root.bin");
    REQUIRE_OK(root_file);
    REQUIRE_OK(original);
    CHECK(*root_file == *original);
}

TEST_CASE(zstd_pack_rolls_volumes_over) {
    auto dir = test::make_temp_dir();
    auto folder = tree::open_folder(make_game(dir));
    REQUIRE_OK(folder);
    PackOptions o = options_for();
    o.volume_limit = 64 + kChunk + 1000; // a raw full chunk fits once
    auto packed = pack(**folder, o);
    REQUIRE_OK(packed);
    const Index& ix = (*packed)->index();
    CHECK(ix.volume_sizes.size() > 5);
    CHECK_EQ((*packed)->stats().volumes, static_cast<std::uint32_t>(ix.volume_sizes.size()));
    for (std::size_t v = 0; v < ix.volume_sizes.size(); ++v) {
        CHECK(ix.volume_sizes[v] <= o.volume_limit);
        auto bytes = read(**packed, volume_name(static_cast<std::uint32_t>(v)));
        REQUIRE_OK(bytes);
        auto h = parse_volume_header(*bytes);
        REQUIRE_OK(h);
        CHECK_EQ(h->id, static_cast<std::uint32_t>(v));
        CHECK_EQ(h->count, static_cast<std::uint32_t>(ix.volume_sizes.size()));
        CHECK_EQ(bytes->size(), ix.volume_sizes[v]);
    }
    // A chunk that does not fit starts the next volume right after its header.
    for (std::size_t i = 1; i < ix.chunks.size(); ++i)
        if (ix.chunks[i].volume != ix.chunks[i - 1].volume) {
            CHECK_EQ(ix.chunks[i].volume, ix.chunks[i - 1].volume + 1);
            CHECK_EQ(ix.chunks[i].offset, 64u);
            CHECK(ix.chunks[i - 1].offset + ix.chunks[i - 1].stored_size + ix.chunks[i].stored_size > o.volume_limit);
        }
    auto index_bytes = read(**packed, kIndexName);
    REQUIRE_OK(index_bytes);
    REQUIRE_OK(parse_index(*index_bytes, o.volume_limit));
    // Too small a limit for the real format: the reader with the real limit
    // still accepts it (volumes may end short of the limit).
    REQUIRE_OK(parse_index(*index_bytes));
    auto unpacked = unpack(**packed);
    REQUIRE_OK(unpacked);
    for (const char* p : kPackedFiles) {
        auto a = read(**folder, p);
        auto b = read(**unpacked, p);
        REQUIRE_OK(a);
        REQUIRE_OK(b);
        CHECK(*a == *b);
    }
}

TEST_CASE(zstd_pack_is_deterministic_and_takes_its_archive_id) {
    auto dir = test::make_temp_dir();
    auto folder = tree::open_folder(make_game(dir));
    REQUIRE_OK(folder);
    std::vector<std::uint8_t> index, volume;
    for (unsigned workers : {1u, 4u}) {
        auto packed = pack(**folder, options_for(workers));
        REQUIRE_OK(packed);
        auto i = read(**packed, kIndexName);
        auto v = read(**packed, "ampr_zstd-000.pak");
        REQUIRE_OK(i);
        REQUIRE_OK(v);
        if (index.empty()) index = *i, volume = *v;
        else CHECK(index == *i && volume == *v);
    }
    // Without one, a random ID: everything else stays the same.
    PackOptions o = options_for();
    o.archive_id.reset();
    auto packed = pack(**folder, o);
    REQUIRE_OK(packed);
    CHECK((*packed)->index().archive_id != kId);
    auto v = read(**packed, "ampr_zstd-000.pak");
    REQUIRE_OK(v);
    CHECK(v->size() == volume.size() && std::equal(v->begin() + 64, v->end(), volume.begin() + 64));
}

TEST_CASE(zstd_pack_needs_a_runtime_and_a_level) {
    auto dir = test::make_temp_dir();
    std::string root = make_game(dir);
    test::write_file(root + "/fakelib/keep.txt", {'x'});
    REQUIRE_OK(remove_file(root + "/fakelib/libSceAmpr.sprx"));
    auto folder = tree::open_folder(root);
    REQUIRE_OK(folder);
    CHECK(!pack(**folder, options_for()).ok());
    PackOptions o = options_for();
    auto runtime = File::open(root + "/eboot.bin", File::Mode::read);
    REQUIRE_OK(runtime);
    o.runtime_module = std::make_shared<File>(std::move(*runtime));
    REQUIRE_OK(pack(**folder, o));
    o.level = 23;
    CHECK(!pack(**folder, o).ok());
}

TEST_CASE(zstd_convert_through_images_and_back) {
    auto dir = test::make_temp_dir();
    std::string game = make_game(dir);
    convert::Options o;
    o.zstd = options_for();
    for (auto [name, format] : {std::pair{"/packed.exfat", convert::Format::exfat}, std::pair{"/packed.ffpkg", convert::Format::ffpkg},
                                std::pair{"/packed/", convert::Format::folder}}) {
        auto report = convert::run(game, dir + name, format, o);
        REQUIRE_OK(report);
        REQUIRE_OK(report->zstd ? Status{} : Status(make_error(Errc::invalid_data, "no zstd stats")));
        CHECK_EQ(report->zstd->files_packed, 7ull);
        auto input = convert::open(dir + name);
        REQUIRE_OK(input);
        CHECK(is_packed(input->tree->root()));
        REQUIRE_OK(verify(*input->tree));
        // The image keeps the packer's ampr_emu.index (it lists the packed files).
        auto kept = read(*input->tree, "ampr_emu.index");
        REQUIRE_OK(kept);
        auto same = convert::compare(game, dir + name);
        REQUIRE_OK(same);
        CHECK(same->only_in_a.empty());
        CHECK(same->different.empty());
    }
    // Unpacked again, to a folder: the original files.
    convert::Options u;
    u.zstd_unpack = true;
    REQUIRE_OK(convert::run(dir + "/packed.exfat", dir + "/plain/", convert::Format::folder, u));
    auto plain = tree::open_folder(dir + "/plain");
    REQUIRE_OK(plain);
    CHECK(!is_packed((*plain)->root()));
    for (const char* p : kPackedFiles) {
        auto a = read(**plain, p);
        REQUIRE_OK(a);
        auto b = test::read_file(game + "/" + p);
        CHECK(*a == b);
    }
    // Not for packages, nor with LZ4.
    CHECK(!convert::run(game, dir + "/x.pkg", convert::Format::fpkg, o).ok());
    convert::Options both = o;
    both.lz4 = lz4pack::PackOptions{};
    CHECK(!convert::run(game, dir + "/y.exfat", convert::Format::exfat, both).ok());
}

TEST_CASE(zstd_from_lz4_packs_keeps_the_lz4_set) {
    auto dir = test::make_temp_dir();
    std::string game = make_game(dir);
    convert::Options l;
    lz4pack::PackOptions lz;
    // Only data/text.bin and data/small.bin go into the LZ4 packs.
    lz.config.default_action = lz4pack::Action::loose;
    lz4pack::Rule rule;
    rule.include = {"data/text.bin", "data/small.bin"};
    lz.config.rules.push_back(rule);
    lz.config.auto_loose_large_files = false;
    lz.timestamp = 1700000000;
    l.lz4 = lz;
    REQUIRE_OK(convert::run(game, dir + "/lz4/", convert::Format::folder, l));
    convert::Options o;
    o.zstd = options_for();
    auto report = convert::run(dir + "/lz4", dir + "/zstd/", convert::Format::folder, o);
    REQUIRE_OK(report);
    auto folder = tree::open_folder(dir + "/zstd");
    REQUIRE_OK(folder);
    CHECK(!lz4pack::is_packed((*folder)->root()));
    auto index_bytes = read(**folder, kIndexName);
    REQUIRE_OK(index_bytes);
    auto ix = parse_index(*index_bytes);
    REQUIRE_OK(ix);
    CHECK_EQ(ix->files.size(), 2u);
    CHECK(tree::find((*folder)->root(), "data/noise.bin") != nullptr);
    auto same = convert::compare(game, dir + "/zstd");
    REQUIRE_OK(same);
    CHECK(same->only_in_a.empty());
    CHECK(same->different.empty());
}

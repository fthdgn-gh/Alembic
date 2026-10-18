// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/bytes.hpp"
#include "alembic/exfat.hpp"
#include "alembic/unicode.hpp"
#include "test.hpp"

#include <cstring>
#include <map>

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

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(seed + i * 31 + (i >> 9));
    return data;
}

// A minimal exFAT reader, enough to check the writer's output independently.
struct Reader {
    const std::vector<std::uint8_t>& image;
    std::uint64_t heap = 0;
    std::uint32_t cluster_size = 0;
    std::uint64_t fat = 0;

    explicit Reader(const std::vector<std::uint8_t>& img) : image(img) {
        fat = std::uint64_t(get_u32(img.data() + 0x50)) * 512;
        heap = std::uint64_t(get_u32(img.data() + 0x58)) * 512;
        cluster_size = 512u << img[0x6D];
    }

    std::uint64_t offset(std::uint32_t cluster) const { return heap + std::uint64_t(cluster - 2) * cluster_size; }

    // Follows the FAT chain and returns `length` bytes.
    std::vector<std::uint8_t> read_chain(std::uint32_t cluster, std::uint64_t length) const {
        std::vector<std::uint8_t> out;
        while (out.size() < length) {
            std::uint64_t n = std::min<std::uint64_t>(cluster_size, length - out.size());
            out.insert(out.end(), image.begin() + offset(cluster), image.begin() + offset(cluster) + n);
            cluster = get_u32(image.data() + fat + cluster * 4);
        }
        return out;
    }

    // Collects path -> contents for every file under the directory.
    void list(std::uint32_t cluster, std::uint64_t length, const std::string& prefix,
              std::map<std::string, std::vector<std::uint8_t>>& files, int& checksum_errors) const {
        std::vector<std::uint8_t> dir = read_chain(cluster, length);
        for (std::size_t i = 0; i + 32 <= dir.size(); i += 32) {
            if (dir[i] != 0x85) continue;
            int secondary = dir[i + 1];
            std::vector<std::uint8_t> set(dir.begin() + i, dir.begin() + i + (secondary + 1) * 32);
            std::uint16_t sum = 0;
            for (std::size_t k = 0; k < set.size(); ++k)
                if (k != 2 && k != 3) sum = static_cast<std::uint16_t>(((sum >> 1) | (sum << 15)) + set[k]);
            if (sum != get_u16(set.data() + 2)) ++checksum_errors;
            bool is_dir = get_u16(set.data() + 4) & 0x10;
            const std::uint8_t* stream = set.data() + 32;
            std::size_t name_length = stream[3];
            std::uint32_t first = get_u32(stream + 0x14);
            std::uint64_t size = get_u64(stream + 0x18);
            std::u16string name;
            for (std::size_t k = 0; k < name_length; ++k)
                name.push_back(static_cast<char16_t>(get_u16(set.data() + 64 + (k / 15) * 32 + 2 + (k % 15) * 2)));
            std::string path = prefix + unicode::utf16_to_utf8(name);
            if (is_dir) list(first, size, path + "/", files, checksum_errors);
            else files[path] = size ? read_chain(first, size) : std::vector<std::uint8_t>{};
            i += secondary * 32;
        }
    }
};

std::string build_tree() {
    std::string root = test::make_temp_dir();
    test::make_dir(root + "/sce_sys");
    test::make_dir(root + "/Data");
    test::make_dir(root + "/Data/empty dir");
    test::write_file(root + "/eboot.bin", pattern(100000, 1));
    test::write_file(root + "/sce_sys/param.json", pattern(300, 2));
    test::write_file(root + "/Data/a_very_long_file_name_over_fifteen_units.dat", pattern(65536, 3));
    test::write_file(root + "/Data/empty.txt", {});
    test::write_file(root + "/Data/\xC3\xA9t\xC3\xA9.bin", pattern(7, 4)); // été.bin
    test::write_file(root + "/.DS_Store", pattern(10, 5));                   // ignored
    test::write_file(root + "/._eboot.bin", pattern(10, 6));                 // ignored
    return root;
}

} // namespace

TEST_CASE(exfat_image_reads_back) {
    std::string root = build_tree();
    auto builder = exfat::ImageBuilder::scan(root);
    REQUIRE_OK(builder);
    CHECK_EQ(builder->cluster_size(), 0x10000u);
    CHECK_EQ(builder->file_count(), 5u);
    CHECK_EQ(builder->directory_count(), 4u);

    VectorSink sink;
    REQUIRE_OK(builder->write(sink));
    const std::vector<std::uint8_t>& img = sink.bytes;
    CHECK_EQ(img.size(), builder->image_length());
    CHECK(std::memcmp(img.data() + 3, "EXFAT   ", 8) == 0);
    CHECK_EQ(get_u16(img.data() + 0x1FE), std::uint16_t(0xAA55));
    CHECK(std::memcmp(img.data(), img.data() + 12 * 512, 12 * 512) == 0); // backup boot region
    CHECK_EQ(get_u64(img.data() + 0x48) * 512, std::uint64_t(img.size()));

    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 11 * 512; ++i)
        if (i != 106 && i != 107 && i != 112) sum = ((sum >> 1) | (sum << 31)) + img[i];
    CHECK_EQ(get_u32(img.data() + 11 * 512), sum);

    Reader reader(img);
    std::map<std::string, std::vector<std::uint8_t>> files;
    int checksum_errors = 0;
    std::uint32_t root_cluster = get_u32(img.data() + 0x60);
    reader.list(root_cluster, reader.cluster_size, "", files, checksum_errors);
    CHECK_EQ(checksum_errors, 0);
    CHECK_EQ(files.size(), std::size_t(5));
    CHECK(files["eboot.bin"] == pattern(100000, 1));
    CHECK(files["sce_sys/param.json"] == pattern(300, 2));
    CHECK(files["Data/a_very_long_file_name_over_fifteen_units.dat"] == pattern(65536, 3));
    CHECK(files["Data/empty.txt"].empty());
    CHECK(files["Data/\xC3\xA9t\xC3\xA9.bin"] == pattern(7, 4));
}

TEST_CASE(exfat_adds_ampr_index_when_marker_present) {
    std::string root = build_tree();
    test::make_dir(root + "/fakelib");
    test::write_file(root + "/fakelib/libSceAmpr.sprx", pattern(4096, 7));
    test::write_file(root + "/ampr_emu.index", pattern(16, 8)); // stale copy, replaced

    auto builder = exfat::ImageBuilder::scan(root);
    REQUIRE_OK(builder);
    VectorSink sink;
    REQUIRE_OK(builder->write(sink));
    Reader reader(sink.bytes);
    std::map<std::string, std::vector<std::uint8_t>> files;
    int checksum_errors = 0;
    reader.list(get_u32(sink.bytes.data() + 0x60), reader.cluster_size, "", files, checksum_errors);
    CHECK(files.count("ampr_emu.index") == 1);
    const auto& index = files["ampr_emu.index"];
    CHECK(index.size() > 48 && std::memcmp(index.data(), "AMPRIDX3", 8) == 0);
    CHECK_EQ(get_u64(index.data() + 16), std::uint64_t(6)); // every real file, not the index

    exfat::BuildOptions no_ampr;
    no_ampr.generate_ampr_index = false;
    auto plain = exfat::ImageBuilder::scan(root, no_ampr);
    REQUIRE_OK(plain);
    CHECK_EQ(plain->file_count(), 7u); // includes the stale ampr_emu.index as a normal file
}

TEST_CASE(exfat_rejects_bad_cluster_size) {
    std::string root = test::make_temp_dir();
    exfat::BuildOptions options;
    options.cluster_size = 3000;
    CHECK(!exfat::ImageBuilder::scan(root, options).ok());
    options.cluster_size = 0x800;
    CHECK(!exfat::ImageBuilder::scan(root, options).ok());
}

TEST_CASE(exfat_empty_folder) {
    std::string root = test::make_temp_dir();
    auto builder = exfat::ImageBuilder::scan(root);
    REQUIRE_OK(builder);
    VectorSink sink;
    REQUIRE_OK(builder->write(sink));
    CHECK_EQ(sink.bytes.size(), builder->image_length());
}

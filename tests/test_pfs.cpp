// SPDX-License-Identifier: GPL-3.0-or-later
// PFS (.ffpfs) writing and reading. Byte parity with MkPFS is checked by
// tools/oracle/compare.sh; these cover round trips and the rules.
#include "alembic/convert.hpp"
#include "alembic/pfs.hpp"
#include "test.hpp"

#include <string>

using namespace alembic;

namespace {

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(seed + i * 13 + (i >> 9));
    return data;
}

std::string make_tree(const std::string& parent) {
    std::string root = parent + "/game";
    test::make_dir(root);
    test::make_dir(root + "/sce_sys");
    test::make_dir(root + "/Data");
    test::make_dir(root + "/Data/Deep");
    test::make_dir(root + "/Data/Deep/Er");
    test::make_dir(root + "/Data/Empty");
    test::write_file(root + "/sce_sys/param.json", {'{', '}'});
    test::write_file(root + "/eboot.bin", pattern(200000, 1));
    test::write_file(root + "/Data/big.bin", pattern(3 * 65536 + 5, 2));
    test::write_file(root + "/Data/Deep/Er/leaf.txt", pattern(10, 3));
    test::write_file(root + "/Data/zero.bin", {});
    // "0_" and "1@" share a flat-path-table hash.
    test::write_file(root + "/0_", pattern(4, 4));
    test::write_file(root + "/1@", pattern(5, 5));
    return root;
}

std::vector<std::uint8_t> read_all(const tree::Source& source, const tree::Node& file) {
    auto in = source.open(file);
    if (!in.ok()) return {};
    std::vector<std::uint8_t> data(static_cast<std::size_t>(file.size));
    if (!data.empty() && !(*in)->read_at(0, data).ok()) return {};
    return data;
}

} // namespace

TEST_CASE(pfs_round_trip) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_tree(tmp);
    convert::Options options;
    options.ffpfsc.build_timestamp = 1700000000;
    REQUIRE_OK(convert::run(game, tmp + "/out.ffpfs", convert::Format::ffpfs, options));

    auto format = convert::detect(tmp + "/out.ffpfs");
    REQUIRE_OK(format);
    CHECK(*format == convert::Format::ffpfs);
    auto input = convert::open(tmp + "/out.ffpfs");
    REQUIRE_OK(input);
    auto folder = tree::open_folder(game);
    REQUIRE_OK(folder);
    for (const char* path : {"eboot.bin", "Data/big.bin", "Data/Deep/Er/leaf.txt", "Data/zero.bin", "0_", "1@",
                             "sce_sys/param.json"}) {
        const tree::Node* a = tree::find(input->tree->root(), path);
        const tree::Node* b = tree::find((*folder)->root(), path);
        CHECK(a != nullptr);
        if (!a || !b) continue;
        CHECK_EQ(a->size, b->size);
        CHECK(read_all(*input->tree, *a) == read_all(**folder, *b));
    }
    // MkPFS only records folders that hold files.
    CHECK(tree::find(input->tree->root(), "Data/Empty") == nullptr);

    // The hash collision needs a collision_resolver next to flat_path_table.
    std::vector<std::uint8_t> image = test::read_file(tmp + "/out.ffpfs");
    std::string needle = "collision_resolver";
    CHECK(std::search(image.begin(), image.end(), needle.begin(), needle.end()) != image.end());

    // Same inputs and time stamp, same bytes; the planned length is the file length.
    REQUIRE_OK(convert::run(game, tmp + "/again.ffpfs", convert::Format::ffpfs, options));
    CHECK(test::read_file(tmp + "/again.ffpfs") == image);
    auto builder = pfs::ImageBuilder::from_source(**folder, (*folder)->root(), {65536, true, 1700000000});
    REQUIRE_OK(builder);
    CHECK_EQ(builder->image_length(), static_cast<std::uint64_t>(image.size()));
}

TEST_CASE(pfs_rules) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_tree(tmp);
    // PFS names are ASCII only.
    test::write_file(game + "/Data/\xC3\xA9t\xC3\xA9.txt", pattern(3, 6));
    auto bad = convert::run(game, tmp + "/bad.ffpfs", convert::Format::ffpfs);
    CHECK(!bad.ok());

    // An FFPFSC is PFS too, but detected as FFPFSC; its tree reads the same.
    std::string clean = test::make_temp_dir();
    std::string game2 = make_tree(clean);
    convert::Options options;
    options.ffpfsc.build_timestamp = 1;
    REQUIRE_OK(convert::run(game2, clean + "/x.ffpfsc", convert::Format::ffpfsc, options));
    auto format = convert::detect(clean + "/x.ffpfsc");
    REQUIRE_OK(format);
    CHECK(*format == convert::Format::ffpfsc);

    // Block sizes: powers of two from 4 KiB to 1 MiB.
    auto folder = tree::open_folder(game2);
    REQUIRE_OK(folder);
    CHECK(!pfs::ImageBuilder::from_source(**folder, (*folder)->root(), {3000, true, 1}).ok());
    CHECK(pfs::ImageBuilder::from_source(**folder, (*folder)->root(), {4096, true, 1}).ok());
}

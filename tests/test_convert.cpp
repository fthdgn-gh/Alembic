// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/convert.hpp"
#include "test.hpp"

#include <string>

using namespace alembic;

namespace {

std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(seed + i * 7 + (i >> 11));
    return data;
}

std::string make_game(const std::string& parent) {
    std::string root = parent + "/game";
    test::make_dir(root);
    test::make_dir(root + "/sce_sys");
    test::make_dir(root + "/Data");
    test::make_dir(root + "/Data/Empty");
    test::write_file(root + "/sce_sys/param.json", {'{', '"', 't', 'i', 't', 'l', 'e', 'I', 'd', '"', ':', '"', 'P', 'P',
                                                    'S', 'A', '0', '1', '2', '3', '4', '"', '}'});
    test::write_file(root + "/eboot.bin", bytes(300000, 1));
    test::write_file(root + "/Data/big.bin", bytes(2 * 1024 * 1024 + 17, 2));
    test::write_file(root + "/Data/\xC3\xA9t\xC3\xA9.txt", bytes(10, 3));
    test::write_file(root + "/Data/empty.bin", {});
    return root;
}

// Compares two folders file by file (names, sizes and contents).
void check_same_tree(const std::string& a, const std::string& b) {
    auto ta = tree::open_folder(a);
    auto tb = tree::open_folder(b);
    REQUIRE_OK(ta);
    REQUIRE_OK(tb);
    std::vector<std::pair<std::string, const tree::Node*>> stack{{"", &(*ta)->root()}};
    while (!stack.empty()) {
        auto [path, node] = stack.back();
        stack.pop_back();
        const tree::Node* other = tree::find((*tb)->root(), path);
        CHECK(other != nullptr);
        if (!other) continue;
        CHECK_EQ(node->children.size(), other->children.size());
        for (const tree::Node& child : node->children) {
            std::string child_path = path.empty() ? child.name : path + "/" + child.name;
            if (child.is_directory) stack.push_back({child_path, &child});
            else CHECK(test::read_file(a + "/" + child_path) == test::read_file(b + "/" + child_path));
        }
    }
}

} // namespace

TEST_CASE(convert_round_trips) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    convert::Options options;
    options.ffpfsc.build_timestamp = 1;

    // folder -> ffpfsc -> folder
    auto r1 = convert::run(game, tmp + "/g.ffpfsc", convert::Format::ffpfsc, options);
    REQUIRE_OK(r1);
    CHECK(r1->detail == "PPSA01234.exfat");
    REQUIRE_OK(convert::run(tmp + "/g.ffpfsc", tmp + "/from_ffpfsc", convert::Format::folder, options));
    check_same_tree(game, tmp + "/from_ffpfsc");

    // ffpfsc -> exfat (unwrap) equals folder -> exfat
    REQUIRE_OK(convert::run(tmp + "/g.ffpfsc", tmp + "/unwrapped.exfat", convert::Format::exfat, options));
    REQUIRE_OK(convert::run(game, tmp + "/direct.exfat", convert::Format::exfat, options));
    CHECK(test::read_file(tmp + "/unwrapped.exfat") == test::read_file(tmp + "/direct.exfat"));

    // exfat -> folder, exfat --rebuild -> exfat (same layout, so same bytes)
    REQUIRE_OK(convert::run(tmp + "/direct.exfat", tmp + "/from_exfat", convert::Format::folder, options));
    check_same_tree(game, tmp + "/from_exfat");
    CHECK(!convert::run(tmp + "/direct.exfat", tmp + "/again.exfat", convert::Format::exfat, options).ok());
    options.rebuild = true;
    REQUIRE_OK(convert::run(tmp + "/direct.exfat", tmp + "/again.exfat", convert::Format::exfat, options));
    CHECK(test::read_file(tmp + "/again.exfat") == test::read_file(tmp + "/direct.exfat"));

    // exfat --rebuild -> ffpfsc names the payload from param.json, like a folder build,
    // and gives the same container.
    REQUIRE_OK(convert::run(tmp + "/direct.exfat", tmp + "/rebuilt.ffpfsc", convert::Format::ffpfsc, options));
    CHECK(test::read_file(tmp + "/rebuilt.ffpfsc") == test::read_file(tmp + "/g.ffpfsc"));

    // exfat -> ffpfsc without --rebuild wraps the image and keeps its file name
    options.rebuild = false;
    auto wrapped = convert::run(tmp + "/direct.exfat", tmp + "/wrapped.ffpfsc", convert::Format::ffpfsc, options);
    REQUIRE_OK(wrapped);
    CHECK(wrapped->detail == "direct.exfat");
}

TEST_CASE(convert_detects_and_refuses) {
    std::string tmp = test::make_temp_dir();
    test::write_file(tmp + "/junk.bin", bytes(5000, 9));
    CHECK(!convert::detect(tmp + "/junk.bin").ok());
    test::write_file(tmp + "/fake.pkg", {0x7F, 'F', 'I', 'H', 0, 0, 0, 0});
    auto fpkg = convert::detect(tmp + "/fake.pkg");
    REQUIRE_OK(fpkg);
    CHECK(*fpkg == convert::Format::fpkg);
    CHECK(!convert::run(tmp + "/fake.pkg", tmp + "/x.exfat", convert::Format::exfat).ok());
    CHECK(convert::guess_output_format("a/b.FFPFSC") == std::optional<convert::Format>(convert::Format::ffpfsc));
    CHECK(convert::guess_output_format("out/") == std::optional<convert::Format>(convert::Format::folder));
    CHECK(!convert::guess_output_format("noext"));
}

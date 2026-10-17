// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/ffpfsc.hpp"
#include "test.hpp"

#include <string>

using namespace alembic;

namespace {

std::vector<std::uint8_t> text(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string make_dump(const std::string& parent, const std::string& name, const std::string& param) {
    std::string root = parent + "/" + name;
    test::make_dir(root);
    test::make_dir(root + "/sce_sys");
    test::write_file(root + "/sce_sys/param.json", text(param));
    std::vector<std::uint8_t> eboot(200000);
    for (std::size_t i = 0; i < eboot.size(); ++i) eboot[i] = static_cast<std::uint8_t>(i % 251);
    test::write_file(root + "/eboot.bin", eboot);
    return root;
}

} // namespace

TEST_CASE(ffpfsc_round_trip_from_directory) {
    std::string tmp = test::make_temp_dir();
    std::string dump = make_dump(tmp, "My Game", R"({"titleId": "ppsa01234"})");
    ffpfsc::BuildOptions options;
    options.build_timestamp = 1700000000;
    auto result = ffpfsc::create_from_directory(dump, tmp + "/out/game.ffpfsc", options);
    REQUIRE_OK(result);
    CHECK(result->inner_file_name == "PPSA01234.exfat");

    auto file = File::open(tmp + "/out/game.ffpfsc", File::Mode::read);
    REQUIRE_OK(file);
    auto info = ffpfsc::inspect(*file);
    REQUIRE_OK(info);
    CHECK(info->inner_file_name == "PPSA01234.exfat");
    CHECK_EQ(info->payload_offset, std::uint64_t(6 * 0x10000));
    CHECK_EQ(info->timestamp, std::int64_t(1700000000));
    CHECK_EQ(info->logical_length, result->source_length);

    // The extracted payload is exactly the exFAT image of the dump.
    REQUIRE_OK(ffpfsc::extract_payload(tmp + "/out/game.ffpfsc", tmp + "/payload.exfat"));
    REQUIRE_OK(exfat::write_directory_image(dump, tmp + "/direct.exfat"));
    CHECK(test::read_file(tmp + "/payload.exfat") == test::read_file(tmp + "/direct.exfat"));

    auto verified = ffpfsc::verify(tmp + "/out/game.ffpfsc", tmp + "/direct.exfat");
    REQUIRE_OK(verified);
    CHECK(verified->source_matches == std::optional<bool>(true));
}

TEST_CASE(ffpfsc_from_image_and_overwrite) {
    std::string tmp = test::make_temp_dir();
    std::vector<std::uint8_t> image(300000, 7);
    test::write_file(tmp + "/disk.img", image);
    auto result = ffpfsc::create_from_image(tmp + "/disk.img", tmp + "/disk.ffpfsc");
    REQUIRE_OK(result);
    CHECK(result->inner_file_name == "disk.img");
    CHECK(!ffpfsc::create_from_image(tmp + "/disk.img", tmp + "/disk.ffpfsc").ok()); // exists
    ffpfsc::BuildOptions options;
    options.overwrite_existing = true;
    options.inner_file_name = "other.img";
    REQUIRE_OK(ffpfsc::create_from_image(tmp + "/disk.img", tmp + "/disk.ffpfsc", options));
    REQUIRE_OK(ffpfsc::extract_payload(tmp + "/disk.ffpfsc", tmp + "/back.img"));
    CHECK(test::read_file(tmp + "/back.img") == image);
    options.inner_file_name = "caf\xC3\xA9.img";
    CHECK(!ffpfsc::create_from_image(tmp + "/disk.img", tmp + "/x.ffpfsc", options).ok()); // not ASCII
}

TEST_CASE(ffpfsc_refuses_output_inside_dump) {
    std::string tmp = test::make_temp_dir();
    std::string dump = make_dump(tmp, "game", "{}");
    CHECK(!ffpfsc::create_from_directory(dump, dump + "/out.ffpfsc").ok());
}

TEST_CASE(ffpfsc_dump_image_names) {
    std::string tmp = test::make_temp_dir();
    CHECK(ffpfsc::resolve_dump_image_name(make_dump(tmp, "a", R"({"TITLEID": "abcd12345"})")) == "ABCD12345.exfat");
    CHECK(ffpfsc::resolve_dump_image_name(make_dump(tmp, "b", R"({"titleId": "PPSA1234"})")) == "b.exfat");
    CHECK(ffpfsc::resolve_dump_image_name(make_dump(tmp, "c", R"({"titleId": "PPSA01234",})")) == "c.exfat");
    CHECK(ffpfsc::resolve_dump_image_name(make_dump(tmp, " .J\xC3\xA9u:x. ", "{}")) == "J_u_x.exfat");
    CHECK(ffpfsc::resolve_dump_image_name(make_dump(tmp, "...", "{}")) == "PS5_GAME.exfat");
}

TEST_CASE(ffpfsc_game_root_search) {
    std::string tmp = test::make_temp_dir();
    test::make_dir(tmp + "/wrap");
    test::make_dir(tmp + "/wrap/inner");
    std::string dump = make_dump(tmp + "/wrap/inner", "PPSA01234-app", "{}");
    auto root = ffpfsc::resolve_game_root(tmp + "/wrap");
    REQUIRE_OK(root);
    CHECK(*root == dump);
    make_dump(tmp + "/wrap", "second", "{}");
    CHECK(!ffpfsc::resolve_game_root(tmp + "/wrap").ok()); // two candidates
}

TEST_CASE(ffpfsc_inspect_rejects_corruption) {
    std::string tmp = test::make_temp_dir();
    test::write_file(tmp + "/disk.img", std::vector<std::uint8_t>(100000, 1));
    REQUIRE_OK(ffpfsc::create_from_image(tmp + "/disk.img", tmp + "/d.ffpfsc"));
    auto file = File::open(tmp + "/d.ffpfsc", File::Mode::read_write);
    REQUIRE_OK(file);
    REQUIRE_OK(ffpfsc::inspect(*file));
    std::uint8_t bad[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    REQUIRE_OK(file->write_at(3 * 0x10000, Bytes(bad, 4))); // flat path table hash
    CHECK(!ffpfsc::inspect(*file).ok());
}

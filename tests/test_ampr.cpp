// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/ampr.hpp"
#include "alembic/bytes.hpp"
#include "test.hpp"

#include <cstring>

using namespace alembic;

TEST_CASE(ampr_hash_matches_ps5pkgtool) {
    auto lower = ampr::hash_path("/app0/eboot.bin");
    auto upper = ampr::hash_path("/APP0/EBOOT.BIN");
    REQUIRE_OK(lower);
    REQUIRE_OK(upper);
    CHECK(*lower == *upper);
    // PS5PKGTool's offset basis is 1469598103934665603, not the standard FNV
    // 14695981039346656037 (a digit is missing). We must match it.
    auto a = ampr::hash_path("A");
    REQUIRE_OK(a);
    CHECK_EQ(*a, 4953267810257967366ull);
}

TEST_CASE(ampr_build_layout) {
    std::vector<ampr::FileRecord> files = {
        {"sce_sys/param.json", 100, 1700000000},
        {"eboot.bin", 5000, 1700000001},
        {"EBOOT.BIN", 1, 1},          // duplicate key, dropped
        {"ampr_emu.index", 64, 1},     // the index itself, dropped
        {"fakelib/libSceAmpr.sprx", 7, 2},
    };
    auto built = ampr::build(files);
    REQUIRE_OK(built);
    const std::vector<std::uint8_t>& data = *built;
    auto info = ampr::inspect(data);
    REQUIRE_OK(info);
    CHECK_EQ(info->record_count, 3u);
    CHECK_EQ(info->slot_count, 8u);
    CHECK(std::memcmp(data.data(), "AMPRIDX3", 8) == 0);

    // Sorted by lower-cased path: eboot.bin, fakelib/..., sce_sys/...
    std::size_t paths = 48 + 3 * 24;
    const char* first = reinterpret_cast<const char*>(data.data() + paths + get_u32(data.data() + 48));
    CHECK(std::string(first) == "/app0/eboot.bin");
    CHECK_EQ(get_u64(data.data() + 48 + 8), std::uint64_t(5000));
    const char* third = reinterpret_cast<const char*>(data.data() + paths + get_u32(data.data() + 48 + 48));
    CHECK(std::string(third) == "/app0/sce_sys/param.json");
}

TEST_CASE(ampr_build_empty) {
    auto built = ampr::build({});
    REQUIRE_OK(built);
    CHECK(built->empty());
}

TEST_CASE(ampr_rejects_bad_paths) {
    CHECK(!ampr::build({{"a/../b", 1, 0}}).ok());
    CHECK(!ampr::build({{"a//b", 1, 0}}).ok());
}

TEST_CASE(ampr_schemes_differ_only_for_non_ascii) {
    using ampr::Scheme;
    auto a = ampr::hash_path("/app0/Sce_Sys/Param.json", Scheme::ampr_emu);
    auto b = ampr::hash_path("/app0/Sce_Sys/Param.json", Scheme::ps5pkgtool);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK(*a == *b);
    // "été": ampr_emu hashes the UTF-8 bytes and leaves É alone; PS5PKGTool
    // hashes lower-cased code points. Values checked against ampr_emu's reader.
    auto c = ampr::hash_path("/app0/Data/\xC3\xA9t\xC3\xA9.dat", Scheme::ampr_emu);
    auto d = ampr::hash_path("/app0/Data/\xC3\xA9t\xC3\xA9.dat", Scheme::ps5pkgtool);
    REQUIRE_OK(c);
    REQUIRE_OK(d);
    CHECK_EQ(*c, 14369817798231876246ull);
    CHECK_EQ(*d, 15512106277425209738ull);
    auto upper = ampr::hash_path("/app0/Data/\xC3\x89t\xC3\xA9.dat", Scheme::ampr_emu); // Été
    REQUIRE_OK(upper);
    CHECK(*upper != *c);
}

TEST_CASE(ampr_emu_scheme_rules) {
    // ampr_emu's builder never indexes these root files.
    auto built = ampr::build({{"eboot.bin", 1, 0}, {"AMPR_COMMANDS.BIN", 1, 0}, {"apr_emu.log", 1, 0},
                              {"ampr_emu.index.bak", 1, 0}, {"data/apr_emu.log", 1, 0}});
    REQUIRE_OK(built);
    auto info = ampr::inspect(*built);
    REQUIRE_OK(info);
    CHECK_EQ(info->record_count, 2u); // eboot.bin and data/apr_emu.log

    // Names over 255 bytes or paths over 1023 bytes make the reader reject the index.
    CHECK(!ampr::build({{std::string(256, 'a'), 1, 0}}).ok());
    CHECK(ampr::build({{std::string(255, 'a'), 1, 0}}).ok());
    std::string deep;
    for (int i = 0; i < 70; ++i) deep += "abcdefghijklmn/";
    CHECK(!ampr::build({{deep + "f", 1, 0}}).ok());
    CHECK(!ampr::build({{"a\tb", 1, 0}}).ok());
    // PS5PKGTool's scheme has no such limits.
    CHECK(ampr::build({{std::string(256, 'a'), 1, 0}}, ampr::Scheme::ps5pkgtool).ok());
}

TEST_CASE(ampr_emu_sorts_by_folded_bytes) {
    // Byte order puts "Z" (lowered to 'z', 0x7A) before "é" (0xC3 ...), and "_" (0x5F) before "a".
    auto built = ampr::build({{"\xC3\xA9.bin", 1, 0}, {"Z.bin", 1, 0}, {"a.bin", 1, 0}, {"_.bin", 1, 0}});
    REQUIRE_OK(built);
    const std::vector<std::uint8_t>& data = *built;
    std::size_t paths = 48 + 4 * 24;
    std::vector<std::string> order;
    for (int i = 0; i < 4; ++i)
        order.emplace_back(reinterpret_cast<const char*>(data.data() + paths + get_u32(data.data() + 48 + i * 24)));
    CHECK(order[0] == "/app0/_.bin");
    CHECK(order[1] == "/app0/a.bin");
    CHECK(order[2] == "/app0/Z.bin");
    CHECK(order[3] == "/app0/\xC3\xA9.bin");
}

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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/json.hpp"
#include "test.hpp"

using namespace alembic;

TEST_CASE(json_top_level_strings) {
    auto props = json::top_level_properties(
        R"({"titleId": "PPSA01234", "n": 1.5e3, "nested": {"titleId": "XXXX00000"}, "a": [1, true, null],)"
        R"( "esc": "a\"b\\cé😀"})");
    REQUIRE_OK(props);
    CHECK_EQ(props->size(), std::size_t(5));
    CHECK((*props)[0].name == "titleId" && (*props)[0].value == std::optional<std::string>("PPSA01234"));
    CHECK(!(*props)[1].value);
    CHECK(!(*props)[2].value); // nested objects are skipped, not searched
    CHECK((*props)[4].value == std::optional<std::string>("a\"b\\c\xC3\xA9\xF0\x9F\x98\x80"));
}

TEST_CASE(json_rejects_invalid) {
    CHECK(!json::top_level_properties("").ok());
    CHECK(!json::top_level_properties("{").ok());
    CHECK(!json::top_level_properties(R"({"a": 1,})").ok());      // trailing comma
    CHECK(!json::top_level_properties(R"({"a": 01})").ok());      // leading zero
    CHECK(!json::top_level_properties("{\"a\": \"x\ty\"}").ok()); // raw control character
    CHECK(!json::top_level_properties(R"({"a": 1} x)").ok());     // trailing junk
    CHECK(!json::top_level_properties("// c\n{}").ok());          // comments
    CHECK(!json::top_level_properties(R"({"a": tru})").ok());
    CHECK(!json::top_level_properties("\xEF\xBB\xBF{}").ok()); // BOM: .NET rejects it too
    std::string deep(65, '[');
    deep += std::string(65, ']');
    CHECK(!json::top_level_properties(deep).ok()); // deeper than 64
    std::string ok_deep(64, '[');
    ok_deep += std::string(64, ']');
    CHECK(json::top_level_properties(ok_deep).ok());
}

TEST_CASE(json_non_object_root) {
    auto props = json::top_level_properties(R"(["titleId", "PPSA01234"])");
    REQUIRE_OK(props);
    CHECK(props->empty());
}

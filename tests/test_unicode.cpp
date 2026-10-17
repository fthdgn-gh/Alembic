// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/unicode.hpp"
#include "test.hpp"

using namespace alembic;

TEST_CASE(unicode_round_trip) {
    std::string text = "eboot.bin \xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80"; // é 日 😀
    auto utf16 = unicode::utf8_to_utf16(text);
    REQUIRE_OK(utf16);
    CHECK_EQ(utf16->size(), std::size_t(10 + 1 + 1 + 2));
    CHECK(unicode::utf16_to_utf8(*utf16) == text);
}

TEST_CASE(unicode_rejects_bad_utf8) {
    CHECK(!unicode::utf8_to_utf16("\xC0\xAF").ok());         // overlong '/'
    CHECK(!unicode::utf8_to_utf16("\xED\xA0\x80").ok());     // surrogate
    CHECK(!unicode::utf8_to_utf16("\xE6\x97").ok());         // truncated
    CHECK(!unicode::utf8_to_utf16("\xF4\x90\x80\x80").ok()); // > U+10FFFF
}

TEST_CASE(unicode_lower_invariant_matches_dotnet) {
    CHECK_EQ(std::uint32_t(unicode::to_lower_invariant(U'A')), std::uint32_t(U'a'));
    CHECK_EQ(std::uint32_t(unicode::to_lower_invariant(U'É')), std::uint32_t(U'é'));  // É
    CHECK_EQ(std::uint32_t(unicode::to_lower_invariant(U'İ')), std::uint32_t(0x130));         // İ: .NET leaves it unchanged
    CHECK_EQ(std::uint32_t(unicode::to_lower_invariant(U'K')), std::uint32_t(U'k'));       // Kelvin
    CHECK_EQ(std::uint32_t(unicode::to_lower_invariant(U'\U00010400')), std::uint32_t(U'\U00010428'));
    CHECK_EQ(std::uint32_t(unicode::to_lower_invariant(U'ß')), std::uint32_t(U'ß'));  // ß unchanged
    CHECK(unicode::to_lower_invariant(u"SCE_SYS/Param.JSON") == u"sce_sys/param.json");
}

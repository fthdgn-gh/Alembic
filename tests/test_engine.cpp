// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's JSON value, folder rules and output naming.
#include "json_value.hpp"
#include "l10n.hpp"
#include "runner.hpp"
#include "test.hpp"

using namespace alembic;
using namespace alembic::engine;

TEST_CASE(engine_json_round_trips) {
    auto v = parse_json(R"({"a":[1,-2,3.5,true,null],"s":"x\"\u00e9\ud83d\ude00","big":159723618304,"o":{}})");
    REQUIRE_OK(v);
    CHECK_EQ((*v)["a"].items().size(), 5u);
    CHECK_EQ((*v)["a"].items()[1].integer(), -2);
    CHECK((*v)["a"].items()[2].number() == 3.5);
    CHECK((*v)["s"].str() == "x\"\xC3\xA9\xF0\x9F\x98\x80");
    CHECK_EQ((*v)["big"].integer(), 159723618304);
    CHECK((*v)["missing"].is_null());
    CHECK(v->dump() == R"({"a":[1,-2,3.5,true,null],"s":"x\"é😀","big":159723618304,"o":{}})");
    CHECK(!parse_json("{\"a\":1,}").ok());
    CHECK(!parse_json("[1] x").ok());
    CHECK(!parse_json("\"\\ud800\"").ok());
    CHECK(!parse_json("01").ok());
    CHECK(json_quote("a\x01\xFF") == "\"a\\u0001\xEF\xBF\xBD\"");
}

TEST_CASE(engine_paths_stay_inside_the_roots) {
    std::vector<std::string> roots{"/data", "/mnt/usb0/"};
    CHECK(path_allowed("/data", roots));
    CHECK(path_allowed("/data/homebrew/x.pkg", roots));
    CHECK(path_allowed("/mnt/usb0/game", roots));
    CHECK(!path_allowed("/database", roots));
    CHECK(!path_allowed("/data/../etc", roots));
    CHECK(!path_allowed("/data/./x", roots));
    CHECK(!path_allowed("data/x", roots));
    CHECK(!path_allowed("/system/x", roots));
    CHECK(!path_allowed("", roots));
}

TEST_CASE(engine_names_outputs_next_to_the_input) {
    JobSpec s;
    s.input = "/data/games/A.pkg";
    s.to = "ffpfsc";
    auto out = resolve_output(s);
    REQUIRE_OK(out);
    CHECK(*out == "/data/games/A.ffpfsc");
    s.lz4 = true;
    s.to = "exfat";
    CHECK(*resolve_output(s) == "/data/games/A-lz4.exfat");
    s.lz4 = false;
    s.input = "/data/games/A.exfat";
    CHECK(!resolve_output(s).ok()); // would replace the input
    s.output = "/data/other.exfat";
    CHECK(*resolve_output(s) == "/data/other.exfat");
    JobSpec other;
    other.input = "/mnt/usb0/games/A.pkg";
    other.to = "ffpfsc";
    other.output_dir = "/data/homebrew/";
    CHECK(*resolve_output(other) == "/data/homebrew/A.ffpfsc"); // another drive, default name
    other.output_name = "B.ffpfsc";
    CHECK(*resolve_output(other) == "/data/homebrew/B.ffpfsc");
    other.to = "folder";
    other.output_name = "";
    CHECK(*resolve_output(other) == "/data/homebrew/A/");
    other.output_dir = "/";
    CHECK(*resolve_output(other) == "/A/");
    JsonValue bad = JsonValue::object();
    bad.set("input", "/data/x.pkg");
    bad.set("output_name", "../x");
    CHECK(!JobSpec::from_json(bad).ok());
    // ShadowMount+'s mount point limit: 58 characters for .ffpfsc, 63 for .exfat.
    CHECK_EQ(mount_name_limit("/data/x.ffpfsc"), 58u);
    CHECK_EQ(mount_name_limit("/data/x.EXFAT"), 63u);
    CHECK_EQ(mount_name_limit("/data/x/"), 0u);
    JobSpec longname;
    longname.input = "/data/XX0000-ABCD12345_00-0123456789ABCDEFG-A0100-V0100.pkg"; // 54 characters
    longname.to = "ffpfsc";
    longname.lz4 = true;
    CHECK(*resolve_output(longname) == "/data/XX0000-ABCD12345_00-0123456789ABCDEFG-A0100-V0100-lz4.ffpfsc"); // 58: fits
    longname.input = "/data/XX0000-ABCD12345_00-0123456789ABCDEFGHIJKLMN-A0100-V0100.pkg"; // + -lz4 = 60
    CHECK(*resolve_output(longname) == "/data/ABCD12345-A0100-V0100-lz4.ffpfsc");
    longname.to = "exfat";
    CHECK(*resolve_output(longname) == "/data/XX0000-ABCD12345_00-0123456789ABCDEFGHIJKLMN-A0100-V0100-lz4.exfat"); // 60 <= 63
    CHECK(shorten_name(std::string(70, 'x') + "-lz4", 58) == std::string(54, 'x') + "-lz4");
    CHECK(shorten_name("short", 58) == "short");
    CHECK(mount_name_warning("/data/" + std::string(59, 'a') + ".ffpfsc") != "");
    CHECK(mount_name_warning("/data/" + std::string(58, 'a') + ".ffpfsc") == "");
    JobSpec none;
    none.input = "/data/x.pkg";
    CHECK(!resolve_target(none).ok()); // no output and no target
    none.output = "/data/x.ffpfsc";
    CHECK(*resolve_target(none) == "ffpfsc");
}

// The engine's translations come from the page's i18n.js.
TEST_CASE(engine_l10n_messages) {
    std::string here = __FILE__;
    std::vector<std::uint8_t> js = test::read_file(here.substr(0, here.rfind('/')) + "/../src/engine/web/i18n.js");
    CHECK(!js.empty());
    l10n::load(reinterpret_cast<const char*>(js.data()), js.size());
    CHECK(l10n::has_language("tr") && l10n::has_language("ja") && !l10n::has_language("xx"));

    l10n::Vars v;
    CHECK(l10n::match("cannot write '{path}': {reason}", "cannot write '/mnt/usb0/a b.exfat': No space left on device", v));
    CHECK(v.size() == 2 && v[0].second == "/mnt/usb0/a b.exfat" && v[1].second == "No space left on device");
    CHECK(!l10n::match("cannot write '{path}': {reason}", "cannot read '/x': y", v));
    CHECK(!l10n::match("Copied {name}", "Copied ", v)); // a part is never empty

    l10n::set_language("en");
    CHECK(l10n::t("Converted {name}", {{"name", "A"}}) == "Converted A");
    CHECK(l10n::message("cancel the job first") == "cancel the job first");
    l10n::set_language("tr");
    CHECK(l10n::t("Converted {name}", {{"name", "A"}}) == "A dönüştürüldü");
    // The pattern, and the system reason inside it.
    CHECK(l10n::message("cannot write '/x': No space left on device") == "'/x' yazılamıyor: Aygıtta boş yer kalmadı");
    CHECK(l10n::message("the output does not match the source (differs: eboot.bin); the source was kept") ==
          "çıktı kaynakla eşleşmiyor (farklı: eboot.bin); kaynak korundu");
    CHECK(l10n::message("something nobody translated") == "something nobody translated");
    l10n::set_language("xx");
    CHECK(l10n::language() == "en");
}

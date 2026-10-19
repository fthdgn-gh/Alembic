// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/ampr_emu.hpp"
#include "alembic/convert.hpp"
#include "alembic/io.hpp"
#include "test.hpp"

#include <cstring>
#include <string>

using namespace alembic;

namespace {

std::vector<std::uint8_t> filler(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(seed + i * 13);
    return data;
}

// A game whose eboot.bin names libSceAmpr across a 4 MiB read boundary when
// `imports`, with fakelib/ holding one library and libkernel.sprx.
std::string make_game(const std::string& parent, bool imports) {
    std::string root = parent + "/game";
    test::make_dir(root);
    test::make_dir(root + "/sce_sys");
    test::make_dir(root + "/fakelib");
    const char* param = "{\"titleId\":\"PPSA01234\"}";
    test::write_file(root + "/sce_sys/param.json", std::vector<std::uint8_t>(param, param + std::strlen(param)));
    std::vector<std::uint8_t> eboot = filler((4u << 20) + 1000, 1);
    if (imports) std::memcpy(eboot.data() + (4u << 20) - 4, "libSceAmpr", 10);
    test::write_file(root + "/eboot.bin", eboot);
    test::write_file(root + "/fakelib/libSceAgc.sprx", filler(5000, 2));
    test::write_file(root + "/fakelib/libkernel.sprx", filler(300, 3));
    test::write_file(root + "/data.bin", filler(70000, 4));
    return root;
}

std::shared_ptr<File> emulator_file(const std::string& dir) {
    test::write_file(dir + "/libSceAmpr.sprx", filler(12345, 9));
    auto f = File::open(dir + "/libSceAmpr.sprx", File::Mode::read);
    return f.ok() ? std::make_shared<File>(std::move(*f)) : nullptr;
}

} // namespace

TEST_CASE(ampr_emu_check_finds_the_import) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp, true);
    auto tree = tree::open_folder(game);
    REQUIRE_OK(tree);
    auto c = ampr_emu::check(**tree);
    REQUIRE_OK(c);
    CHECK(c->imports_ampr);
    CHECK(!c->ships_emulator);
    CHECK(c->needed());
    CHECK(c->fakelib == "fakelib");

    // fakelib2 wins, as in ShadowMount+.
    test::make_dir(game + "/fakelib2");
    test::write_file(game + "/fakelib2/libSceAmpr.sprx", filler(10, 5));
    auto tree2 = tree::open_folder(game);
    REQUIRE_OK(tree2);
    auto c2 = ampr_emu::check(**tree2);
    REQUIRE_OK(c2);
    CHECK(c2->fakelib == "fakelib2");
    CHECK(c2->ships_emulator);
    CHECK(!c2->needed());

    std::string tmp3 = test::make_temp_dir();
    auto tree3 = tree::open_folder(make_game(tmp3, false));
    REQUIRE_OK(tree3);
    auto c3 = ampr_emu::check(**tree3);
    REQUIRE_OK(c3);
    CHECK(!c3->imports_ampr);
    CHECK(!c3->needed());
}

TEST_CASE(ampr_emu_added_by_convert) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp, true);
    auto emulator = emulator_file(tmp);
    CHECK(emulator != nullptr);
    if (!emulator) return;

    // Without the file: a warning, no index (nothing marks the game as using ampr_emu).
    convert::Options plain;
    plain.ffpfsc.build_timestamp = 1;
    auto r0 = convert::run(game, tmp + "/plain.exfat", convert::Format::exfat, plain);
    REQUIRE_OK(r0);
    CHECK(r0->ampr_needed);
    CHECK(!r0->ampr_emulator_added);
    CHECK(!r0->warnings.empty());
    auto in0 = convert::open(tmp + "/plain.exfat");
    REQUIRE_OK(in0);
    CHECK(tree::find(in0->tree->root(), "ampr_emu.index") == nullptr);

    convert::Options options = plain;
    options.ampr_emulator = emulator;
    for (convert::Format to : {convert::Format::exfat, convert::Format::ffpkg, convert::Format::ffpfs,
                               convert::Format::ffpfsc, convert::Format::folder}) {
        std::string out = tmp + "/out." + std::to_string(static_cast<int>(to)) + (to == convert::Format::folder ? "/" : "");
        auto r = convert::run(game, out, to, options);
        REQUIRE_OK(r);
        CHECK(r->ampr_emulator_added);
        CHECK(r->ampr_emulator_path == "fakelib/libSceAmpr.sprx");
        auto in = convert::open(out);
        REQUIRE_OK(in);
        const tree::Node* sprx = tree::find(in->tree->root(), "fakelib/libSceAmpr.sprx");
        const tree::Node* index = tree::find(in->tree->root(), "ampr_emu.index");
        CHECK(sprx != nullptr);
        CHECK(index != nullptr);
        CHECK(tree::find(in->tree->root(), "fakelib/libSceAgc.sprx") != nullptr);
        if (sprx) {
            auto f = in->tree->open(*sprx);
            REQUIRE_OK(f);
            std::vector<std::uint8_t> got(static_cast<std::size_t>(sprx->size));
            REQUIRE_OK((*f)->read_at(0, MutableBytes(got.data(), got.size())));
            CHECK(got == filler(12345, 9));
        }
        auto again = ampr_emu::check(*in->tree);
        REQUIRE_OK(again);
        CHECK(!again->needed());
    }

    // An image into an image of the same kind is rebuilt to take the emulator.
    auto r2 = convert::run(tmp + "/plain.exfat", tmp + "/again.exfat", convert::Format::exfat, options);
    REQUIRE_OK(r2);
    CHECK(r2->ampr_emulator_added);
}

TEST_CASE(ampr_emu_only_when_needed) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp, false);
    convert::Options options;
    options.ffpfsc.build_timestamp = 1;
    options.ampr_emulator = emulator_file(tmp);
    auto r = convert::run(game, tmp + "/a.exfat", convert::Format::exfat, options);
    REQUIRE_OK(r);
    CHECK(!r->ampr_emulator_added);
    CHECK(r->warnings.empty());
    options.ampr_emulator_always = true;
    auto r2 = convert::run(game, tmp + "/b.exfat", convert::Format::exfat, options);
    REQUIRE_OK(r2);
    CHECK(r2->ampr_emulator_added);
}

TEST_CASE(ampr_emu_attach_to_folder) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp, true);
    auto emulator = emulator_file(tmp);
    CHECK(emulator != nullptr);
    if (!emulator) return;
    auto r = ampr_emu::attach_to_folder(game, *emulator);
    REQUIRE_OK(r);
    CHECK(!r->already_there);
    CHECK_EQ(r->written.size(), std::size_t(2));
    CHECK(test::read_file(game + "/fakelib/libSceAmpr.sprx") == filler(12345, 9));
    CHECK(is_regular_file(game + "/ampr_emu.index"));

    // The same index a conversion with the emulator writes.
    convert::Options options;
    options.ampr_emulator = emulator;
    test::make_dir(tmp + "/second");
    std::string fresh = make_game(tmp + "/second", true);
    auto converted = convert::run(fresh, tmp + "/conv/", convert::Format::folder, options);
    REQUIRE_OK(converted);
    CHECK(test::read_file(tmp + "/conv/ampr_emu.index").size() == test::read_file(game + "/ampr_emu.index").size());

    auto again = ampr_emu::attach_to_folder(game, *emulator);
    REQUIRE_OK(again);
    CHECK(again->already_there);
}

TEST_CASE(ampr_emu_attach_backport) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp, true);
    auto emulator = emulator_file(tmp);
    CHECK(emulator != nullptr);
    if (!emulator) return;
    convert::Options plain;
    plain.ffpfsc.build_timestamp = 1;
    REQUIRE_OK(convert::run(game, tmp + "/game.ffpfsc", convert::Format::ffpfsc, plain));
    auto image = convert::open(tmp + "/game.ffpfsc");
    REQUIRE_OK(image);

    std::string backports = tmp + "/backports";
    auto r = ampr_emu::attach_backport(*image->tree, backports, *emulator);
    REQUIRE_OK(r);
    std::string bp = backports + "/PPSA01234";
    CHECK(r->folder == bp);
    CHECK(test::read_file(bp + "/fakelib/libSceAmpr.sprx") == filler(12345, 9));
    CHECK(test::read_file(bp + "/fakelib/libSceAgc.sprx") == filler(5000, 2));
    CHECK(!path_exists(bp + "/fakelib/libkernel.sprx")); // ShadowMount+ drops it anyway
    CHECK(is_regular_file(bp + "/ampr_emu.index"));
    CHECK(path_exists(tmp + "/game.ffpfsc"));

    auto again = ampr_emu::attach_backport(*image->tree, backports, *emulator);
    REQUIRE_OK(again);
    CHECK(again->already_there);

    // An existing backport keeps its own fakelib; only the emulator joins it.
    std::string tmp2 = test::make_temp_dir();
    test::make_dir(tmp2 + "/PPSA01234");
    test::make_dir(tmp2 + "/PPSA01234/fakelib");
    test::write_file(tmp2 + "/PPSA01234/fakelib/libScePsml.sprx", filler(77, 6));
    auto r2 = ampr_emu::attach_backport(*image->tree, tmp2, *emulator);
    REQUIRE_OK(r2);
    CHECK(is_regular_file(tmp2 + "/PPSA01234/fakelib/libSceAmpr.sprx"));
    CHECK(!path_exists(tmp2 + "/PPSA01234/fakelib/libSceAgc.sprx"));
    CHECK(is_regular_file(tmp2 + "/PPSA01234/fakelib/libScePsml.sprx"));
}

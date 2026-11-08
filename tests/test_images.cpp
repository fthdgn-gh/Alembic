// SPDX-License-Identifier: GPL-3.0-or-later
// The Images tab's list (src/engine/images.hpp): what the scan finds and how
// a game's AMPR emulator is judged.
#include "images.hpp"
#include "test.hpp"

#include "alembic/io.hpp"

#include <algorithm>
#include <string>

using namespace alembic;
using namespace alembic::engine;

namespace {

void make_game(const std::string& dir, const char* title_id, bool imports) {
    test::make_dir(dir);
    test::make_dir(dir + "/sce_sys");
    std::vector<std::uint8_t> eboot(5000, 0);
    if (imports) std::copy_n("libSceAmpr", 10, eboot.begin() + 100);
    test::write_file(dir + "/eboot.bin", eboot);
    const std::string param = std::string("{\"titleId\":\"") + title_id + "\",\"contentVersion\":\"01.000.000\"}";
    test::write_file(dir + "/sce_sys/param.json", std::vector<std::uint8_t>(param.begin(), param.end()));
}

bool has(const std::vector<images::Found>& found, const std::string& path) {
    return std::any_of(found.begin(), found.end(), [&](const images::Found& f) { return f.path == path; });
}

} // namespace

TEST_CASE(images_scan_finds_folders_images_and_packages) {
    const std::string root = test::make_temp_dir();
    make_game(root + "/A", "PPSA00001", true);
    test::make_dir(root + "/group");
    make_game(root + "/group/B", "PPSA00002", true);  // one level down
    make_game(root + "/.hidden", "PPSA00003", true);  // hidden from ShadowMount+: listed
    test::make_dir(root + "/.Trashes");
    make_game(root + "/.Trashes/C", "PPSA00005", true); // inside a hidden folder: not searched
    test::make_dir(root + "/backports");
    make_game(root + "/backports/PPSA00004", "PPSA00004", true); // not a game source
    test::write_file(root + "/game.ffpfsc", std::vector<std::uint8_t>(16, 0));
    test::write_file(root + "/game.PKG", std::vector<std::uint8_t>(16, 0));
    test::write_file(root + "/notes.txt", std::vector<std::uint8_t>(4, 0));
    const auto found = images::scan({root});
    CHECK(has(found, root + "/A"));
    CHECK(has(found, root + "/group/B"));
    CHECK(has(found, root + "/.hidden"));
    CHECK(has(found, root + "/game.ffpfsc"));
    CHECK(has(found, root + "/game.PKG"));
    CHECK(!has(found, root + "/.Trashes/C"));
    CHECK(!has(found, root + "/backports/PPSA00004"));
    CHECK(!has(found, root + "/notes.txt"));
    CHECK(found.size() == 5);
    for (const auto& f : found) {
        if (f.path == root + "/game.PKG") CHECK(f.kind == "fpkg");
        if (f.path == root + "/game.ffpfsc") CHECK(f.kind == "ffpfsc" && f.size == 16);
        if (f.path == root + "/group/B") CHECK(f.kind == "folder" && f.folder == root + "/group");
    }
}

TEST_CASE(images_entries_carry_the_ampr_state) {
    const std::string root = test::make_temp_dir();
    EngineConfig config;
    config.state_dir = root + "/state";
    config.roots = {root};
    test::make_dir(config.state_dir);
    make_game(root + "/needs", "PPSA00001", true);
    make_game(root + "/plain", "PPSA00002", false);
    make_game(root + "/custom", "PPSA00003", true);
    test::make_dir(root + "/custom/fakelib");
    test::write_file(root + "/custom/fakelib/libSceAmpr.sprx", std::vector<std::uint8_t>(64, 7));

    auto needs = images::entry_json({root + "/needs", "folder", root, 0, 0, ""}, config);
    CHECK(needs["name"].str() == "needs");
    CHECK(needs["game"].boolean());
    CHECK(!needs["hidden"].boolean());
    CHECK(needs["allowed"].boolean());
    CHECK(needs["ampr"]["status"].str() == "missing");
    CHECK(needs["ampr"]["target"].str() == "ampr_runtime"); // nothing downloaded: ampr_emu

    CHECK(images::ampr_of({root + "/plain", "folder", root, 0, 0, ""}, config)["status"].str() == "none");
    CHECK(images::ampr_of({root + "/x.pkg", "fpkg", root, 0, 0, ""}, config).is_null());

    auto custom = images::ampr_of({root + "/custom", "folder", root, 0, 0, ""}, config);
    CHECK(custom["status"].str() == "unknown");
    CHECK(custom["where"].str() == "game");
    CHECK(custom["emulator"].is_null());
}

TEST_CASE(images_downloads_are_searched_for_packages_only) {
    const std::string root = test::make_temp_dir();
    test::make_dir(root + "/app");
    test::make_dir(root + "/app/deep");
    test::make_dir(root + "/app/deep/er");
    test::write_file(root + "/top.pkg", std::vector<std::uint8_t>(8, 0));
    test::write_file(root + "/app/deep/er/game.PKG", std::vector<std::uint8_t>(8, 0));
    test::write_file(root + "/app/image.ffpfsc", std::vector<std::uint8_t>(8, 0)); // not a package: left out
    test::make_dir(root + "/.cache");
    test::write_file(root + "/.cache/hidden.pkg", std::vector<std::uint8_t>(8, 0)); // in a hidden folder: left out
    make_game(root + "/app/Folder", "PPSA00009", true); // a game folder: left out
    const auto found = images::scan_packages(root);
    CHECK(found.size() == 2);
    CHECK(has(found, root + "/top.pkg"));
    CHECK(has(found, root + "/app/deep/er/game.PKG"));
    for (const auto& f : found) CHECK(f.kind == "fpkg");
}

TEST_CASE(images_alembic_app_is_not_an_apr_user) {
    // The app's texts name libSceAmpr; the check must not take it for a game.
    const std::string root = test::make_temp_dir();
    EngineConfig config;
    config.state_dir = root + "/state";
    config.roots = {root};
    test::make_dir(config.state_dir);
    make_game(root + "/PPSA99594", "PPSA99594", true);
    make_game(root + "/PPSA99595", "PPSA99595", true);
    CHECK(images::ampr_of({root + "/PPSA99594", "folder", root, 0, 0, ""}, config)["status"].str() == "none");
    CHECK(images::ampr_of({root + "/PPSA99595", "folder", root, 0, 0, ""}, config)["status"].str() == "none");
    // An "ampr_emu" job on it is refused.
    JobSpec spec;
    spec.kind = "ampr_emu";
    spec.input = root + "/PPSA99594";
    auto r = run_job(spec, config, {});
    CHECK(!r.ok());
}

TEST_CASE(images_repair_alembic_app_removes_only_what_the_job_wrote) {
    const std::string root = test::make_temp_dir();
    EngineConfig config;
    config.state_dir = root + "/state";
    config.backports_root = root + "/data-backports";
    const std::string scan = root + "/homebrew";
    test::make_dir(scan);
    // An image install's backport, as the job wrote it: removed.
    test::make_dir(config.backports_root);
    test::make_dir(config.backports_root + "/PPSA99594");
    test::make_dir(config.backports_root + "/PPSA99594/fakelib");
    test::write_file(config.backports_root + "/PPSA99594/fakelib/libSceAmpr.sprx", std::vector<std::uint8_t>(8, 1));
    test::write_file(config.backports_root + "/PPSA99594/ampr_emu.index", std::vector<std::uint8_t>(8, 2));
    // A scan folder's backport with something else in it: left alone.
    test::make_dir(scan + "/backports");
    test::make_dir(scan + "/backports/PPSA99594");
    test::make_dir(scan + "/backports/PPSA99594/fakelib");
    test::write_file(scan + "/backports/PPSA99594/fakelib/libSceAmpr.sprx", std::vector<std::uint8_t>(8, 1));
    test::write_file(scan + "/backports/PPSA99594/fakelib/other.sprx", std::vector<std::uint8_t>(8, 3));
    // A game's backport: never touched.
    test::make_dir(scan + "/backports/PPSA01234");
    test::make_dir(scan + "/backports/PPSA01234/fakelib");
    test::write_file(scan + "/backports/PPSA01234/fakelib/libSceAmpr.sprx", std::vector<std::uint8_t>(8, 1));
    // The app as a folder, with the two files added: they go, the app stays.
    make_game(scan + "/Alembic", "PPSA99594", true);
    test::make_dir(scan + "/Alembic/fakelib");
    test::write_file(scan + "/Alembic/fakelib/libSceAmpr.sprx", std::vector<std::uint8_t>(8, 1));
    test::write_file(scan + "/Alembic/ampr_emu.index", std::vector<std::uint8_t>(8, 2));

    const auto removed = images::repair_alembic_app(config, {scan});
    CHECK(!path_exists(config.backports_root + "/PPSA99594"));
    CHECK(is_regular_file(scan + "/backports/PPSA99594/fakelib/libSceAmpr.sprx"));
    CHECK(is_regular_file(scan + "/backports/PPSA01234/fakelib/libSceAmpr.sprx"));
    CHECK(!path_exists(scan + "/Alembic/fakelib"));
    CHECK(!path_exists(scan + "/Alembic/ampr_emu.index"));
    CHECK(is_regular_file(scan + "/Alembic/eboot.bin"));
    CHECK(!removed.empty());
}

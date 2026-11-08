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

TEST_CASE(images_scan_finds_folders_and_images) {
    const std::string root = test::make_temp_dir();
    make_game(root + "/A", "PPSA00001", true);
    test::make_dir(root + "/group");
    make_game(root + "/group/B", "PPSA00002", true);  // one level down
    make_game(root + "/.hidden", "PPSA00003", true);  // ShadowMount+ skips '.'
    test::make_dir(root + "/backports");
    make_game(root + "/backports/PPSA00004", "PPSA00004", true); // not a game source
    test::write_file(root + "/game.ffpfsc", std::vector<std::uint8_t>(16, 0));
    test::write_file(root + "/notes.txt", std::vector<std::uint8_t>(4, 0));
    const auto found = images::scan({root});
    CHECK(has(found, root + "/A"));
    CHECK(has(found, root + "/group/B"));
    CHECK(has(found, root + "/game.ffpfsc"));
    CHECK(!has(found, root + "/.hidden"));
    CHECK(!has(found, root + "/backports/PPSA00004"));
    CHECK(!has(found, root + "/notes.txt"));
    CHECK(found.size() == 3);
}

TEST_CASE(images_evaluate_missing_unknown_and_no_apr) {
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

    auto needs = images::evaluate({root + "/needs", ""}, config);
    CHECK(needs.has_value());
    if (!needs) return;
    CHECK((*needs)["status"].str() == "missing");
    CHECK((*needs)["target"].str() == "ampr_runtime"); // nothing downloaded: ampr_emu
    CHECK((*needs)["title_id"].str() == "PPSA00001");
    CHECK((*needs)["allowed"].boolean());

    bool unreadable = true;
    CHECK(!images::evaluate({root + "/plain", ""}, config, &unreadable).has_value());
    CHECK(!unreadable);

    auto custom = images::evaluate({root + "/custom", ""}, config);
    CHECK(custom.has_value());
    if (!custom) return;
    CHECK((*custom)["status"].str() == "unknown");
    CHECK((*custom)["where"].str() == "game");
    CHECK((*custom)["emulator"].is_null());
}

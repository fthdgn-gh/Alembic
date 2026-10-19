// SPDX-License-Identifier: GPL-3.0-or-later
// LZ4 asset packs. Byte-for-byte parity with ampr_pack.py is checked by
// tools/lz4-oracle/compare.sh; these tests cover the pieces on their own.
#include "alembic/lz4pack.hpp"
#include "test.hpp"

#include <string>

using namespace alembic;
using namespace alembic::lz4pack;

namespace {

std::vector<std::uint8_t> text(std::size_t size, unsigned seed) {
    static const char* words[] = {"mesh ", "level ", "texture ", "0.25 ", "{\n", "}\n", "alpha "};
    std::vector<std::uint8_t> out;
    std::uint32_t x = seed * 2654435761u + 1;
    while (out.size() < size) {
        x = x * 1103515245u + 12345u;
        for (const char* c = words[(x >> 16) % 7]; *c && out.size() < size; ++c) out.push_back(static_cast<std::uint8_t>(*c));
    }
    return out;
}

std::vector<std::uint8_t> noise(std::size_t size, unsigned seed) {
    std::vector<std::uint8_t> out(size);
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 7;
    for (auto& b : out) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        b = static_cast<std::uint8_t>(x);
    }
    return out;
}

std::string make_game(const std::string& parent) {
    std::string root = parent + "/game";
    for (const char* d : {"", "/sce_sys", "/fakelib", "/assets", "/assets/sub", "/sound"}) test::make_dir(root + d);
    test::write_file(root + "/eboot.bin", noise(70000, 1));
    test::write_file(root + "/sce_sys/param.json", text(300, 2));
    test::write_file(root + "/fakelib/libSceAmpr.sprx", noise(5000, 3));
    test::write_file(root + "/assets/a.res", text(200000, 4));
    test::write_file(root + "/assets/b.res", noise(70000, 5));
    std::vector<std::uint8_t> twice = text(65536, 6);
    twice.insert(twice.end(), twice.begin(), twice.end()); // one block twice: de-duplicated within a lane
    test::write_file(root + "/assets/sub/c.res", twice);
    test::write_file(root + "/assets/sub/copy.res", text(65536, 6)); // same block, but likely another lane
    test::write_file(root + "/assets/empty.res", {});
    test::write_file(root + "/sound/x.bank", text(40000, 7));
    return root;
}

Result<std::vector<std::uint8_t>> read(const tree::Source& source, const std::string& path) {
    const tree::Node* node = tree::find(source.root(), path);
    if (!node) return make_error(Errc::invalid_argument, "missing " + path);
    ALEMBIC_ASSIGN(auto file, source.open(*node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node->size));
    ALEMBIC_TRY(file->read_at(0, data));
    return data;
}

} // namespace

TEST_CASE(lz4_glob_follows_fnmatch) {
    CHECK(glob_match("*/**", "a/b"));
    CHECK(!glob_match("*/**", "a"));
    CHECK(glob_match("*.prx", "dir/x.prx")); // '*' crosses '/'
    CHECK(glob_match("**/*.dat", "a/b.dat"));
    CHECK(!glob_match("**/*.dat", "b.dat"));
    CHECK(glob_match("/sce_sys/**", "sce_sys/param.json")); // leading slashes ignored
    CHECK(glob_match("data\\x?.bin", "data/x1.bin"));
    CHECK(glob_match("[[]weird] name.bin", "[weird] name.bin"));
    CHECK(glob_match("x[!ab].c", "xc.c"));
    CHECK(!glob_match("x[!ab].c", "xa.c"));
    CHECK(glob_match("x[a-c].c", "xb.c"));
    CHECK(glob_match("[", "["));            // unclosed '[' is a literal
    CHECK(glob_match("?t?", "\xC3\xA9t\xC3\xA9")); // '?' is one code point
    CHECK(!glob_match("A*", "a")); // case-sensitive
}

TEST_CASE(lz4_manifest_round_trips_and_rejects_damage) {
    auto dir = test::make_temp_dir();
    auto folder = tree::open_folder(make_game(dir));
    REQUIRE_OK(folder);
    PackOptions options;
    options.config = automatic_profile((*folder)->root());
    options.config.workers = 2;
    options.timestamp = 1700000000;
    auto packed = pack(**folder, options);
    REQUIRE_OK(packed);
    auto bytes = read(**packed, kIndexName);
    REQUIRE_OK(bytes);
    auto parsed = parse_manifest(*bytes);
    REQUIRE_OK(parsed);
    CHECK(encode_manifest(*parsed) == *bytes);
    CHECK(parsed->build_id == (*packed)->build_id());

    std::vector<std::uint8_t> damaged = *bytes;
    damaged[damaged.size() - 3] ^= 1; // payload CRC
    CHECK(!parse_manifest(damaged).ok());
    damaged = *bytes;
    damaged[40] ^= 1; // file count, header CRC
    CHECK(!parse_manifest(damaged).ok());
}

TEST_CASE(lz4_pack_verify_unpack) {
    auto dir = test::make_temp_dir();
    auto folder = tree::open_folder(make_game(dir));
    REQUIRE_OK(folder);
    PackOptions options;
    options.config = automatic_profile((*folder)->root());
    options.config.workers = 3;
    options.timestamp = 1700000000;
    auto packed = pack(**folder, options);
    REQUIRE_OK(packed);
    const PackStats& st = (*packed)->stats();
    CHECK(st.files_packed >= 4);
    CHECK(st.chunks_shared >= 1); // the second block of c.res
    CHECK(st.chunks_lz4 >= 1);
    CHECK(st.chunks_raw >= 1); // noise

    // Packed originals are gone, loose and system files stay.
    const tree::Node& root = (*packed)->root();
    CHECK(tree::find(root, "assets/a.res") == nullptr);
    CHECK(tree::find(root, "assets/empty.res") != nullptr); // empty files stay loose
    CHECK(tree::find(root, "eboot.bin") != nullptr);
    CHECK(tree::find(root, "sce_sys/param.json") != nullptr);
    CHECK(tree::find(root, "ampr_emu.index") != nullptr);
    CHECK(tree::find(root, "ampr_assets.index.crc") != nullptr);
    CHECK(tree::find(root, "ampr_assets.index.runtime") != nullptr);

    auto verified = verify(**packed);
    REQUIRE_OK(verified);
    CHECK_EQ(verified->files, st.files_packed);

    auto unpacked = unpack(**packed);
    REQUIRE_OK(unpacked);
    for (const char* path : {"assets/a.res", "assets/b.res", "assets/sub/c.res", "assets/sub/copy.res", "sound/x.bank",
                             "eboot.bin"}) {
        auto original = read(**folder, path);
        auto back = read(**unpacked, path);
        REQUIRE_OK(original);
        REQUIRE_OK(back);
        CHECK(*original == *back);
    }
    CHECK(tree::find((*unpacked)->root(), kIndexName) == nullptr);

    // Reading a volume again, or from the middle, gives the same bytes.
    const tree::Node* volume = nullptr;
    for (const tree::Node& n : root.children)
        if (n.name.find(".pak") != std::string::npos && n.size > 70000) volume = &n;
    REQUIRE_OK(volume ? Status{} : Status(make_error(Errc::invalid_data, "no volume")));
    auto whole = read(**packed, volume->name);
    REQUIRE_OK(whole);
    auto file = (*packed)->open(*volume);
    REQUIRE_OK(file);
    std::vector<std::uint8_t> part(5000);
    REQUIRE_OK((*file)->read_at(65536 + 1234, part));
    CHECK(std::equal(part.begin(), part.end(), whole->begin() + 65536 + 1234));
}

TEST_CASE(lz4_pack_is_deterministic_across_thread_counts) {
    auto dir = test::make_temp_dir();
    auto folder = tree::open_folder(make_game(dir));
    REQUIRE_OK(folder);
    std::vector<std::uint8_t> first;
    for (unsigned threads : {1u, 4u}) {
        PackOptions options;
        options.config = automatic_profile((*folder)->root());
        options.config.workers = threads;
        options.timestamp = 1700000000;
        auto packed = pack(**folder, options);
        REQUIRE_OK(packed);
        auto bytes = read(**packed, kIndexName);
        REQUIRE_OK(bytes);
        if (first.empty()) first = *bytes;
        else CHECK(first == *bytes);
    }
}

TEST_CASE(lz4_pack_needs_a_runtime) {
    auto dir = test::make_temp_dir();
    std::string root = make_game(dir);
    std::remove((root + "/fakelib/libSceAmpr.sprx").c_str());
    auto folder = tree::open_folder(root);
    REQUIRE_OK(folder);
    PackOptions options;
    options.config = automatic_profile((*folder)->root());
    CHECK(!pack(**folder, options).ok());
}

// ---- checkpoints: stopped in pass 1 or while the output is written, a run
// continues and gives the same bytes as an uninterrupted one.
#include "alembic/convert.hpp"
#include "alembic/resume.hpp"

TEST_CASE(lz4_pack_resumes_in_both_passes) {
    auto dir = test::make_temp_dir();
    std::string game = make_game(dir);
    auto folder = tree::open_folder(game);
    REQUIRE_OK(folder);
    convert::Options base;
    PackOptions pack;
    pack.config = automatic_profile((*folder)->root());
    pack.config.workers = 3;
    pack.timestamp = 1700000000;
    base.lz4 = pack;
    std::string reference = dir + "/ref.exfat";
    REQUIRE_OK(convert::run(game, reference, convert::Format::exfat, base));

    for (bool in_pass_one : {true, false}) {
        std::string output = dir + (in_pass_one ? "/a.exfat" : "/b.exfat");
        std::vector<resume::State> saved;
        resume::Session session;
        session.partial_path = output + resume::kPartialSuffix;
        session.interval = 64 * 1024;
        session.save = [&](const resume::State& s) {
            saved.push_back(s);
            return Status{};
        };
        convert::Options o = base;
        o.resume = &session;
        auto writer_saves = [&] {
            std::size_t n = 0;
            for (const auto& s : saved) n += resume::get(s, "format") != resume::kLz4Format;
            return n;
        };
        auto stopped = convert::run(game, output, convert::Format::exfat, o, [&](const ProgressEvent& e) {
            if (in_pass_one) return !(e.stage == std::string_view("LZ4 pass 1") && saved.size() >= 3);
            return writer_saves() < 2;
        });
        CHECK(!stopped.ok());
        CHECK(!saved.empty());
        if (saved.empty()) continue;
        const resume::State& last = saved.back();
        CHECK(resume::get(last, "lz4_timestamp") == 1700000000u);
        if (in_pass_one) {
            CHECK(resume::get(last, "format") == resume::kLz4Format);
            CHECK(resume::get(last, "lz4_done") == 0u);
        } else {
            CHECK(resume::get(last, "format") != resume::kLz4Format);
            CHECK(resume::get(last, "lz4_done") == 1u);
        }
        CHECK(is_regular_file(session.partial_path + ".lz4"));
        session.from = last;
        std::uint64_t first_pass_one = ~std::uint64_t{0};
        auto resumed = convert::run(game, output, convert::Format::exfat, o, [&](const ProgressEvent& e) {
            if (e.stage == std::string_view("LZ4 pass 1") && first_pass_one == ~std::uint64_t{0}) first_pass_one = e.done;
            return true;
        });
        REQUIRE_OK(resumed);
        CHECK(test::read_file(output) == test::read_file(reference));
        // Pass 1 replayed what it had: its first progress is past the checkpoint.
        if (in_pass_one) CHECK(first_pass_one != ~std::uint64_t{0} && first_pass_one > 64 * 1024);
        else CHECK(first_pass_one == ~std::uint64_t{0}); // nothing left to compress in pass 1
    }
}

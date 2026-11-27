// SPDX-License-Identifier: GPL-3.0-or-later
// The package writer's parallel paths give the same bytes as the serial ones.
#include "alembic/fpkg_build.hpp"
#include "alembic/lz4pack.hpp"
#include "alembic/zstdpack.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace alembic;
using namespace alembic::fpkg::build;

namespace {

class VectorSink final : public ByteSink {
public:
    Status write(Bytes data) override {
        bytes.insert(bytes.end(), data.begin(), data.end());
        return {};
    }
    std::vector<std::uint8_t> bytes;
};

struct Built {
    std::vector<std::uint8_t> bytes;
    OuterPfs pfs;
};

// Two files: one of 200.5 blocks (several batches and a partial block), and
// a small signed one, like pfs_image.dat and naps_pkg_layout.dat.
Built build_outer(unsigned threads) {
    OuterOptions o;
    o.content_id = "UP0000-TEST00000_00-0000000000000000";
    o.passcode = std::string(32, '0');
    for (int i = 0; i < 16; ++i) o.seed[i] = static_cast<std::uint8_t>(i * 9);
    o.threads = threads;
    VectorSink sink;
    OuterPfsWriter w(sink, o);
    std::vector<std::uint8_t> data(200 * kInnerBlock + kInnerBlock / 2);
    std::uint32_t x = 7;
    for (auto& b : data) b = static_cast<std::uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
    Built out;
    if (!w.begin_file("pfs_image.dat", false).ok()) return out;
    // Odd write sizes, so blocks are assembled across calls.
    for (std::size_t at = 0; at < data.size();) {
        std::size_t n = std::min<std::size_t>(12345, data.size() - at);
        if (!w.write(Bytes(data.data() + at, n)).ok()) return out;
        at += n;
    }
    if (!w.end_file(data.size()).ok()) return out;
    std::vector<std::uint8_t> small(3000, 0x5A);
    if (!w.begin_file("naps_pkg_layout.dat", true).ok() || !w.write(small).ok() || !w.end_file(small.size()).ok()) return out;
    auto pfs = w.finish();
    if (!pfs.ok()) return out;
    out.pfs = std::move(*pfs);
    out.bytes = std::move(sink.bytes);
    return out;
}

} // namespace

TEST_CASE(fpkg_outer_pfs_threads_match_serial) {
    Built serial = build_outer(1);
    Built parallel = build_outer(4);
    CHECK(!serial.bytes.empty());
    CHECK(serial.bytes.size() == static_cast<std::size_t>(serial.pfs.block_count) * kInnerBlock);
    CHECK(serial.bytes == parallel.bytes);
    CHECK(serial.pfs.block_digests == parallel.pfs.block_digests);
    CHECK(serial.pfs.superblock_plaintext == parallel.pfs.superblock_plaintext);
    CHECK_EQ(serial.pfs.block_count, parallel.pfs.block_count);
}

// ---- package checkpoints ----
#include "alembic/convert.hpp"
#include "alembic/io.hpp"
#include "alembic/resume.hpp"

#include <string>

namespace {

// ProsperoPkgTool's public key moduli, from its checkout when it is there.
std::string keys_folder() {
    std::string k = test::source_dir() + "/.refs/ProsperoPkgTool/ProsperoPkgTool/Keys";
    if (is_regular_file(k + "/passcode.bin") && is_regular_file(k + "/mount_image.bin")) return k;
    return "";
}

std::vector<std::uint8_t> pattern(std::size_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> d(size);
    std::uint32_t x = seed * 2654435761u + 1;
    for (std::size_t i = 0; i < size; ++i) d[i] = i < 4 ? 0 : static_cast<std::uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
    return d;
}

std::string make_package_game(const std::string& parent) {
    std::string root = parent + "/game";
    test::make_dir(root);
    test::make_dir(root + "/sce_sys");
    test::make_dir(root + "/data");
    std::string param = "{\"titleId\":\"TEST00000\",\"contentId\":\"UP0000-TEST00000_00-0000000000000000\","
                        "\"contentVersion\":\"01.000.000\"}";
    test::write_file(root + "/sce_sys/param.json", std::vector<std::uint8_t>(param.begin(), param.end()));
    test::write_file(root + "/sce_sys/keystone", pattern(96, 9));
    test::write_file(root + "/eboot.bin", pattern(700000, 1));
    test::write_file(root + "/data/a.bin", pattern(3 * 1024 * 1024 + 5, 2));
    test::write_file(root + "/data/b.bin", pattern(1500000, 3));
    test::write_file(root + "/data/c.bin", pattern(10, 4));
    // Compressible, so a Kraken build has pieces of every size on disk.
    std::vector<std::uint8_t> text;
    for (std::uint32_t i = 0; text.size() < 2 * 1024 * 1024; ++i) {
        std::string line = "line " + std::to_string(i * 7919 % 100003) + " of the alembic test game\n";
        text.insert(text.end(), line.begin(), line.end());
    }
    test::write_file(root + "/data/t.txt", text);
    return root;
}

convert::Options package_options(const std::string& keys, unsigned threads) {
    convert::Options o;
    o.fpkg.keys_folder = keys;
    std::array<std::uint8_t, 16> seed{};
    for (int i = 0; i < 16; ++i) seed[i] = static_cast<std::uint8_t>(0x10 + i);
    o.fpkg.seed = seed;
    o.fpkg.threads = threads;
    return o;
}

} // namespace

TEST_CASE(fpkg_resume_matches_uninterrupted) {
    std::string keys = keys_folder();
    if (keys.empty()) {
        std::printf("  (skipped: no ProsperoPkgTool key moduli in .refs)\n");
        return;
    }
    std::string tmp = test::make_temp_dir();
    std::string game = make_package_game(tmp);
    for (auto [threads, compression] : {std::pair{1u, fpkg::build::Compression::stored},
                                        std::pair{3u, fpkg::build::Compression::stored},
                                        std::pair{1u, fpkg::build::Compression::kraken},
                                        std::pair{3u, fpkg::build::Compression::kraken}}) {
        const bool kraken = compression == fpkg::build::Compression::kraken;
        std::string reference = tmp + "/ref.pkg", output = tmp + "/out.pkg";
        convert::Options plain = package_options(keys, threads);
        plain.fpkg.compression = compression;
        plain.overwrite = true;
        REQUIRE_OK(convert::run(game, reference, convert::Format::fpkg, plain));
        for (bool latest : {false, true}) {
            std::vector<resume::State> saved;
            resume::Session session;
            session.partial_path = output + ".partial";
            session.interval = 256 * 1024;
            session.save = [&](const resume::State& s) {
                saved.push_back(s);
                return Status{};
            };
            convert::Options o = plain;
            o.resume = &session;
            (void)remove_file(output);
            auto stopped = convert::run(game, output, convert::Format::fpkg, o, [&](const ProgressEvent&) { return saved.size() < 5; });
            CHECK(!stopped.ok());
            CHECK(saved.size() >= 5);
            if (saved.size() < 5) return;
            CHECK(is_regular_file(session.partial_path + ".state"));
            CHECK(is_regular_file(session.partial_path + ".digests"));
            CHECK(is_regular_file(session.partial_path + ".chunks") == kraken);
            // A crash leaves torn data past the checkpoint.
            std::vector<std::uint8_t> partial = test::read_file(session.partial_path);
            partial.insert(partial.end(), 100000, 0xA5);
            test::write_file(session.partial_path, partial);
            session.from = latest ? saved.back() : saved[2];
            // An older checkpoint's side file is gone (it was replaced): only the latest can continue.
            if (!latest) {
                auto stale = convert::run(game, output, convert::Format::fpkg, o);
                CHECK(!stale.ok());
                CHECK(!stale.ok() && stale.error().message.find("checkpoint") != std::string::npos);
                session.from = saved.back();
            }
            std::uint64_t first_done = ~std::uint64_t{0};
            auto resumed = convert::run(game, output, convert::Format::fpkg, o, [&](const ProgressEvent& e) {
                if (first_done == ~std::uint64_t{0} && e.total && e.stage == std::string_view("Building the inner image")) first_done = e.done;
                return true;
            });
            REQUIRE_OK(resumed);
            CHECK(resumed->resumable);
            CHECK(first_done != ~std::uint64_t{0} && first_done > 4 * session.interval); // did not start over
            CHECK(test::read_file(output) == test::read_file(reference));
            CHECK(!path_exists(session.partial_path));
            CHECK(!path_exists(session.partial_path + ".state"));
            CHECK(!path_exists(session.partial_path + ".digests"));
            CHECK(!path_exists(session.partial_path + ".chunks"));
            // A checkpoint is only good for the compression it was made with.
            if (kraken) {
                convert::Options other = o;
                other.fpkg.compression = fpkg::build::Compression::stored;
                session.from = saved.back();
                (void)remove_file(output);
                auto wrong = convert::run(game, output, convert::Format::fpkg, other);
                CHECK(!wrong.ok());
            }
        }
        if (kraken) CHECK(test::read_file(reference).size() < 7 * 1024 * 1024); // the text shrank
    }
}

#include "alembic/crypto.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/fpkg_install.hpp"
#include "alembic/bytes.hpp"

#include <cstring>

// The installed form (fpkg_install.hpp): the split writer gives the same bytes
// as the whole package, and the files made from it follow the installer's.
TEST_CASE(fpkg_install_layout) {
    std::string keys = keys_folder();
    if (keys.empty()) {
        std::printf("  (skipped: no ProsperoPkgTool key moduli in .refs)\n");
        return;
    }
    std::string tmp = test::make_temp_dir();
    std::string game = make_package_game(tmp);
    convert::Options o = package_options(keys, 2);
    REQUIRE_OK(convert::run(game, tmp + "/whole.pkg", convert::Format::fpkg, o));
    o.fpkg.si_path = tmp + "/app.si";
    REQUIRE_OK(convert::run(game, tmp + "/app.pkg", convert::Format::fpkg, o));
    std::vector<std::uint8_t> whole = test::read_file(tmp + "/whole.pkg"), app = test::read_file(tmp + "/app.pkg"),
                              si = test::read_file(tmp + "/app.si");
    CHECK_EQ(app.size() + si.size(), whole.size());
    CHECK(std::equal(app.begin(), app.end(), whole.begin()));
    CHECK(std::equal(si.begin(), si.end(), whole.begin() + static_cast<std::ptrdiff_t>(app.size())));

    auto file = File::open(tmp + "/whole.pkg", File::Mode::read);
    REQUIRE_OK(file);
    auto package = fpkg::read_package(*file);
    REQUIRE_OK(package);
    const fpkg::CntEntry* param = nullptr;
    for (const auto& e : package->entries)
        if (e.id == 0x2000) param = &e;
    CHECK(param != nullptr);
    if (!param) return;
    auto param_json = fpkg::read_entry(*file, *package, *param);
    REQUIRE_OK(param_json);
    Bytes head(whole.data(), 0x10000), cnt(whole.data() + package->cnt_offset, 0x1000);
    auto in = fpkg::install::make_inputs(head, cnt, app.size(), *param_json, si);
    REQUIRE_OK(in);
    CHECK(in->title_id == "TEST00000");
    CHECK(in->master_version == "01.00"); // from contentVersion: the test game has no masterVersion
    auto files = fpkg::install::make_files(*in, "/data/x.pkg");
    REQUIRE_OK(files);
    auto from_files = fpkg::install::read_inputs(tmp + "/app.pkg", tmp + "/app.si");
    REQUIRE_OK(from_files);
    auto again = fpkg::install::make_files(*from_files, "/data/x.pkg");
    REQUIRE_OK(again);
    CHECK(again->app_pbm == files->app_pbm && again->app_json == files->app_json && again->d0_pdb == files->d0_pdb);
    auto members = fpkg::install::read_stored_zip(si);
    REQUIRE_OK(members);
    bool found = false;
    for (const auto& [name, data] : *members)
        if (name == "config/UP0000-TEST00000_00-0000000000000000/playgo-chunk.crc") found = data == files->app_crc;
    CHECK(found);

    const std::vector<std::uint8_t>& pbm = files->app_pbm;
    const std::uint64_t blocks = (app.size() + 0xFFFF) / 0x10000, bitmap = (blocks + 7) / 8;
    CHECK_EQ(pbm.size(), 0x100 + bitmap + 32);
    if (pbm.size() != 0x100 + bitmap + 32) return;
    CHECK(std::memcmp(pbm.data(), "pdbmTEST00000\0" "01.00\0", 20) == 0);
    CHECK_EQ(get_u64(&pbm[0x20]), static_cast<std::uint64_t>(app.size()));
    CHECK(std::memcmp(&pbm[0x60], cnt.data() + 0xFE0, 32) == 0);
    CHECK_EQ(get_u32(&pbm[0xAC]), static_cast<std::uint32_t>(bitmap));
    std::uint64_t bits = 0;
    for (std::uint64_t i = 0; i < bitmap; ++i) bits += static_cast<unsigned>(__builtin_popcount(pbm[0x100 + i]));
    CHECK_EQ(bits, blocks);
    crypto::Digest sha = crypto::sha256(Bytes(pbm.data(), pbm.size() - 32));
    CHECK(std::memcmp(sha.data(), pbm.data() + pbm.size() - 32, 32) == 0);

    for (const std::vector<std::uint8_t>* pdb : {&files->d0_pdb, &files->d1_pdb}) {
        CHECK(pdb->size() > 8);
        if (pdb->size() <= 8) return;
        CHECK_EQ(get_u32(pdb->data()), 1u);
        CHECK_EQ(get_u32(pdb->data() + pdb->size() - 4), crypto::crc32c(Bytes(pdb->data(), pdb->size() - 4)));
        std::size_t at = 4, records = 0;
        while (at + 12 <= pdb->size() - 4) {
            CHECK_EQ(get_u32(pdb->data() + at + 4), get_u32(pdb->data() + at + 8));
            at += 12 + get_u32(pdb->data() + at + 4);
            ++records;
        }
        CHECK_EQ(at, pdb->size() - 4);
        CHECK_EQ(records, std::size_t{52});
    }
    CHECK_EQ(files->d0_pdb.size(), files->d1_pdb.size());

    std::string json(files->app_json.begin(), files->app_json.end()), xml(files->app_xml.begin(), files->app_xml.end());
    CHECK(json.find("\"originalFileSize\":" + std::to_string(app.size()) + ",") != std::string::npos);
    CHECK(json.find("\"url\":\"\\/data\\/x.pkg\"") != std::string::npos);
    CHECK(xml.rfind("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<psproject fmt=\"playgo-status\" version=\"2000\">", 0) == 0);
    CHECK(xml.find("<chunk_status chunk_count=\"1\"><chunks language_mask=\"18446744073709551615\" scenario_mask=\"1\">"
                   "<chunk id=\"0\" locus=\"3\" req_locus=\"3\"/></chunks><scenarios><scenario>0</scenario></scenarios>") !=
          std::string::npos);
}

// Issue #7: a package of a game that went through an image or folder with
// the AMPR emulator (or LZ4 or zstd packs) is the package of the game itself,
// byte for byte; --keep-ampr-emu packages it as it is.
TEST_CASE(fpkg_leaves_out_the_ampr_emulator) {
    std::string keys = keys_folder();
    if (keys.empty()) {
        std::printf("  (skipped: no ProsperoPkgTool key moduli in .refs)\n");
        return;
    }
    std::string tmp = test::make_temp_dir();
    std::string game = make_package_game(tmp);
    test::write_file(tmp + "/libSceAmpr.sprx", pattern(12345, 7));
    auto emulator_file = File::open(tmp + "/libSceAmpr.sprx", File::Mode::read);
    REQUIRE_OK(emulator_file);
    std::shared_ptr<const RandomAccess> emulator = std::make_shared<File>(std::move(*emulator_file));
    convert::Options pkg = package_options(keys, 2);
    std::string reference = tmp + "/ref.pkg";
    auto clean = convert::run(game, reference, convert::Format::fpkg, pkg);
    REQUIRE_OK(clean);
    CHECK(clean->ampr_removed.empty());
    const std::vector<std::uint8_t> want = test::read_file(reference);

    // The same game with ampr_emu, LZ4 packs or zstd packs, in a folder.
    convert::Options with_emu;
    with_emu.ampr_emulator = emulator;
    with_emu.ampr_emulator_always = true;
    convert::Options with_lz4;
    {
        auto folder = tree::open_folder(game);
        REQUIRE_OK(folder);
        lz4pack::PackOptions p;
        p.config = lz4pack::automatic_profile((*folder)->root());
        p.config.workers = 2;
        p.timestamp = 1700000000;
        p.runtime_module = emulator;
        with_lz4.lz4 = p;
    }
    convert::Options with_zstd;
    {
        zstdpack::PackOptions p;
        p.timestamp = 1700000000;
        p.workers = 2;
        p.runtime_module = emulator;
        p.spill_path = tmp + "/zstd-frames.tmp";
        with_zstd.zstd = p;
    }
    int n = 0;
    for (const convert::Options* via : {&with_emu, &with_lz4, &with_zstd}) {
        std::string folder = tmp + "/via" + std::to_string(++n) + "/";
        auto made = convert::run(game, folder, convert::Format::folder, *via);
        REQUIRE_OK(made);
        CHECK(is_regular_file(folder + "fakelib/libSceAmpr.sprx"));
        CHECK(is_regular_file(folder + "ampr_emu.index"));

        std::string out = tmp + "/clean" + std::to_string(n) + ".pkg";
        auto r = convert::run(folder, out, convert::Format::fpkg, pkg);
        REQUIRE_OK(r);
        CHECK(std::find(r->ampr_removed.begin(), r->ampr_removed.end(), "fakelib/libSceAmpr.sprx") != r->ampr_removed.end());
        CHECK(std::find(r->ampr_removed.begin(), r->ampr_removed.end(), "ampr_emu.index") != r->ampr_removed.end());
        auto same = convert::compare(reference, out); // a package adds its own sce_sys files
        REQUIRE_OK(same);
        CHECK(same->only_in_a.empty());
        CHECK(same->only_in_b.empty());
        CHECK(same->different.empty());
        CHECK(test::read_file(out) == want);

        // Kept: the emulator goes into the package.
        convert::Options keep = pkg;
        keep.keep_ampr_emulator = true;
        std::string kept = tmp + "/kept" + std::to_string(n) + ".pkg";
        auto k = convert::run(folder, kept, convert::Format::fpkg, keep);
        REQUIRE_OK(k);
        CHECK(k->ampr_removed.empty());
        auto in = convert::open(kept);
        REQUIRE_OK(in);
        CHECK(tree::find(in->tree->root(), "fakelib/libSceAmpr.sprx") != nullptr);
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The package writer's parallel paths give the same bytes as the serial ones.
#include "alembic/fpkg_build.hpp"
#include "test.hpp"

#include <cstdio>
#include <unistd.h>
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
#ifdef ALEMBIC_SOURCE_DIR
    std::string k = std::string(ALEMBIC_SOURCE_DIR) + "/.refs/ProsperoPkgTool/ProsperoPkgTool/Keys";
    if (is_regular_file(k + "/passcode.bin") && is_regular_file(k + "/mount_image.bin")) return k;
#endif
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
    for (unsigned threads : {1u, 3u}) {
        std::string reference = tmp + "/ref.pkg", output = tmp + "/out.pkg";
        convert::Options plain = package_options(keys, threads);
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
            ::unlink(output.c_str());
            auto stopped = convert::run(game, output, convert::Format::fpkg, o, [&](const ProgressEvent&) { return saved.size() < 5; });
            CHECK(!stopped.ok());
            CHECK(saved.size() >= 5);
            if (saved.size() < 5) return;
            CHECK(is_regular_file(session.partial_path + ".state"));
            CHECK(is_regular_file(session.partial_path + ".digests"));
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
        }
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Interrupted conversions continued from a checkpoint must give the same
// bytes as an uninterrupted run.
#include "alembic/convert.hpp"
#include "alembic/resume.hpp"
#include "test.hpp"

#include <string>
#include <vector>

using namespace alembic;

namespace {

// Half compressible, half not, so PFSC stores both kinds of blocks.
std::vector<std::uint8_t> content(std::size_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> data(size);
    std::uint32_t x = seed * 2654435761u + 1;
    for (std::size_t i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        data[i] = (i / 65536) % 2 ? static_cast<std::uint8_t>(x >> 24) : static_cast<std::uint8_t>(i / 4096);
    }
    return data;
}

std::string make_game(const std::string& parent) {
    std::string root = parent + "/game";
    test::make_dir(root);
    test::make_dir(root + "/sce_sys");
    test::make_dir(root + "/data");
    std::string param = "{\"titleId\":\"ABCD12345\"}";
    test::write_file(root + "/sce_sys/param.json", std::vector<std::uint8_t>(param.begin(), param.end()));
    test::write_file(root + "/eboot.bin", content(700000, 1));
    test::write_file(root + "/data/a.bin", content(3 * 1024 * 1024 + 5, 2));
    test::write_file(root + "/data/b.bin", content(1500000, 3));
    test::write_file(root + "/data/c.bin", content(10, 4));
    return root;
}

convert::Options base_options() {
    convert::Options o;
    o.ffpfsc.build_timestamp = 1700000000; // also the PFS time stamp
    o.ufs2.timestamp = 1700000000;
    o.ufs2.process_id = 4242;
    o.ffpfsc.compression.threads = 3;
    return o;
}

// Runs `input` -> `output` once without stopping (the reference), then stops
// a second run part way, damages the partial file past an older checkpoint,
// continues from that checkpoint and compares.
// `latest`: continue from the checkpoint saved when the run stopped, not an older one.
void check_resume(const std::string& input, const std::string& dir, const std::string& name, convert::Format to,
                  bool latest = false) {
    std::string reference = dir + "/ref-" + name, output = dir + "/out-" + name;
    REQUIRE_OK(convert::run(input, reference, to, base_options()));

    std::vector<resume::State> saved;
    resume::Session session;
    session.partial_path = output + ".partial";
    session.interval = 256 * 1024;
    session.save = [&](const resume::State& s) {
        saved.push_back(s);
        return Status{};
    };
    convert::Options o = base_options();
    o.resume = &session;
    auto stopped = convert::run(input, output, to, o, [&](const ProgressEvent&) { return saved.size() < 4; });
    CHECK(!stopped.ok());
    CHECK(!path_exists(output));
    CHECK(saved.size() >= 2);
    if (saved.size() < 2) return;
    // A crash leaves data past the last checkpoint, possibly torn.
    std::vector<std::uint8_t> partial = test::read_file(session.partial_path);
    std::vector<std::uint8_t> junk(100000, 0xA5);
    partial.insert(partial.end(), junk.begin(), junk.end());
    test::write_file(session.partial_path, partial);

    // The stop itself saves one more, at the exact point it stopped.
    CHECK(saved.size() >= 4);
    session.from = latest ? saved.back() : saved[1];
    saved.clear();
    std::uint64_t first_done = ~std::uint64_t{0};
    auto resumed = convert::run(input, output, to, o, [&](const ProgressEvent& e) {
        if (first_done == ~std::uint64_t{0} && e.total) first_done = e.done;
        return true;
    });
    REQUIRE_OK(resumed);
    CHECK(first_done != ~std::uint64_t{0} && first_done >= (latest ? 3 : 2) * session.interval); // not at 0
    CHECK(resumed->resumable);
    CHECK(test::read_file(output) == test::read_file(reference));
    CHECK(!path_exists(session.partial_path));
}

} // namespace

TEST_CASE(resume_folder_to_ffpfsc) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    check_resume(game, tmp, "a.ffpfsc", convert::Format::ffpfsc);
}

TEST_CASE(resume_folder_to_exfat) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    check_resume(game, tmp, "a.exfat", convert::Format::exfat);
}

TEST_CASE(resume_image_to_ffpfsc_and_back) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    std::string image = tmp + "/game.exfat";
    REQUIRE_OK(convert::run(game, image, convert::Format::exfat, base_options()));
    check_resume(image, tmp, "b.ffpfsc", convert::Format::ffpfsc); // compresses the image as it is
    std::string compressed = tmp + "/ref-b.ffpfsc";
    check_resume(compressed, tmp, "b.exfat", convert::Format::exfat); // copies the image out
}

TEST_CASE(resume_rejects_a_foreign_checkpoint) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    resume::Session session;
    session.partial_path = tmp + "/x.partial";
    test::write_file(session.partial_path, {1, 2, 3});
    session.from = resume::State{{"format", resume::kSequentialFormat}, {"length", 1}, {"written", 1}};
    convert::Options o = base_options();
    o.resume = &session;
    CHECK(!convert::run(game, tmp + "/x.ffpfsc", convert::Format::ffpfsc, o).ok());
    CHECK(!convert::run(game, tmp + "/x.exfat", convert::Format::exfat, o).ok());
}

TEST_CASE(resume_from_the_stop_point) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    check_resume(game, tmp, "c.ffpfsc", convert::Format::ffpfsc, true);
    check_resume(game, tmp, "c.exfat", convert::Format::exfat, true);
}

TEST_CASE(resume_folder_to_ffpkg_and_ffpfs) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    check_resume(game, tmp, "d.ffpkg", convert::Format::ffpkg);
    check_resume(game, tmp, "d.ffpfs", convert::Format::ffpfs, true);
}

TEST_CASE(resume_ffpkg_keeps_its_clock_values) {
    // Without fixed values the first run picks the time and process ID; the
    // continued run must reuse them, or the image would mix two of each.
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    std::string out = tmp + "/e.ffpkg";
    std::vector<resume::State> saved;
    resume::Session session;
    session.partial_path = out + ".partial";
    session.interval = 256 * 1024;
    session.save = [&](const resume::State& st) {
        saved.push_back(st);
        return Status{};
    };
    convert::Options o;
    o.resume = &session;
    CHECK(!convert::run(game, out, convert::Format::ffpkg, o, [&](const ProgressEvent&) { return saved.size() < 2; }).ok());
    CHECK(!saved.empty());
    if (saved.empty()) return;
    session.from = saved.back();
    REQUIRE_OK(convert::run(game, out, convert::Format::ffpkg, o));
    // The same image again, in one go, with the checkpoint's values.
    convert::Options fixed;
    fixed.ufs2.timestamp = static_cast<std::int64_t>(resume::get(saved.back(), "timestamp"));
    fixed.ufs2.process_id = static_cast<std::int32_t>(resume::get(saved.back(), "pid"));
    REQUIRE_OK(convert::run(game, tmp + "/e-ref.ffpkg", convert::Format::ffpkg, fixed));
    CHECK(test::read_file(out) == test::read_file(tmp + "/e-ref.ffpkg"));
}

TEST_CASE(resume_image_to_folder) {
    std::string tmp = test::make_temp_dir();
    std::string game = make_game(tmp);
    std::string image = tmp + "/game.exfat";
    REQUIRE_OK(convert::run(game, image, convert::Format::exfat, base_options()));
    std::string out = tmp + "/out/";
    std::vector<resume::State> saved;
    resume::Session session;
    session.interval = 256 * 1024;
    session.save = [&](const resume::State& st) {
        saved.push_back(st);
        return Status{};
    };
    convert::Options o = base_options();
    o.resume = &session;
    CHECK(!convert::run(image, out, convert::Format::folder, o, [&](const ProgressEvent&) { return saved.size() < 2; }).ok());
    CHECK(!saved.empty());
    if (saved.empty()) return;
    CHECK(resume::get(saved.back(), "files") >= 1 || resume::get(saved.back(), "bytes") > 0);
    // A file written after the checkpoint is cut short, as by a crash.
    test::write_file(out + "data/c.bin", {1});
    session.from = saved.back();
    std::uint64_t first_done = ~std::uint64_t{0};
    REQUIRE_OK(convert::run(image, out, convert::Format::folder, o, [&](const ProgressEvent& e) {
        if (first_done == ~std::uint64_t{0} && e.total) first_done = e.done;
        return true;
    }));
    CHECK(first_done != ~std::uint64_t{0} && first_done > session.interval); // the kept files were not written again
    for (const char* f : {"eboot.bin", "sce_sys/param.json", "data/a.bin", "data/b.bin", "data/c.bin"})
        CHECK(test::read_file(out + f) == test::read_file(game + "/" + f));
}

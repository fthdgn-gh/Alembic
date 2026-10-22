// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.hpp"

#include "alembic/io.hpp"

#include <cstdlib>
#include <cstring>
#include <random>

namespace test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

int& failures() {
    static int count = 0;
    return count;
}

void fail(const char* file, int line, const std::string& message) {
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, message.c_str());
    ++failures();
}

// Folders made by make_temp_dir(), removed when the run ends.
static std::vector<std::string>& temp_dirs() {
    static std::vector<std::string> dirs;
    return dirs;
}

std::string make_temp_dir() {
#ifdef _WIN32
    std::string base = alembic::portable_path(alembic::environment("TEMP"));
    if (base.empty()) base = "C:/Windows/Temp";
#else
    std::string base = alembic::environment("TMPDIR");
    if (base.empty()) base = "/tmp";
#endif
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    static std::mt19937_64 rng(std::random_device{}());
    for (int attempt = 0; attempt < 100; ++attempt) {
        char name[32];
        std::snprintf(name, sizeof name, "alembic-test-%012llx", static_cast<unsigned long long>(rng() & 0xffffffffffffULL));
        std::string dir = base + "/" + name;
        if (alembic::make_directory(dir).ok()) {
            temp_dirs().push_back(dir);
            return dir;
        }
    }
    std::fprintf(stderr, "cannot create a temporary folder in %s\n", base.c_str());
    std::abort();
}

void make_dir(const std::string& path) {
    auto made = alembic::make_directory(path);
    if (!made.ok()) {
        std::fprintf(stderr, "%s\n", made.error().message.c_str());
        std::abort();
    }
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& data) {
    if (alembic::path_exists(path)) (void)alembic::remove_file(path);
    auto f = alembic::File::open(path, alembic::File::Mode::create_new);
    if (!f.ok() || !f->write_at(0, data).ok()) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::abort();
    }
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::vector<std::uint8_t> data;
    auto f = alembic::File::open(path, alembic::File::Mode::read);
    if (!f.ok()) return data;
    auto size = f->size();
    if (!size.ok()) return data;
    data.resize(static_cast<std::size_t>(*size));
    if (!f->read_at(0, data).ok()) data.clear();
    return data;
}

std::string source_dir() {
    std::string dir = alembic::portable_path(alembic::environment("ALEMBIC_SOURCE_DIR"));
#ifdef ALEMBIC_SOURCE_DIR
    if (dir.empty()) dir = ALEMBIC_SOURCE_DIR;
#endif
    return dir;
}

// Deletes a folder and everything in it; links are removed, not followed.
static void remove_all(const std::string& path) {
    alembic::PathStat st = alembic::stat_path(path, false);
    if (st.is_directory()) {
        if (auto names = alembic::list_directory(path); names.ok())
            for (const std::string& n : *names) remove_all(path + "/" + n);
        (void)alembic::remove_directory(path);
    } else if (st.kind != alembic::PathKind::missing) {
        if (!alembic::remove_file(path).ok()) (void)alembic::remove_directory(path); // a Windows folder link
    }
}

} // namespace test

int main(int argc, char** argv) {
    alembic::use_utf8_console();
    bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    if (verbose) {
        --argc;
        ++argv;
    }
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const test::Case& c : test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = test::failures();
        c.fn();
        ++run;
        if (verbose || test::failures() != before)
            std::fprintf(stderr, "%s %s\n", test::failures() == before ? "ok  " : "FAIL", c.name);
    }
    std::fprintf(stderr, "%d tests, %d failed checks\n", run, test::failures());
    // ALEMBIC_KEEP_TEST_DIRS=1 keeps them for a look after a failure.
    if (!std::getenv("ALEMBIC_KEEP_TEST_DIRS"))
        for (const std::string& dir : test::temp_dirs())
            test::remove_all(dir);
    return test::failures() == 0 ? 0 : 1;
}

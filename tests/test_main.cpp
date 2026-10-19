// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.hpp"

#include <cstdlib>
#include <cstring>
#include <ftw.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    const char* base = std::getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/alembic-test-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (!::mkdtemp(buffer.data())) {
        std::perror("mkdtemp");
        std::abort();
    }
    temp_dirs().push_back(buffer.data());
    return buffer.data();
}

void make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0) {
        std::perror(path.c_str());
        std::abort();
    }
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f || (data.size() && std::fwrite(data.data(), 1, data.size(), f) != data.size())) {
        std::perror(path.c_str());
        std::abort();
    }
    std::fclose(f);
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::vector<std::uint8_t> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return data;
    std::uint8_t buffer[65536];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) data.insert(data.end(), buffer, buffer + n);
    std::fclose(f);
    return data;
}

} // namespace test

int main(int argc, char** argv) {
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
            ::nftw(dir.c_str(), [](const char* path, const struct stat*, int, struct FTW*) { return ::remove(path); }, 16,
                   FTW_DEPTH | FTW_PHYS);
    return test::failures() == 0 ? 0 : 1;
}

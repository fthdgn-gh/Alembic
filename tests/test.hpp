// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny test harness. No exceptions, so failed checks just record and continue.
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace test {

using Fn = void (*)();
struct Case {
    const char* name;
    Fn fn;
};
std::vector<Case>& registry();
int& failures();

struct Register {
    Register(const char* name, Fn fn) { registry().push_back({name, fn}); }
};

void fail(const char* file, int line, const std::string& message);

// Creates a fresh empty directory under the system temp folder.
std::string make_temp_dir();
void write_file(const std::string& path, const std::vector<std::uint8_t>& data);
void make_dir(const std::string& path);
std::vector<std::uint8_t> read_file(const std::string& path);

} // namespace test

#define TEST_CASE(name)                                                         \
    static void name();                                                         \
    static test::Register name##_registration(#name, name);                     \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");        \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        auto test_a_ = (a);                                                     \
        auto test_b_ = (b);                                                     \
        if (!(test_a_ == test_b_))                                              \
            test::fail(__FILE__, __LINE__,                                      \
                       "CHECK_EQ(" #a ", " #b "): " + std::to_string(test_a_) + \
                           " != " + std::to_string(test_b_));                   \
    } while (0)

// Checks a Status or Result is ok, printing the error otherwise. Returns from
// the test on failure so later code can rely on the value.
#define REQUIRE_OK(expr)                                                        \
    do {                                                                        \
        const auto& test_r_ = (expr);                                           \
        if (!test_r_.ok()) {                                                    \
            test::fail(__FILE__, __LINE__, "REQUIRE_OK(" #expr "): " +          \
                                               test_r_.error().message);        \
            return;                                                             \
        }                                                                       \
    } while (0)

// SPDX-License-Identifier: GPL-3.0-or-later
// A small TOML reader for LZ4 pack profiles: the subset ampr_pack.py and
// ampr_pack_profile.py write, and that hand-written profiles use. Tables,
// dotted table names and keys, arrays of tables, basic and literal strings,
// integers, floats, booleans, (multi-line) arrays and inline tables.
// Multi-line strings and dates are refused. Duplicate keys are errors, as in
// TOML (and Python's tomllib, which ampr_pack.py uses).
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alembic::toml {

struct Value {
    enum class Type { table, array, string, integer, floating, boolean };
    Type type = Type::table;
    std::string string;
    std::int64_t integer = 0;
    double floating = 0;
    bool boolean = false;
    std::vector<Value> items;                          // array
    std::vector<std::pair<std::string, Value>> fields; // table, in document order
    bool table_array = false;                          // array made by [[name]]

    bool is_table() const { return type == Type::table; }
    // The field `key` of a table, or nullptr.
    const Value* get(std::string_view key) const;
};

// Parses a whole document into its root table.
Result<Value> parse(std::string_view text);

} // namespace alembic::toml

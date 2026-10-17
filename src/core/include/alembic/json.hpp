// SPDX-License-Identifier: GPL-3.0-or-later
// A small, strict JSON reader: just enough to pull top-level string fields
// out of sce_sys/param.json. It accepts what .NET's JsonDocument accepts with
// default options (RFC 8259, no comments, no trailing commas, depth <= 64).
#pragma once

#include "alembic/result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::json {

struct Property {
    std::string name;                 // decoded, UTF-8
    std::optional<std::string> value; // set only when the value is a string
};

// Parses a whole document. Fails if it is not valid JSON. Returns the
// properties of the root object in document order (duplicates kept), or an
// empty list when the root is not an object.
Result<std::vector<Property>> top_level_properties(std::string_view document);

} // namespace alembic::json

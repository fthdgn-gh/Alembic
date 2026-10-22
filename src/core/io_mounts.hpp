// SPDX-License-Identifier: GPL-3.0-or-later
// The mount table behind alembic::mount (io.hpp), for io.cpp and io_win32.cpp.
#pragma once

#include "alembic/io.hpp"

#include <memory>
#include <string>

namespace alembic::detail {

// The FileSystem for `path`, or null when it is a path on this machine.
std::shared_ptr<FileSystem> mounted(const std::string& path);

} // namespace alembic::detail

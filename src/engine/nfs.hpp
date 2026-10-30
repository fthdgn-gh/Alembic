// SPDX-License-Identifier: GPL-3.0-or-later
// NFS exports (netfs.hpp): an NFSv3 client (RFC 1813) over ONC RPC on TCP
// (RFC 5531), written from the RFCs. The portmapper (RFC 1833, port 111)
// says where the MOUNT service is; without one (rclone, go-nfs) MOUNT is
// asked on the NFS port. AUTH_SYS with the uid:gid of the server's
// settings, from a source port below 1024 when this system allows it
// (exports marked "secure", the Linux default, refuse others). Reads and
// writes are pipelined; writes are UNSTABLE, then one COMMIT at the end.
//
// Without an export in the settings, the server's exports (MOUNT EXPORT)
// are folders: "/volume1/games" is reached as volume1 › games.
#pragma once

#include "netfs.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alembic::engine::nfs {

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server);

// For tests.
namespace detail {
// The names one level below `directory` in a set of export paths (a
// listing of the exports' tree above where the exports are).
std::vector<std::string> export_children(const std::vector<std::string>& exports, const std::string& directory);
// The export a path is in (the longest one it starts with), or "".
std::string export_of(const std::vector<std::string>& exports, const std::string& path);
} // namespace detail

} // namespace alembic::engine::nfs

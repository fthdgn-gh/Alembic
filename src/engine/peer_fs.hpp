// SPDX-License-Identifier: GPL-3.0-or-later
// Paired engines' files as a mounted file system (io.hpp): "/peer/<id>/<path
// on that engine>" ("/peer/<id>/C:/Games/x" for a Windows one). Every call is
// a signed request to that engine's /api/peer/fs/* (main.cpp serves them,
// inside its shared folders only). Files read 4 MiB at a time with the next
// piece fetched as the reader moves on; writes next to each other go out
// together, up to 4 MiB a request.
#pragma once

#include "pairing.hpp"
#include "peers.hpp"

#include "alembic/io.hpp"

#include <memory>
#include <optional>
#include <string>

namespace alembic::engine::peer_fs {

inline constexpr const char* kPrefix = "/peer/";
inline constexpr std::size_t kChunk = 4u << 20; // bytes per read or write request

// "/peer/<id>/<rest>" -> id and the path on that engine ("" for "/peer/<id>").
struct Target {
    std::string id;
    std::string path;
};
std::optional<Target> parse(const std::string& path);
// The path on this engine as another engine names it ("/data/x" -> "/peer/<id>/data/x").
std::string name_for(const std::string& id, const std::string& local_path);

std::shared_ptr<FileSystem> make(peers::Discovery& discovery, pairing::Pairing& pairing);

// A signed request to a paired engine (also for the job and browse calls).
Result<std::string> request(peers::Discovery& discovery, pairing::Pairing& pairing, const std::string& id,
                            const std::string& method, const std::string& path, const std::map<std::string, std::string>& query,
                            const std::string& body = "", int timeout_ms = 30000, std::size_t max_reply = kChunk + 65536);

std::string url_encode(const std::string& s);
// "file", "directory", "link", "other", "missing" (the wire names).
const char* kind_name(PathKind k);

} // namespace alembic::engine::peer_fs

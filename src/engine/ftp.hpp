// SPDX-License-Identifier: GPL-3.0-or-later
// FTP servers as places for inputs and outputs: plain FTP (RFC 959) with
// passive data connections (EPSV, else PASV), binary type, REST for reading
// from an offset, and STOR uploads under a temporary name that is renamed
// when the file is complete. No TLS: passwords cross the network as text.
//
// Paths: "/ftp/<server id>/<path on the server>". Inputs read through a
// cache with one running download, read ahead on a thread of its own; an
// output is one upload, front to back, so only formats written strictly in
// order can go straight to a server.
#pragma once

#include "json_value.hpp"

#include "alembic/convert.hpp"
#include "alembic/io.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alembic::engine::ftp {

struct Server {
    std::string id;   // short, made from the host and user
    std::string name; // shown in the page
    std::string host;
    std::uint16_t port = 21;
    std::string user = "anonymous";
    std::string password;
};

struct Entry {
    std::string name;
    bool is_directory = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0; // Unix seconds, 0 when unknown
};

// One control connection (and at most one data connection at a time).
class Connection {
public:
    static Result<std::unique_ptr<Connection>> open(const Server& server, int timeout_ms = 10000);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Result<std::uint64_t> size(const std::string& path);
    Result<std::vector<Entry>> list(const std::string& directory);
    Status make_directory(const std::string& path);
    Status rename(const std::string& from, const std::string& to);
    Status remove(const std::string& path);

    // A download from `offset` (REST + RETR); read() returns 0 at the end.
    Status start_download(const std::string& path, std::uint64_t offset);
    Result<std::size_t> read(MutableBytes out);
    Status stop_download(); // early: the data connection is dropped and the reply drained
    // An upload (STOR); finish_upload() waits for the server's confirmation.
    Status start_upload(const std::string& path);
    Status write(Bytes data);
    Status finish_upload();

private:
    Connection() = default;
    struct Reply {
        int code = 0;
        std::string text;
    };
    Result<Reply> reply();
    Result<Reply> command(const std::string& line);
    Status open_data(); // EPSV, else PASV
    std::intptr_t control_ = -1, data_ = -1; // net::Socket
    std::string peer_; // the server's address, for passive connections
    std::string buffer_;
    bool data_ended_ = false;
};

// The servers the engine knows (kept in <state>/ftp.json) and files on them.
class Servers final {
public:
    explicit Servers(std::string state_dir);
    ~Servers();

    std::vector<Server> list() const;
    // Logs in once to check, then saves; returns the server with its id.
    Result<Server> add(Server server);
    // Changes a server's name, address, port and login (logs in first). It
    // keeps its id, so paths to it (queued jobs, remembered folders) still
    // work; `keep_password` keeps the saved password.
    Result<Server> update(const std::string& id, Server server, bool keep_password);
    Status remove(const std::string& id);

    static bool is_ftp(const std::string& path);
    // The file behind an FTP path (null when it is not one, or not there).
    std::shared_ptr<const RandomAccess> open(const std::string& path);
    Result<std::vector<Entry>> list_directory(const std::string& path);
    Status make_directory(const std::string& path);
    bool exists(const std::string& path);
    // A new file, uploaded under "<name>.alembic-part" and renamed by commit().
    Result<std::unique_ptr<convert::OutputStream>> create(const std::string& path, bool overwrite);

    struct File;

private:
    Result<std::pair<Server, std::string>> split(const std::string& path) const; // server, path on it
    Status save() const;
    std::string file_;
    mutable std::mutex mutex_;
    std::vector<Server> servers_;
    // Open files, so that several readers of one path share its cache.
    std::map<std::string, std::weak_ptr<File>> open_;
};

} // namespace alembic::engine::ftp

// SPDX-License-Identifier: GPL-3.0-or-later
// Network servers as places for inputs and outputs: FTP (ftp.hpp), SMB
// shares (smb.hpp) and WebDAV servers (webdav.hpp). Each protocol is a
// Session (log in, list, read from an offset, upload front to back); the
// server list, the read-ahead cache for inputs and the uploads under a
// temporary name are shared by all of them.
//
// Paths: "/<prefix>/<server id>/<path on the server>", the prefix being
// "ftp", "smb" or "dav". Inputs read through a cache with one running
// download, read ahead on a thread of its own; an output is one upload,
// front to back, so only formats written strictly in order can go straight
// to a server.
#pragma once

#include "alembic/convert.hpp"
#include "alembic/io.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alembic::engine::netfs {

enum class Protocol { ftp, smb, webdav };

const char* protocol_name(Protocol p);                // "ftp", "smb", "webdav" (the API's names)
const char* protocol_label(Protocol p);               // "FTP", "SMB", "WebDAV"
const char* path_prefix(Protocol p);                  // "/ftp", "/smb", "/dav"
std::uint16_t default_port(Protocol p);               // 21, 445, 80
std::optional<Protocol> parse_protocol(const std::string& name);

struct Server {
    std::string id;   // short, made from the host, user and share
    std::string name; // shown in the page
    Protocol protocol = Protocol::ftp;
    std::string host;
    std::uint16_t port = 0; // 0: the protocol's own
    std::string user;
    std::string password;
    // SMB: the share, optionally with a folder in it ("Games" or
    // "Games/PS5"). WebDAV: the server's path for the top folder ("/dav").
    // FTP: unused.
    std::string root;

    std::string path() const; // "/ftp/<id>"
};

struct Entry {
    std::string name;
    bool is_directory = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0; // Unix seconds, 0 when unknown
};

// One logged-in connection to a server. Paths are absolute on the server
// ("/" is the top), with '/' between names. At most one download or upload
// runs at a time.
class Session {
public:
    virtual ~Session() = default;
    // The size of a file; folders may fail or give 0.
    virtual Result<std::uint64_t> size(const std::string& path) = 0;
    virtual Result<std::vector<Entry>> list(const std::string& directory) = 0;
    virtual Status make_directory(const std::string& path) = 0;
    virtual Status rename(const std::string& from, const std::string& to) = 0;
    virtual Status remove(const std::string& path) = 0;

    // A download from `offset`; read() returns 0 at the end.
    virtual Status start_download(const std::string& path, std::uint64_t offset) = 0;
    virtual Result<std::size_t> read(MutableBytes out) = 0;
    // Ends a download early. A failure means the connection is out of step
    // and must not be used again.
    virtual Status stop_download() = 0;
    // An upload that replaces the file; finish_upload() waits until the
    // server has all of it.
    virtual Status start_upload(const std::string& path) = 0;
    virtual Status write(Bytes data) = 0;
    virtual Status finish_upload() = 0;
};

// Connects and logs in.
Result<std::unique_ptr<Session>> connect(const Server& server);

// The servers the engine knows (kept in <state>/servers.json, earlier
// <state>/ftp.json) and files on them.
class Servers final {
public:
    explicit Servers(std::string state_dir);
    ~Servers();

    std::vector<Server> list() const;
    // Logs in once to check, then saves; returns the server with its id.
    Result<Server> add(Server server);
    // Changes a server's name, address, port, login and share (logs in
    // first). It keeps its id and protocol, so paths to it (queued jobs,
    // remembered folders) still work; `keep_password` keeps the saved one.
    Result<Server> update(const std::string& id, Server server, bool keep_password);
    Status remove(const std::string& id);

    // Whether a path is on a network server (by its prefix only).
    static bool handles(const std::string& path);
    // The server a path is on, by its prefix and id.
    std::optional<Server> server_of(const std::string& path) const;
    // The file behind a path (null when it is not one, or not there).
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
    std::string file_, legacy_file_;
    mutable std::mutex mutex_;
    std::vector<Server> servers_;
    // Open files, so that several readers of one path share its cache.
    std::map<std::string, std::weak_ptr<File>> open_;
};

// Cleans what a user typed: an address given as a URL ("http://nas:5005/dav",
// "smb://pc/Games") fills the port and root; the default port when none.
// Fails for what cannot work (HTTPS, an SMB server without a share).
Status normalise(Server& server);

// For the protocols: a TCP connection with time-outs on connecting and on
// each send and receive (`peer` gets the address it reached), and sending
// or receiving all of a buffer. Errors start with `label` ("SMB: ...").
Result<std::intptr_t> tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms, const char* label,
                                  std::string* peer = nullptr);
Status send_all(std::intptr_t socket, const void* data, std::size_t size, const char* label);
Status recv_all(std::intptr_t socket, void* data, std::size_t size, const char* label);

} // namespace alembic::engine::netfs

// SPDX-License-Identifier: GPL-3.0-or-later
// FTP servers (netfs.hpp): plain FTP (RFC 959) with passive data connections
// (EPSV, else PASV), binary type, MLSD (else LIST) and REST for reading from
// an offset. No TLS: passwords cross the network as text.
#pragma once

#include "netfs.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alembic::engine::ftp {

using netfs::Entry;

// One control connection (and at most one data connection at a time).
class Connection final : public netfs::Session {
public:
    static Result<std::unique_ptr<Connection>> open(const netfs::Server& server, int timeout_ms = 10000);
    ~Connection() override;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Result<std::uint64_t> size(const std::string& path) override;
    Result<std::vector<Entry>> list(const std::string& directory) override;
    Status make_directory(const std::string& path) override;
    Status rename(const std::string& from, const std::string& to) override;
    Status remove(const std::string& path) override;

    // REST + RETR. stop_download() drops the data connection and drains the reply.
    Status start_download(const std::string& path, std::uint64_t offset) override;
    Result<std::size_t> read(MutableBytes out) override;
    Status stop_download() override;
    // STOR; finish_upload() waits for the server's confirmation.
    Status start_upload(const std::string& path) override;
    Status write(Bytes data) override;
    Status finish_upload() override;

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

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server);

} // namespace alembic::engine::ftp

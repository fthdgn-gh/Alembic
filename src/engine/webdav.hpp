// SPDX-License-Identifier: GPL-3.0-or-later
// WebDAV servers (netfs.hpp), RFC 4918 over plain HTTP/1.1: PROPFIND to
// list, GET with a Range to read from an offset, PUT with chunked transfer
// coding to upload, MKCOL, MOVE and DELETE. Basic and Digest (RFC 7616,
// MD5) logins. No HTTPS: passwords and files cross the network unencrypted
// (Basic sends the password as text).
#pragma once

#include "netfs.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alembic::engine::webdav {

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server);

// For tests.
namespace detail {
struct Item {
    std::string href; // decoded, without scheme and host
    netfs::Entry entry;
};
// The responses of a PROPFIND multistatus body.
std::vector<Item> parse_multistatus(const std::string& xml);
std::string encode_path(const std::string& path); // percent-encodes all but unreserved bytes and '/'
std::string decode(const std::string& text);      // %XX
std::int64_t parse_http_date(const std::string& text); // RFC 1123; 0 when unknown
} // namespace detail

} // namespace alembic::engine::webdav

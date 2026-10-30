// SPDX-License-Identifier: GPL-3.0-or-later
// SMB shares (netfs.hpp): an SMB2/SMB3 client written from Microsoft's open
// specifications [MS-SMB2] and [MS-NLMP]. Dialects 2.0.2 to 3.1.1 over
// direct TCP (port 445); NTLMv2 logins in SPNEGO (or a guest login with no
// user); every request signed once logged in (HMAC-SHA256, or AES-CMAC from
// 3.0), as Windows 11 and current NAS systems require. Reads and writes are
// pipelined, several requests in flight. Not supported: encryption (shares
// that require it are refused with a message), Kerberos, DFS referrals and
// listing a server's shares (the share is given with the server).
#pragma once

#include "netcrypto.hpp"
#include "netfs.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alembic::engine::smb {

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server);

// The NTLMv2 and key steps, for tests.
namespace detail {
// NTOWFv2: HMAC-MD5 of the upper-case user and the domain under MD4 of the password.
netcrypto::Md ntowfv2(const std::string& password, const std::string& user, const std::string& domain);
struct Ntlmv2 {
    std::vector<std::uint8_t> nt_response; // NTProofStr followed by the client blob
    netcrypto::Md session_base_key;
};
Ntlmv2 ntlmv2_response(const netcrypto::Md& ntowf, const std::uint8_t server_challenge[8], const std::uint8_t client_challenge[8],
                       std::uint64_t timestamp, Bytes target_info);
// SP800-108 in counter mode with HMAC-SHA256, 128 bits out ([MS-SMB2] 3.1.4.2);
// label and context as SMB gives them (with their final NUL where it has one).
netcrypto::Md kdf(const std::uint8_t key[16], Bytes label, Bytes context);
void rc4(Bytes key, MutableBytes data);
} // namespace detail

} // namespace alembic::engine::smb

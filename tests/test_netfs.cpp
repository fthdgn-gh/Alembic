// SPDX-License-Identifier: GPL-3.0-or-later
// Network servers: the hashes and NTLMv2 steps SMB logins use (published
// test vectors), WebDAV's PROPFIND parsing and what users type as addresses.
#include "netcrypto.hpp"
#include "netfs.hpp"
#include "nfs.hpp"
#include "smb.hpp"
#include "test.hpp"
#include "webdav.hpp"

#include <string>
#include <vector>

using namespace alembic;
using namespace alembic::engine;

namespace {

std::vector<std::uint8_t> unhex(const std::string& text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return out;
}
Bytes bytes_of(const std::string& s) { return Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
Bytes bytes_of(const std::vector<std::uint8_t>& v) { return Bytes(v.data(), v.size()); }
template <class T> std::string hex_of(const T& d) { return netcrypto::hex(Bytes(d.data(), d.size())); }

} // namespace

TEST_CASE(netcrypto_hashes_match_their_standards) {
    CHECK(hex_of(netcrypto::md4(bytes_of(""))) == "31d6cfe0d16ae931b73c59d7e0c089c0");    // RFC 1320
    CHECK(hex_of(netcrypto::md4(bytes_of("abc"))) == "a448017aaf21d8525fc10ae87aa6729d");
    CHECK(hex_of(netcrypto::md5(bytes_of(""))) == "d41d8cd98f00b204e9800998ecf8427e");    // RFC 1321
    CHECK(hex_of(netcrypto::md5(bytes_of("abc"))) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(hex_of(netcrypto::md5(bytes_of("12345678901234567890123456789012345678901234567890123456789012345678901234567890"))) ==
          "57edf4a22be3c955ac49da2e2107b67a");
    auto key = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"); // RFC 2104
    CHECK(hex_of(netcrypto::hmac_md5(bytes_of(key), bytes_of("Hi There"))) == "9294727a3638bb1c13f48ef8158bfc9d");
    CHECK(hex_of(netcrypto::sha512(bytes_of("abc"))) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(hex_of(netcrypto::sha512(bytes_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))) ==
          "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    // RFC 4493 AES-CMAC.
    auto k = unhex("2b7e151628aed2a6abf7158809cf4f3c");
    auto m = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    CHECK(hex_of(netcrypto::aes_cmac(k.data(), Bytes(m.data(), 0))) == "bb1d6929e95937287fa37d129b756746");
    CHECK(hex_of(netcrypto::aes_cmac(k.data(), Bytes(m.data(), 16))) == "070a16b46b4d4144f79bdd9dd04a287c");
    CHECK(hex_of(netcrypto::aes_cmac(k.data(), Bytes(m.data(), 40))) == "dfa66747de9ae63030ca32611497c827");
    CHECK(hex_of(netcrypto::aes_cmac(k.data(), Bytes(m.data(), 64))) == "51f0bebf7e3b9d92fc49741779363cfe");
}

TEST_CASE(smb_ntlmv2_matches_the_ms_nlmp_example) {
    // [MS-NLMP] 4.2.4: user "User", domain "Domain", password "Password".
    auto ntowf = smb::detail::ntowfv2("Password", "User", "Domain");
    CHECK(hex_of(ntowf) == "0c868a403bfd7a93a3001ef22ef02e3f");
    auto server = unhex("0123456789abcdef"), client = unhex("aaaaaaaaaaaaaaaa");
    auto info = unhex("02000c0044006f006d00610069006e0001000c005300650072007600650072000000000000000000");
    info.resize(info.size() - 4); // the example's AV list ends with MsvAvEOL; the 4 zeros after it are the blob's
    auto r = smb::detail::ntlmv2_response(ntowf, server.data(), client.data(), 0, bytes_of(info));
    CHECK(netcrypto::hex(Bytes(r.nt_response.data(), 16)) == "68cd0ab851e51c96aabc927bebef6a1c");
    CHECK(hex_of(r.session_base_key) == "8de40ccadbc14a82f15cb0ad0de95ca3");
    // RC4 (the session key exchange): a known vector.
    auto data = unhex("0000000000000000");
    smb::detail::rc4(bytes_of(unhex("0123456789abcdef")), MutableBytes(data.data(), data.size()));
    CHECK(netcrypto::hex(bytes_of(data)) == "7494c2e7104b0879");
}

TEST_CASE(webdav_reads_multistatus_replies) {
    const std::string xml = R"(<?xml version="1.0" encoding="utf-8" ?>
<D:multistatus xmlns:D="DAV:">
<D:response><D:href>/dav/Games/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype>
<D:getlastmodified>Tue, 27 Oct 2026 10:15:00 GMT</D:getlastmodified></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
<D:response><D:href>http://nas:5005/dav/Games/A%20game%20%26%20more.ffpfsc</D:href><D:propstat><D:prop><D:resourcetype/>
<D:getcontentlength>159723618304</D:getcontentlength><D:getlastmodified>Wed, 28 Oct 2026 01:02:03 GMT</D:getlastmodified></D:prop>
<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
<response xmlns="DAV:"><href>/dav/Games/sub%C3%A9/</href><propstat><prop><resourcetype><collection/></resourcetype></prop></propstat></response>
</D:multistatus>)";
    auto items = webdav::detail::parse_multistatus(xml);
    CHECK_EQ(items.size(), 3u);
    if (items.size() != 3) return;
    CHECK(items[0].href == "/dav/Games/");
    CHECK(items[0].entry.is_directory);
    CHECK(items[0].entry.name == "Games");
    CHECK(items[1].entry.name == "A game & more.ffpfsc");
    CHECK(!items[1].entry.is_directory);
    CHECK_EQ(items[1].entry.size, 159723618304ull);
    CHECK_EQ(items[1].entry.mtime, 1793149323);
    CHECK(items[2].entry.is_directory);
    CHECK(items[2].entry.name == "sub\xC3\xA9");
    CHECK(webdav::detail::encode_path("/a b/é#?.pkg") == "/a%20b/%C3%A9%23%3F.pkg");
    CHECK_EQ(webdav::detail::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"), 784111777);
}

TEST_CASE(netfs_addresses_as_users_type_them) {
    netfs::Server s;
    s.protocol = netfs::Protocol::webdav;
    s.host = " http://nas.local:5005/dav/ ";
    REQUIRE_OK(netfs::normalise(s));
    CHECK(s.host == "nas.local");
    CHECK_EQ(s.port, 5005);
    CHECK(s.root == "/dav");

    netfs::Server w;
    w.protocol = netfs::Protocol::smb;
    w.host = "\\\\PC\\Games\\PS5";
    REQUIRE_OK(netfs::normalise(w));
    CHECK(w.host == "PC");
    CHECK(w.root == "Games/PS5");
    CHECK_EQ(w.port, 445);

    netfs::Server noshare; // the whole server: its shares are the top folders
    noshare.protocol = netfs::Protocol::smb;
    noshare.host = "192.168.1.10";
    REQUIRE_OK(netfs::normalise(noshare));
    CHECK(noshare.root.empty());

    netfs::Server tls;
    tls.protocol = netfs::Protocol::webdav;
    tls.host = "https://cloud.example/remote.php/dav";
    CHECK(!netfs::normalise(tls).ok());

    netfs::Server f;
    f.host = "10.0.0.2";
    REQUIRE_OK(netfs::normalise(f));
    CHECK(f.user == "anonymous");
    CHECK_EQ(f.port, 21);
    CHECK(netfs::Servers::handles("/smb/pc-me-Games/x.pkg"));
    CHECK(netfs::Servers::handles("/dav/nas/x"));
    CHECK(!netfs::Servers::handles("/smb"));
    CHECK(!netfs::Servers::handles("/data/x"));
}

TEST_CASE(nfs_exports_are_a_tree_of_folders) {
    const std::vector<std::string> exports = {"/volume1/games", "/volume1/media", "/srv/nfs"};
    auto top = nfs::detail::export_children(exports, "/");
    CHECK_EQ(top.size(), 2u);
    CHECK(top[0] == "srv" && top[1] == "volume1");
    auto v1 = nfs::detail::export_children(exports, "/volume1");
    CHECK(v1.size() == 2 && v1[0] == "games" && v1[1] == "media");
    CHECK(nfs::detail::export_children(exports, "/volume1/games").empty());
    CHECK(nfs::detail::export_of(exports, "/volume1/games/PS5/a.pkg") == "/volume1/games");
    CHECK(nfs::detail::export_of(exports, "/volume1/games") == "/volume1/games");
    CHECK(nfs::detail::export_of(exports, "/volume1").empty());
    CHECK(nfs::detail::export_of({"/"}, "/a/b") == "/");
    CHECK(nfs::detail::export_of({"/data", "/data/games"}, "/data/games/x") == "/data/games");

    netfs::Server n;
    n.protocol = netfs::Protocol::nfs;
    n.host = "nas:/volume1/games/";
    n.user = "1026";
    REQUIRE_OK(netfs::normalise(n));
    CHECK(n.host == "nas" && n.root == "/volume1/games" && n.user == "1026:1026" && n.port == 2049);
    netfs::Server bad;
    bad.protocol = netfs::Protocol::nfs;
    bad.host = "nas";
    bad.user = "admin";
    CHECK(!netfs::normalise(bad).ok());
}

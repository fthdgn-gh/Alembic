// SPDX-License-Identifier: GPL-3.0-or-later
// Network servers: the hashes and NTLMv2 steps SMB logins use (published
// test vectors), WebDAV's PROPFIND parsing and what users type as addresses.
#include "netcrypto.hpp"
#include "netfs.hpp"
#include "nfs.hpp"
#include "smb.hpp"
#include "tls.hpp"
#include "test.hpp"
#include "webdav.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

TEST_CASE(netcrypto_ciphers_match_their_standards) {
    // AES-GCM: the GCM specification's test cases 2 and 4 (McGrew and Viega).
    {
        std::vector<std::uint8_t> key(16, 0), nonce(12, 0), data(16, 0);
        std::uint8_t tag[16];
        netcrypto::aes_gcm_encrypt(key.data(), nonce.data(), Bytes(), MutableBytes(data.data(), data.size()), tag);
        CHECK(hex_of(data) == std::string("0388dace60b6a392f328c2b971b2fe78"));
        CHECK(netcrypto::hex(Bytes(tag, 16)) == std::string("ab6e47d42cec13bdf53a67b21257bddf"));
    }
    const auto key = unhex("feffe9928665731c6d6a8f9467308308");
    {
        const auto nonce = unhex("cafebabefacedbaddecaf888");
        const auto aad = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        const auto plain = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e24"
                                 "49a6b525b16aedf5aa0de657ba637b39");
        std::vector<std::uint8_t> data = plain;
        std::uint8_t tag[16];
        netcrypto::aes_gcm_encrypt(key.data(), nonce.data(), bytes_of(aad), MutableBytes(data.data(), data.size()), tag);
        CHECK(hex_of(data) == std::string("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c"
                                           "7d8f6a5aac84aa051ba30b396a0aac973d58e091"));
        CHECK(netcrypto::hex(Bytes(tag, 16)) == std::string("5bc94fbc3221a5db94fae95ae7121a47"));
        // A changed byte is refused and the data left as it came.
        std::vector<std::uint8_t> bad = data;
        bad[5] ^= 1;
        CHECK(!netcrypto::aes_gcm_decrypt(key.data(), nonce.data(), bytes_of(aad), MutableBytes(bad.data(), bad.size()), tag));
        CHECK(bad[5] == (data[5] ^ 1));
        CHECK(netcrypto::aes_gcm_decrypt(key.data(), nonce.data(), bytes_of(aad), MutableBytes(data.data(), data.size()), tag));
        CHECK(data == plain);
    }
    // AES-CCM: RFC 3610 packet vector 1 (13-byte nonce, 8-byte tag).
    {
        std::vector<std::uint8_t> k, nonce = unhex("00000003020100a0a1a2a3a4a5"), aad, data;
        for (int i = 0xC0; i < 0xD0; ++i) k.push_back(static_cast<std::uint8_t>(i));
        for (int i = 0; i < 8; ++i) aad.push_back(static_cast<std::uint8_t>(i));
        for (int i = 8; i < 0x1F; ++i) data.push_back(static_cast<std::uint8_t>(i));
        std::uint8_t tag[8];
        netcrypto::aes_ccm_encrypt(k.data(), bytes_of(nonce), bytes_of(aad), MutableBytes(data.data(), data.size()), MutableBytes(tag, 8));
        CHECK(hex_of(data) == std::string("588c979a61c663d2f066d0c2c0f989806d5f6b61dac384"));
        CHECK(netcrypto::hex(Bytes(tag, 8)) == std::string("17e8d12cfdf926e0"));
    }
    // AES-CCM as SMB 3 uses it (11-byte nonce, 16-byte tag), checked with
    // Python's cryptography (OpenSSL) AESCCM; then the way back.
    {
        std::vector<std::uint8_t> nonce, aad, plain;
        for (int i = 1; i < 12; ++i) nonce.push_back(static_cast<std::uint8_t>(i));
        for (int i = 100; i < 132; ++i) aad.push_back(static_cast<std::uint8_t>(i));
        for (int i = 0; i < 100; ++i) plain.push_back(static_cast<std::uint8_t>(i * 7));
        std::vector<std::uint8_t> data = plain;
        std::uint8_t tag[16];
        netcrypto::aes_ccm_encrypt(key.data(), bytes_of(nonce), bytes_of(aad), MutableBytes(data.data(), data.size()), MutableBytes(tag, 16));
        CHECK(hex_of(data) + netcrypto::hex(Bytes(tag, 16)) ==
                 std::string("f0b1018318886154d5d4d3c6f7b77d5827e791b919bb7a3655adaf583d4c87aaeb5881810bba813dfedf28543691d6cd"
                             "88483e0e59358f93529a1d81bcc025ddaa28bddb84e9317f11f55c21f06367e3a23d5d01715321ff3b2df4cd4adb6dff"
                             "a6a3822ea915af378b61b4f1d440271b049928d5"));
        std::uint8_t bad_tag[16];
        std::memcpy(bad_tag, tag, 16);
        bad_tag[15] ^= 0x80;
        std::vector<std::uint8_t> copy = data;
        CHECK(!netcrypto::aes_ccm_decrypt(key.data(), bytes_of(nonce), bytes_of(aad), MutableBytes(copy.data(), copy.size()), Bytes(bad_tag, 16)));
        CHECK(copy == data);
        CHECK(netcrypto::aes_ccm_decrypt(key.data(), bytes_of(nonce), bytes_of(aad), MutableBytes(data.data(), data.size()), Bytes(tag, 16)));
        CHECK(data == plain);
    }
    // Long messages cross the keystream batches (64 blocks) and an odd tail.
    for (std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(1023), std::size_t(1024), std::size_t(70001)}) {
        std::vector<std::uint8_t> plain(n), nonce(12, 9), aad(32, 3);
        for (std::size_t i = 0; i < n; ++i) plain[i] = static_cast<std::uint8_t>(i * 31 + 7);
        std::vector<std::uint8_t> g = plain, c = plain;
        std::uint8_t tg[16], tc[16];
        netcrypto::aes_gcm_encrypt(key.data(), nonce.data(), bytes_of(aad), MutableBytes(g.data(), g.size()), tg);
        netcrypto::aes_ccm_encrypt(key.data(), Bytes(nonce.data(), 11), bytes_of(aad), MutableBytes(c.data(), c.size()), MutableBytes(tc, 16));
        CHECK(n == 0 || g != plain);
        CHECK(netcrypto::aes_gcm_decrypt(key.data(), nonce.data(), bytes_of(aad), MutableBytes(g.data(), g.size()), tg));
        CHECK(netcrypto::aes_ccm_decrypt(key.data(), Bytes(nonce.data(), 11), bytes_of(aad), MutableBytes(c.data(), c.size()), Bytes(tc, 16)));
        CHECK(g == plain && c == plain);
    }
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

    // https:// and ftps:// switch TLS on (port 443 for WebDAV); SFTP is refused.
    netfs::Server tls;
    tls.protocol = netfs::Protocol::webdav;
    tls.host = "https://cloud.example/remote.php/dav";
    REQUIRE_OK(netfs::normalise(tls));
    CHECK(tls.tls && tls.port == 443 && tls.host == "cloud.example" && tls.root == "/remote.php/dav");
    netfs::Server ftps;
    ftps.protocol = netfs::Protocol::ftp;
    ftps.host = "ftps://nas:2121";
    REQUIRE_OK(netfs::normalise(ftps));
    CHECK(ftps.tls && ftps.port == 2121 && ftps.host == "nas");
    netfs::Server sftp;
    sftp.protocol = netfs::Protocol::ftp;
    sftp.host = "sftp://nas";
    CHECK(!netfs::normalise(sftp).ok());
    // TLS means nothing for SMB and NFS.
    netfs::Server smb_tls;
    smb_tls.protocol = netfs::Protocol::smb;
    smb_tls.host = "pc";
    smb_tls.tls = true;
    smb_tls.fingerprint = "AA";
    REQUIRE_OK(netfs::normalise(smb_tls));
    CHECK(!smb_tls.tls && smb_tls.fingerprint.empty());

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

// Upload, list, download (whole and from an offset), rename and remove on a
// real server: the live tests below.
namespace {
void live_session(netfs::Session& s) {
    // 3 MiB and a bit: several 1 MiB writes and reads in flight. Files at
    // the top: unprivileged Samba on macOS cannot make folders (its mkdir
    // renames a temporary folder, which fails there).
    std::vector<std::uint8_t> data(3 * 1024 * 1024 + 12345);
    std::uint32_t x = 12345;
    for (auto& b : data) b = static_cast<std::uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
    (void)s.remove("/alembic-live-a.bin");
    (void)s.remove("/alembic-live-b.bin");
    REQUIRE_OK(s.start_upload("/alembic-live-a.bin"));
    for (std::size_t at = 0; at < data.size(); at += 300000)
        REQUIRE_OK(s.write(Bytes(data.data() + at, std::min<std::size_t>(300000, data.size() - at))));
    REQUIRE_OK(s.finish_upload());
    auto size = s.size("/alembic-live-a.bin");
    REQUIRE_OK(size);
    CHECK(*size == data.size());
    auto listing = s.list("/");
    REQUIRE_OK(listing);
    bool listed = false;
    for (const netfs::Entry& e : *listing) listed = listed || (e.name == "alembic-live-a.bin" && e.size == data.size());
    CHECK(listed);
    auto download = [&](std::uint64_t from) {
        std::vector<std::uint8_t> got;
        if (!s.start_download("/alembic-live-a.bin", from).ok()) return got;
        std::vector<std::uint8_t> buf(200000);
        for (;;) {
            auto n = s.read(MutableBytes(buf.data(), buf.size()));
            if (!n.ok() || *n == 0) break;
            got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
        }
        return got;
    };
    CHECK(download(0) == data);
    CHECK(download(1000001) == std::vector<std::uint8_t>(data.begin() + 1000001, data.end()));
    // A download stopped part way, then the connection used again.
    REQUIRE_OK(s.start_download("/alembic-live-a.bin", 0));
    std::vector<std::uint8_t> some(100000);
    CHECK(s.read(MutableBytes(some.data(), some.size())).ok());
    CHECK(s.stop_download().ok());
    REQUIRE_OK(s.rename("/alembic-live-a.bin", "/alembic-live-b.bin"));
    CHECK(!s.size("/alembic-live-a.bin").ok());
    REQUIRE_OK(s.remove("/alembic-live-b.bin"));
}

std::vector<std::string> split(std::string s, char sep, std::size_t fields) {
    std::vector<std::string> f;
    for (;;) {
        std::size_t c = s.find(sep);
        if (f.size() + 1 == fields || c == std::string::npos) {
            f.push_back(s);
            break;
        }
        f.push_back(s.substr(0, c));
        s = s.substr(c + 1);
    }
    return f;
}
} // namespace

// A real SMB server, when ALEMBIC_SMB_TEST is "host:port:share:user:password"
// (tools/netfs/samba_check.sh runs Samba that way in each configuration).
TEST_CASE(smb_live_session) {
    const char* spec = std::getenv("ALEMBIC_SMB_TEST");
    if (!spec || !*spec) {
        std::printf("  (skipped: ALEMBIC_SMB_TEST not set)\n");
        return;
    }
    std::vector<std::string> f = split(spec, ':', 5);
    CHECK(f.size() == 5);
    if (f.size() != 5) return;
    netfs::Server server;
    server.protocol = netfs::Protocol::smb;
    server.host = f[0];
    server.port = static_cast<std::uint16_t>(std::stoi(f[1]));
    server.root = f[2];
    server.user = f[3];
    server.password = f[4];
    auto session = smb::open(server);
    if (!session.ok()) std::printf("  open: %s\n", session.error().message.c_str());
    CHECK(session.ok());
    if (!session.ok()) return;
    live_session(**session);
}

// Any network server, when ALEMBIC_NETFS_TEST is
// "protocol|host|port|root|user|password|tls|fingerprint" (tls "1" or "0";
// tools/netfs/tls_check.sh). With TLS, the trust steps come first: no pin
// is refused as "not trusted yet" with the fingerprint, a wrong pin as
// "changed", and only the right pin gets through.
TEST_CASE(netfs_live_session) {
    const char* spec = std::getenv("ALEMBIC_NETFS_TEST");
    if (!spec || !*spec) {
        std::printf("  (skipped: ALEMBIC_NETFS_TEST not set)\n");
        return;
    }
    std::vector<std::string> f = split(spec, '|', 8);
    CHECK(f.size() == 8);
    if (f.size() != 8) return;
    netfs::Server server;
    auto protocol = netfs::parse_protocol(f[0]);
    CHECK(protocol.has_value());
    if (!protocol) return;
    server.protocol = *protocol;
    server.host = f[1];
    server.port = static_cast<std::uint16_t>(std::stoi(f[2]));
    server.root = f[3];
    server.user = f[4];
    server.password = f[5];
    server.tls = f[6] == "1";
    if (server.tls) {
        server.fingerprint = "";
        auto first = netfs::connect(server);
        CHECK(!first.ok());
        auto seen = first.ok() ? std::nullopt : tls::untrusted_fingerprint(first.error());
        CHECK(seen.has_value() && *seen == f[7]);
        if (!first.ok() && (!seen || *seen != f[7])) std::printf("  first: %s\n", first.error().message.c_str());
        server.fingerprint = std::string(f[7].size(), 'A');
        auto wrong = netfs::connect(server);
        CHECK(!wrong.ok() && wrong.error().message.find("changed") != std::string::npos);
        server.fingerprint = f[7];
    }
    auto session = netfs::connect(server);
    if (!session.ok()) std::printf("  open: %s\n", session.error().message.c_str());
    CHECK(session.ok());
    if (!session.ok()) return;
    live_session(**session);
}

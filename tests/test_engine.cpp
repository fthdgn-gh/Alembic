// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's JSON value, folder rules and output naming.
#include "json_value.hpp"
#include "l10n.hpp"
#include "keys.hpp"
#include "pairing.hpp"
#include "peers.hpp"
#include "runner.hpp"
#include "smp.hpp"
#include "test.hpp"

using namespace alembic;
using namespace alembic::engine;

#include <cctype>
#include <cstring>

TEST_CASE(engine_json_round_trips) {
    auto v = parse_json(R"({"a":[1,-2,3.5,true,null],"s":"x\"\u00e9\ud83d\ude00","big":159723618304,"o":{}})");
    REQUIRE_OK(v);
    CHECK_EQ((*v)["a"].items().size(), 5u);
    CHECK_EQ((*v)["a"].items()[1].integer(), -2);
    CHECK((*v)["a"].items()[2].number() == 3.5);
    CHECK((*v)["s"].str() == "x\"\xC3\xA9\xF0\x9F\x98\x80");
    CHECK_EQ((*v)["big"].integer(), 159723618304);
    CHECK((*v)["missing"].is_null());
    CHECK(v->dump() == R"({"a":[1,-2,3.5,true,null],"s":"x\"é😀","big":159723618304,"o":{}})");
    CHECK(!parse_json("{\"a\":1,}").ok());
    CHECK(!parse_json("[1] x").ok());
    CHECK(!parse_json("\"\\ud800\"").ok());
    CHECK(!parse_json("01").ok());
    CHECK(json_quote("a\x01\xFF") == "\"a\\u0001\xEF\xBF\xBD\"");
}

TEST_CASE(engine_paths_stay_inside_the_roots) {
#ifdef _WIN32
    const std::string d = "C:"; // absolute paths start with a drive there
#else
    const std::string d;
#endif
    std::vector<std::string> roots{d + "/data", d + "/mnt/usb0/"};
    CHECK(path_allowed(d + "/data", roots));
    CHECK(path_allowed(d + "/data/homebrew/x.pkg", roots));
    CHECK(path_allowed(d + "/mnt/usb0/game", roots));
    CHECK(!path_allowed(d + "/database", roots));
    CHECK(!path_allowed(d + "/data/../etc", roots));
    CHECK(!path_allowed(d + "/data/./x", roots));
    CHECK(!path_allowed("data/x", roots));
    CHECK(!path_allowed(d + "/system/x", roots));
    CHECK(!path_allowed("", roots));
#ifdef _WIN32
    CHECK(path_allowed("c:/DATA/x", roots));       // no case on Windows
    CHECK(!path_allowed("C:/data\\..\\x", roots)); // '\\' is turned into '/' before this
    CHECK(!path_allowed("/data", roots));          // drive-relative
    CHECK(path_allowed("D:/x", {"D:/"}));          // a drive root
#endif
}

TEST_CASE(engine_names_outputs_next_to_the_input) {
    JobSpec s;
    s.input = "/data/games/A.pkg";
    s.to = "ffpfsc";
    auto out = resolve_output(s);
    REQUIRE_OK(out);
    CHECK(*out == "/data/games/A.ffpfsc");
    s.lz4 = true;
    s.to = "exfat";
    CHECK(*resolve_output(s) == "/data/games/A-lz4.exfat");
    s.lz4 = false;
    s.input = "/data/games/A.exfat";
    CHECK(!resolve_output(s).ok()); // would replace the input
    s.output = "/data/other.exfat";
    CHECK(*resolve_output(s) == "/data/other.exfat");
    JobSpec other;
    other.input = "/mnt/usb0/games/A.pkg";
    other.to = "ffpfsc";
    other.output_dir = "/data/homebrew/";
    CHECK(*resolve_output(other) == "/data/homebrew/A.ffpfsc"); // another drive, default name
    other.output_name = "B.ffpfsc";
    CHECK(*resolve_output(other) == "/data/homebrew/B.ffpfsc");
    other.to = "folder";
    other.output_name = "";
    CHECK(*resolve_output(other) == "/data/homebrew/A/");
    other.output_dir = "/";
    CHECK(*resolve_output(other) == "/A/");
    JsonValue bad = JsonValue::object();
    bad.set("input", "/data/x.pkg");
    bad.set("output_name", "../x");
    CHECK(!JobSpec::from_json(bad).ok());
    // ShadowMount+'s mount point limit: 58 characters for .ffpfsc, 63 for .exfat.
    CHECK_EQ(mount_name_limit("/data/x.ffpfsc"), 58u);
    CHECK_EQ(mount_name_limit("/data/x.EXFAT"), 63u);
    CHECK_EQ(mount_name_limit("/data/x/"), 0u);
    JobSpec longname;
    longname.input = "/data/XX0000-ABCD12345_00-0123456789ABCDEFG-A0100-V0100.pkg"; // 54 characters
    longname.to = "ffpfsc";
    longname.lz4 = true;
    CHECK(*resolve_output(longname) == "/data/XX0000-ABCD12345_00-0123456789ABCDEFG-A0100-V0100-lz4.ffpfsc"); // 58: fits
    longname.input = "/data/XX0000-ABCD12345_00-0123456789ABCDEFGHIJKLMN-A0100-V0100.pkg"; // + -lz4 = 60
    CHECK(*resolve_output(longname) == "/data/ABCD12345-A0100-V0100-lz4.ffpfsc");
    longname.to = "exfat";
    CHECK(*resolve_output(longname) == "/data/XX0000-ABCD12345_00-0123456789ABCDEFGHIJKLMN-A0100-V0100-lz4.exfat"); // 60 <= 63
    CHECK(shorten_name(std::string(70, 'x') + "-lz4", 58) == std::string(54, 'x') + "-lz4");
    CHECK(shorten_name("short", 58) == "short");
    CHECK(mount_name_warning("/data/" + std::string(59, 'a') + ".ffpfsc") != "");
    CHECK(mount_name_warning("/data/" + std::string(58, 'a') + ".ffpfsc") == "");
    JobSpec none;
    none.input = "/data/x.pkg";
    CHECK(!resolve_target(none).ok()); // no output and no target
    none.output = "/data/x.ffpfsc";
    CHECK(*resolve_target(none) == "ffpfsc");
}

// The engine's translations come from the page's i18n.js.
TEST_CASE(engine_l10n_messages) {
    std::vector<std::uint8_t> js = test::read_file(test::source_dir() + "/src/engine/web/i18n.js");
    CHECK(!js.empty());
    l10n::load(reinterpret_cast<const char*>(js.data()), js.size());
    CHECK(l10n::has_language("tr") && l10n::has_language("ja") && !l10n::has_language("xx"));

    l10n::Vars v;
    CHECK(l10n::match("cannot write '{path}': {reason}", "cannot write '/mnt/usb0/a b.exfat': No space left on device", v));
    CHECK(v.size() == 2 && v[0].second == "/mnt/usb0/a b.exfat" && v[1].second == "No space left on device");
    CHECK(!l10n::match("cannot write '{path}': {reason}", "cannot read '/x': y", v));
    CHECK(!l10n::match("Copied {name}", "Copied ", v)); // a part is never empty

    l10n::set_language("en");
    CHECK(l10n::t("Converted {name}", {{"name", "A"}}) == "Converted A");
    CHECK(l10n::message("cancel the job first") == "cancel the job first");
    l10n::set_language("tr");
    CHECK(l10n::t("Converted {name}", {{"name", "A"}}) == "A dönüştürüldü");
    // The pattern, and the system reason inside it.
    CHECK(l10n::message("cannot write '/x': No space left on device") == "'/x' yazılamıyor: Aygıtta boş yer kalmadı");
    CHECK(l10n::message("the output does not match the source (differs: eboot.bin); the source was kept") ==
          "çıktı kaynakla eşleşmiyor (farklı: eboot.bin); kaynak korundu");
    CHECK(l10n::message("something nobody translated") == "something nobody translated");
    l10n::set_language("xx");
    CHECK(l10n::language() == "en");
}

TEST_CASE(shadowmount_hidden_names_and_config) {
    using namespace alembic::engine;
    CHECK(smp::hidden_path("/data/games/A.ffpfsc") == "/data/games/.A.ffpfsc");
    CHECK(smp::hidden_path("/data/games/.A.ffpfsc") == "/data/games/A.ffpfsc");
    CHECK(smp::is_hidden("/data/games/.A.ffpfsc"));
    CHECK(!smp::is_hidden("/data/games/A.ffpfsc"));

    std::string tmp = test::make_temp_dir();
    CHECK(!smp::read_endpoint(tmp + "/missing.ini").enabled);
    const char* ini = "# comment\napi_bind_address = 0.0.0.0\napi_port=12345\n";
    test::write_file(tmp + "/config.ini", std::vector<std::uint8_t>(ini, ini + std::strlen(ini)));
    smp::Endpoint e = smp::read_endpoint(tmp + "/config.ini");
    CHECK(e.enabled);
    CHECK(e.address == "127.0.0.1");
    CHECK_EQ(e.port, std::uint16_t(12345));
    const char* off = "api_enabled=0\n";
    test::write_file(tmp + "/off.ini", std::vector<std::uint8_t>(off, off + std::strlen(off)));
    CHECK(!smp::read_endpoint(tmp + "/off.ini").enabled);
    CHECK(!smp::post(smp::read_endpoint(tmp + "/off.ini"), "/api/v1/scan", JsonValue::object()).ok());
    auto parsed = smp::parse_endpoint("192.168.1.5:10101");
    CHECK(parsed.has_value() && parsed->address == "192.168.1.5" && parsed->port == 10101);
    CHECK(!smp::parse_endpoint("nonsense").has_value());
}

TEST_CASE(engine_discovery_beacons) {
    peers::Identity self;
    self.id = "0123456789abcdef0123456789abcdef";
    self.name = "Desk PC";
    self.os = "Windows 11 (build 26100, x64)";
    self.platform = "host";
    self.version = "0.1.1";
    self.port = 5940;
    auto b = peers::decode_beacon(peers::encode_beacon(self, true));
    CHECK(b.ok());
    if (!b.ok()) return;
    CHECK(b->who.id == self.id && b->who.name == self.name && b->who.os == self.os && b->who.port == 5940);
    CHECK(b->query);
    CHECK(!peers::decode_beacon(peers::encode_beacon(self, false))->query);
    // Anything else is ignored.
    CHECK(!peers::decode_beacon("hello").ok());
    CHECK(!peers::decode_beacon(R"({"alembic":"engine","v":2,"id":"0123456789abcdef0123456789abcdef","port":5940})").ok());
    CHECK(!peers::decode_beacon(R"({"alembic":"engine","v":1,"id":"not-an-id","port":5940})").ok());
    CHECK(!peers::decode_beacon(R"({"alembic":"engine","v":1,"id":"0123456789abcdef0123456789abcdef","port":70000})").ok());
    // Text from the network is cleaned: no control characters, at most 80 bytes, whole UTF-8 sequences.
    std::string long_name(78, 'a');
    long_name += "\xC3\xA9\xC3\xA9"; // 82 bytes, the cut falls inside the second é
    auto c = peers::decode_beacon(R"({"alembic":"engine","v":1,"id":"0123456789abcdef0123456789abcdef","port":1,"name":"a\u0007b","os":")" + long_name + "\"}");
    CHECK(c.ok());
    if (!c.ok()) return;
    CHECK(c->who.name == "ab");
    CHECK(c->who.os == std::string(78, 'a') + "\xC3\xA9");
    auto d = peers::decode_beacon(R"({"alembic":"engine","v":1,"id":"0123456789abcdef0123456789abcdef","port":1,"os":")" +
                                  std::string(79, 'a') + "\xC3\xA9\"}"); // the cut splits the é
    CHECK(d.ok() && d->who.os == std::string(79, 'a'));
    // The engine's id is made once and kept.
    std::string dir = test::make_temp_dir();
    std::string id = peers::engine_id(dir);
    CHECK(id.size() == 32);
    CHECK(peers::engine_id(dir) == id);
}

// RFC 7748 (sections 5.2 and 6.1) and RFC 4231 (test case 2).
TEST_CASE(engine_pairing_crypto) {
    auto key = [](const char* hex) {
        keys::Key k{};
        keys::from_hex(hex, MutableBytes(k.data(), k.size()));
        return k;
    };
    CHECK(keys::to_hex(keys::x25519(key("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                                    key("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"))) ==
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    keys::Key alice = key("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    keys::Key bob = key("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    keys::Key alice_pub = keys::x25519_base(alice), bob_pub = keys::x25519_base(bob);
    CHECK(keys::to_hex(alice_pub) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK(keys::to_hex(bob_pub) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    CHECK(keys::to_hex(keys::x25519(alice, bob_pub)) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK(keys::to_hex(keys::x25519(bob, alice_pub)) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    std::string jefe = "Jefe", data = "what do ya want for nothing?";
    CHECK(keys::to_hex(keys::hmac_sha256(Bytes(reinterpret_cast<const std::uint8_t*>(jefe.data()), jefe.size()),
                                         Bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    keys::Key k{};
    CHECK(!keys::from_hex("zz", MutableBytes(k.data(), 1)));
    CHECK(keys::equal(Bytes(alice.data(), 32), Bytes(alice.data(), 32)) && !keys::equal(Bytes(alice.data(), 32), Bytes(bob.data(), 32)));
}

namespace {
peers::Identity identity(const std::string& id, const std::string& name) {
    peers::Identity who;
    who.id = id;
    who.name = name;
    who.port = 5940;
    return who;
}
keys::Key random_key() {
    keys::Key k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::uint8_t>(i * 37 + 11);
    return k;
}
} // namespace

// Engine A's side of pairing, with the test playing engine B.
TEST_CASE(engine_pairing_protocol) {
    const std::string a_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", b_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    std::string dir = test::make_temp_dir();
    std::vector<std::string> notes;
    pairing::Pairing::Hooks hooks;
    hooks.notify = [&](const std::string& m) { notes.push_back(m); };
    pairing::Pairing a(dir, identity(a_id, "Console"), hooks);

    keys::Key b_secret = random_key(), b_pub = keys::x25519_base(b_secret);
    std::array<std::uint8_t, 16> b_nonce{};
    b_nonce[0] = 7;
    std::string commit_input(b_pub.begin(), b_pub.end());
    commit_input.append(b_nonce.begin(), b_nonce.end());
    JsonValue begin = JsonValue::object();
    begin.set("from", peers::identity_json(identity(b_id, "Desk PC")));
    begin.set("commit", keys::to_hex(keys::sha256(commit_input)));
    auto started = a.on_begin(begin, "192.168.1.20");
    CHECK(started.ok());
    if (!started.ok()) return;
    CHECK(a.incoming() && a.incoming()->code.empty()); // nothing to show before the reveal
    // Another engine cannot step in while this one pairs.
    JsonValue other = begin;
    other.set("from", peers::identity_json(identity("cccccccccccccccccccccccccccccccc", "Other")));
    CHECK(!a.on_begin(other, "192.168.1.30").ok());

    keys::Key a_pub{};
    std::array<std::uint8_t, 16> a_nonce{};
    CHECK(keys::from_hex((*started)["pub"].str(), MutableBytes(a_pub.data(), 32)));
    CHECK(keys::from_hex((*started)["nonce"].str(), MutableBytes(a_nonce.data(), 16)));
    // A key that does not match the commitment ends the attempt.
    JsonValue cheat = JsonValue::object();
    cheat.set("session", (*started)["session"].str());
    cheat.set("pub", keys::to_hex(keys::x25519_base(a_pub)));
    cheat.set("nonce", keys::to_hex(Bytes(b_nonce.data(), 16)));
    CHECK(!a.on_reveal(cheat).ok());
    CHECK(!a.incoming());

    // Again, honestly.
    started = a.on_begin(begin, "192.168.1.20");
    CHECK(started.ok());
    if (!started.ok()) return;
    keys::from_hex((*started)["pub"].str(), MutableBytes(a_pub.data(), 32));
    keys::from_hex((*started)["nonce"].str(), MutableBytes(a_nonce.data(), 16));
    JsonValue reveal = JsonValue::object();
    reveal.set("session", (*started)["session"].str());
    reveal.set("pub", keys::to_hex(b_pub));
    reveal.set("nonce", keys::to_hex(Bytes(b_nonce.data(), 16)));
    CHECK(a.on_reveal(reveal).ok());
    keys::Key shared = keys::x25519(b_secret, a_pub);
    std::string t = pairing::transcript(a_id, b_id, a_pub, b_pub, Bytes(a_nonce.data(), 16), Bytes(b_nonce.data(), 16));
    std::string code = pairing::code_for(shared, t);
    CHECK(code.size() == 6);
    CHECK(a.incoming() && a.incoming()->code == code.substr(0, 3) + " " + code.substr(3)); // both sides get the same code
    CHECK(notes.size() == 1 && notes[0].find(code.substr(0, 3) + " " + code.substr(3)) != std::string::npos);

    keys::Key key = pairing::key_for(shared, t);
    JsonValue finish = JsonValue::object();
    finish.set("session", (*started)["session"].str());
    finish.set("proof", keys::to_hex(keys::hmac_sha256(key, "B|" + t)));
    auto done = a.on_finish(finish);
    CHECK(done.ok());
    if (!done.ok()) return;
    CHECK((*done)["proof"].str() == keys::to_hex(keys::hmac_sha256(key, "A|" + t)));
    CHECK(a.is_paired(b_id) && !a.incoming());

    // B's signed requests: A accepts each once; any change is refused.
    // (The test signs as B with a second Pairing that knows the same key.)
    std::string bdir = test::make_temp_dir();
    test::write_file(bdir + "/paired.json", [&] {
        std::string j = R"({"engines":[{"id":")" + a_id + R"(","name":"Console","key":")" + keys::to_hex(key) + R"(","since":1}]})";
        return std::vector<std::uint8_t>(j.begin(), j.end());
    }());
    pairing::Pairing b(bdir, identity(b_id, "Desk PC"), {});
    auto headers = b.sign(a_id, "POST", "/api/pair/forget", {{"x", "1"}}, "{}");
    CHECK(headers.ok());
    if (!headers.ok()) return;
    HttpRequest r;
    r.method = "POST";
    r.path = "/api/pair/forget";
    r.query = {{"x", "1"}};
    r.body = "{}";
    for (const auto& [name, value] : *headers) {
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        r.headers[lower] = value;
    }
    HttpRequest tampered = r, moved = r;
    tampered.body = "{\"all\":true}";
    moved.path = "/api/pair/check";
    CHECK(!a.verify(tampered).ok());
    CHECK(!a.verify(moved).ok());
    auto who = a.verify(r);
    CHECK(who.ok() && *who == b_id);
    CHECK(!a.verify(r).ok()); // the same request again
    CHECK(!a.verify(HttpRequest{}).ok());

    // Kept across a restart.
    pairing::Pairing again(dir, identity(a_id, "Console"), {});
    CHECK(again.is_paired(b_id));
}

// How engines talk to each other has a number (peers::kPeerProtocol);
// engines with different numbers refuse each other, saying which to update.
TEST_CASE(engine_peer_protocol) {
    peers::Identity self;
    self.id = "0123456789abcdef0123456789abcdef";
    self.name = "Desk PC";
    self.port = 5940;
    self.protocol = peers::kPeerProtocol;
    // Sent in beacons and read back.
    auto b = peers::decode_beacon(peers::encode_beacon(self, false));
    CHECK(b.ok() && b->who.protocol == peers::kPeerProtocol);
    // v0.2.0 sent no number: it speaks protocol 1.
    auto old = peers::decode_beacon(R"({"alembic":"engine","v":1,"id":"0123456789abcdef0123456789abcdef","port":5940})");
    CHECK(old.ok() && old->who.protocol == 1);
    // Nonsense counts as 1 too.
    auto odd = peers::decode_beacon(R"({"alembic":"engine","v":1,"id":"0123456789abcdef0123456789abcdef","port":5940,"protocol":-3})");
    CHECK(odd.ok() && odd->who.protocol == 1);

    CHECK(peers::protocol_problem(self).empty());
    peers::Identity older = self, newer = self;
    older.protocol = peers::kPeerProtocol - 1;
    newer.protocol = peers::kPeerProtocol + 1;
    CHECK(peers::protocol_problem(older).find("Desk PC has an older Alembic") == 0);
    CHECK(peers::protocol_problem(older).find("update it") != std::string::npos);
    CHECK(peers::protocol_problem(newer).find("Desk PC has a newer Alembic") == 0);
    CHECK(peers::protocol_problem(newer).find("update this engine") != std::string::npos);

    // A request's header: none is protocol 1, as v0.2.0 sends none.
    CHECK(peers::protocol_problem(std::to_string(peers::kPeerProtocol)).empty());
    CHECK(peers::protocol_problem(std::string()).empty() == (peers::kPeerProtocol == 1));
    CHECK(!peers::protocol_problem(std::to_string(peers::kPeerProtocol + 1)).empty());
}

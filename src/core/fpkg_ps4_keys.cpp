// SPDX-License-Identifier: GPL-3.0-or-later
// PS4 package keys, read from maxton/LibOrbisPkg's Util/Keys.cs (LGPL-3.0)
// as the engine downloads it; nothing here holds key material. Also the
// keystone (Crypto.CreateKeystone) and the sce_sys entry names
// (PKG/Enums.cs EntryNames).
#include "alembic/fpkg_ps4.hpp"

#include "alembic/sha256.hpp"

#include <cstdio>
#include <cstring>

namespace alembic::fpkg::ps4 {

namespace {

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, "Keys.cs: " + message);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The 0x.. byte literals of the brace block that opens at or after `from`;
// `from` moves past its closing brace.
Result<std::vector<std::uint8_t>> next_block(std::string_view text, std::size_t& from) {
    std::size_t open = text.find('{', from);
    if (open == std::string_view::npos) return invalid("a byte array is missing");
    std::vector<std::uint8_t> out;
    std::size_t i = open + 1;
    for (; i < text.size() && text[i] != '}'; ++i) {
        if (text[i] == '{') return invalid("unexpected nested block");
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') { // a comment
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (text[i] == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            int value = 0, digits = 0;
            std::size_t j = i + 2;
            for (; j < text.size() && hex_value(text[j]) >= 0; ++j, ++digits) value = value * 16 + hex_value(text[j]);
            if (digits == 0 || digits > 2) return invalid("a byte literal is malformed");
            out.push_back(static_cast<std::uint8_t>(value));
            i = j - 1;
        }
    }
    if (i >= text.size()) return invalid("a byte array is not closed");
    from = i + 1;
    return out;
}

Result<std::vector<std::uint8_t>> named_array(std::string_view text, const std::string& name, std::size_t size) {
    std::size_t at = text.find("public static byte[] " + name + " ");
    if (at == std::string_view::npos) return invalid(name + " is missing");
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> bytes, next_block(text, at));
    if (bytes.size() != size) return invalid(name + " has the wrong length");
    return bytes;
}

// A field ("Modulus", ...) of the RSAKeyset declared as `name`.
Result<std::vector<std::uint8_t>> keyset_field(std::string_view text, const std::string& name, const std::string& field) {
    std::size_t start = text.find("public static RSAKeyset " + name + " ");
    if (start == std::string_view::npos) return invalid(name + " is missing");
    std::size_t end = text.find("public static", start + 1);
    std::size_t at = text.find(field + " = new byte", start);
    if (at == std::string_view::npos || (end != std::string_view::npos && at > end))
        return invalid(name + "." + field + " is missing");
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> bytes, next_block(text, at));
    if (bytes.size() != 256) return invalid(name + "." + field + " has the wrong length");
    return bytes;
}

} // namespace

Result<Keys> parse_keys(std::string_view text) {
    Keys keys;
    ALEMBIC_ASSIGN(keys.fake_modulus, keyset_field(text, "FakeKeyset", "Modulus"));
    ALEMBIC_ASSIGN(keys.rif_modulus, keyset_field(text, "DebugRifKeyset", "Modulus"));
    ALEMBIC_ASSIGN(keys.rif_private_exponent, keyset_field(text, "DebugRifKeyset", "PrivateExponent"));
    ALEMBIC_ASSIGN(auto rif, named_array(text, "rif_debug_key", 16));
    std::memcpy(keys.rif_debug_key.data(), rif.data(), 16);
    ALEMBIC_ASSIGN(auto hmac, named_array(text, "keystone_hmac_key", 32));
    std::memcpy(keys.keystone_hmac_key.data(), hmac.data(), 32);
    ALEMBIC_ASSIGN(auto mac, named_array(text, "keystone_mac_data", 32));
    std::memcpy(keys.keystone_mac_data.data(), mac.data(), 32);
    std::size_t at = text.find("public static byte[][] PkgPublicKeys");
    if (at == std::string_view::npos) return invalid("PkgPublicKeys is missing");
    at = text.find('{', at);
    if (at == std::string_view::npos) return invalid("PkgPublicKeys is malformed");
    ++at; // inside the outer block; each key is its own block
    for (auto& key : keys.package_public) {
        ALEMBIC_ASSIGN(key, next_block(text, at));
        if (key.size() != 256) return invalid("a PkgPublicKeys modulus has the wrong length");
    }
    return keys;
}

Result<Keys> load_keys(const std::string& keys_cs_path) {
    auto file = File::open(keys_cs_path, File::Mode::read);
    if (!file.ok())
        return make_error(Errc::invalid_argument, "missing the PS4 key file " + keys_cs_path +
                                                      " (get the PS4 package keys in Components, or pass --ps4-keys FILE)");
    ALEMBIC_ASSIGN(std::uint64_t size, file->size());
    if (size > 1024 * 1024) return invalid("the file is too large");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    ALEMBIC_TRY(file->read_at(0, data));
    if (alembic::Sha256::hex(crypto::sha256(Bytes(data.data(), data.size()))) != kKeysFileSha256)
        return make_error(Errc::invalid_data,
                          "the PS4 key file " + keys_cs_path + " is not the expected one (SHA-256 differs)");
    return parse_keys(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::vector<std::uint8_t> create_keystone(const Keys& keys, const std::string& passcode) {
    static const std::uint8_t kHeader[32] = {'k', 'e', 'y', 's', 't', 'o', 'n', 'e', 0x02, 0x00, 0x01};
    crypto::Digest fingerprint = crypto::hmac_sha256(
        Bytes(keys.keystone_hmac_key.data(), 32), Bytes(reinterpret_cast<const std::uint8_t*>(passcode.data()), passcode.size()));
    std::vector<std::uint8_t> out(kHeader, kHeader + 32);
    out.insert(out.end(), fingerprint.begin(), fingerprint.end());
    crypto::Digest mac = crypto::hmac_sha256(Bytes(keys.keystone_mac_data.data(), 32), Bytes(out.data(), out.size()));
    out.insert(out.end(), mac.begin(), mac.end());
    return out;
}

std::optional<std::uint32_t> entry_id_for_name(const std::string& name) {
    struct Named {
        const char* name;
        std::uint32_t id;
    };
    static const Named kNames[] = {
        {"license.dat", 0x400}, {"license.info", 0x401}, {"nptitle.dat", 0x402}, {"npbind.dat", 0x403},
        {"selfinfo.dat", 0x404}, {"imageinfo.dat", 0x406}, {"target-deltainfo.dat", 0x407},
        {"origin-deltainfo.dat", 0x408}, {"psreserved.dat", 0x409}, {"param.sfo", 0x1000},
        {"playgo-chunk.dat", 0x1001}, {"playgo-chunk.sha", 0x1002}, {"playgo-manifest.xml", 0x1003},
        {"pronunciation.xml", 0x1004}, {"pronunciation.sig", 0x1005}, {"pic1.png", 0x1006},
        {"pubtoolinfo.dat", 0x1007}, {"shareparam.json", 0x100B}, {"shareoverlayimage.png", 0x100C},
        {"save_data.png", 0x100D}, {"shareprivacyguardimage.png", 0x100E}, {"icon0.png", 0x1200},
        {"pic0.png", 0x1220}, {"snd0.at9", 0x1240}, {"icon0.dds", 0x1280}, {"pic0.dds", 0x12A0},
        {"pic1.dds", 0x12C0},
    };
    for (const Named& n : kNames)
        if (name == n.name) return n.id;
    // icon0_NN / pic1_NN .png / .dds, NN = 00..30.
    struct Series {
        const char* prefix;
        const char* suffix;
        std::uint32_t base;
    };
    static const Series kSeries[] = {
        {"icon0_", ".png", 0x1201}, {"icon0_", ".dds", 0x1281}, {"pic1_", ".png", 0x1241}, {"pic1_", ".dds", 0x12C1}};
    for (const Series& s : kSeries) {
        std::size_t p = std::strlen(s.prefix);
        if (name.size() != p + 2 + 4 || name.compare(0, p, s.prefix) != 0 || name.compare(p + 2, 4, s.suffix) != 0)
            continue;
        char a = name[p], b = name[p + 1];
        if (a < '0' || a > '9' || b < '0' || b > '9') continue;
        int n = (a - '0') * 10 + (b - '0');
        if (n < 31) return s.base + static_cast<std::uint32_t>(n);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> entry_id_for_path(const std::string& rel) {
    if (rel.find('/') == std::string::npos) return entry_id_for_name(rel);
    auto two = [](const std::string& s, std::size_t at) -> int {
        if (at + 2 > s.size() || s[at] < '0' || s[at] > '9' || s[at + 1] < '0' || s[at + 1] > '9') return -1;
        return (s[at] - '0') * 10 + (s[at + 1] - '0');
    };
    if (rel == "app/playgo-chunk.dat") return 0x1008;
    if (rel == "app/playgo-chunk.sha") return 0x1009;
    if (rel == "app/playgo-manifest.xml") return 0x100A;
    if (rel == "changeinfo/changeinfo.xml") return 0x1260;
    // changeinfo/changeinfo_NN.xml (NN < 31)
    if (rel.size() == 28 && rel.compare(0, 22, "changeinfo/changeinfo_") == 0 && rel.compare(24, 4, ".xml") == 0) {
        int n = two(rel, 22);
        if (n >= 0 && n < 31) return 0x1261u + static_cast<std::uint32_t>(n);
    }
    // trophy/trophyNN.trp (NN < 100)
    if (rel.size() == 19 && rel.compare(0, 13, "trophy/trophy") == 0 && rel.compare(15, 4, ".trp") == 0) {
        int n = two(rel, 13);
        if (n >= 0) return 0x1400u + static_cast<std::uint32_t>(n);
    }
    // keymap_rp/0NN.png (NN = 01..10) and keymap_rp/II/0JJ.png (II < 31, JJ = 01..10)
    if (rel.compare(0, 10, "keymap_rp/") == 0) {
        if (rel.size() == 17 && rel[10] == '0' && rel.compare(13, 4, ".png") == 0) {
            int n = two(rel, 11);
            if (n >= 1 && n <= 10) return 0x1600u + static_cast<std::uint32_t>(n - 1);
        }
        if (rel.size() == 20 && rel[12] == '/' && rel[13] == '0' && rel.compare(16, 4, ".png") == 0) {
            int i = two(rel, 10), j = two(rel, 14);
            if (i >= 0 && i < 31 && j >= 1 && j <= 10) return 0x1610u + static_cast<std::uint32_t>(16 * i + j - 1);
        }
    }
    return std::nullopt;
}

} // namespace alembic::fpkg::ps4

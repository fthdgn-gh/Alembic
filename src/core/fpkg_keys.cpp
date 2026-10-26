// SPDX-License-Identifier: GPL-3.0-or-later
// The RSA-3072 key wraps of a debug package (the wrap itself is in rsa.cpp,
// ported from pearlxcore/ProsperoPkgTool (MIT)). Only public-key operations (exponent 65537)
// under public moduli; the moduli are not part of Alembic and are loaded from
// files the user supplies (the engine's Components page downloads them,
// pinned by SHA-256).
#include "alembic/fpkg_build.hpp"

#include "alembic/bytes.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/rsa.hpp"
#include "alembic/sha256.hpp"

#include <algorithm>
#include <cstring>

namespace alembic::fpkg::build {

namespace {

constexpr std::size_t kModulus = 384;

// The MT19937-padded wrap (rsa.hpp) under a 384-byte modulus.
std::vector<std::uint8_t> rsa_wrap(const std::uint8_t* modulus, const std::uint8_t* data, std::size_t n) {
    return rsa::mt_wrap(Bytes(modulus, kModulus), Bytes(data, n));
}

struct KeyFile {
    const char* name;
    std::size_t size;
    const char* sha256;
};
// The files the engine downloads (src/engine/deps.cpp, component fpkg_keys).
constexpr KeyFile kKeyFiles[] = {
    {"passcode.bin", 7 * kModulus, "81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e"},
    {"mount_image.bin", kModulus, "4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0"},
};

} // namespace

Result<PackageKeys> load_package_keys(const std::string& folder) {
    PackageKeys keys;
    for (const KeyFile& k : kKeyFiles) {
        std::string path = folder + "/" + k.name;
        auto file = File::open(path, File::Mode::read);
        if (!file.ok())
            return make_error(Errc::invalid_argument, "missing FPKG key file " + path +
                                                          " (get the FPKG key files in Components, or pass --keys DIR)");
        ALEMBIC_ASSIGN(std::uint64_t size, file->size());
        if (size != k.size) return make_error(Errc::invalid_data, "the key file " + path + " has the wrong size");
        std::vector<std::uint8_t> data(k.size);
        ALEMBIC_TRY(file->read_at(0, data));
        if (alembic::Sha256::hex(crypto::sha256(Bytes(data.data(), data.size()))) != k.sha256)
            return make_error(Errc::invalid_data, "the key file " + path + " is not the expected one (SHA-256 differs)");
        if (std::string(k.name) == "passcode.bin") keys.passcode_moduli = std::move(data);
        else keys.mount_image_modulus = std::move(data);
    }
    return keys;
}

std::vector<std::uint8_t> build_entry_keys(const PackageKeys& keys, const std::string& content_id,
                                           const std::string& passcode) {
    std::vector<std::uint8_t> entry;
    std::uint8_t cid[48] = {};
    std::memcpy(cid, content_id.data(), std::min<std::size_t>(content_id.size(), 48));
    crypto::Digest seed = crypto::sha3_256(Bytes(cid, 48));
    entry.insert(entry.end(), seed.begin(), seed.end());
    for (std::uint32_t i = 0; i < 7; ++i) {
        crypto::Digest key = compute_keys(content_id, passcode, i);
        crypto::Digest d = crypto::sha3_256(Bytes(key.data(), 32));
        for (int j = 0; j < 32; ++j) d[j] ^= key[j];
        entry.insert(entry.end(), d.begin(), d.end());
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        std::vector<std::uint8_t> wrap;
        if (i == 0) {
            wrap = rsa_wrap(&keys.passcode_moduli[0], reinterpret_cast<const std::uint8_t*>(passcode.data()), passcode.size());
        } else {
            crypto::Digest key = compute_keys(content_id, passcode, i);
            wrap = rsa_wrap(&keys.passcode_moduli[i * kModulus], key.data(), 32);
        }
        entry.insert(entry.end(), wrap.begin(), wrap.end());
    }
    return entry;
}

std::vector<std::uint8_t> build_image_key(const PackageKeys& keys, const crypto::Digest& ekpfs) {
    std::vector<std::uint8_t> entry;
    std::vector<std::uint8_t> wrap = rsa_wrap(keys.mount_image_modulus.data(), ekpfs.data(), 32);
    while (entry.size() < 0x800) {
        std::size_t take = std::min<std::size_t>(kModulus, 0x800 - entry.size());
        entry.insert(entry.end(), wrap.begin(), wrap.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return entry;
}

std::vector<std::uint8_t> build_cnt_header_wrap(const PackageKeys& keys, const std::uint8_t* header4096) {
    crypto::Digest d = crypto::sha3_256(Bytes(header4096, 4096));
    return rsa_wrap(&keys.passcode_moduli[3 * kModulus], d.data(), 32);
}

} // namespace alembic::fpkg::build

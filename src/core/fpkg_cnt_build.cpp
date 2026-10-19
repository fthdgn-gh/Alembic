// SPDX-License-Identifier: GPL-3.0-or-later
// The CNT container of a debug package (header, entry table and body; the
// outer PFS image follows it in the package). Ported from
// pearlxcore/ProsperoPkgTool (MIT) Containers/ProsperoCntBuilder.cs
// (BuildPrefix, WriteHeader, WriteGeneralDigestsEntry and the rollups),
// Containers/ProsperoCntEntryPolicy.cs and Crypto/ProsperoEntryCipher.cs.
#include "alembic/fpkg_build.hpp"

#include "alembic/bytes.hpp"
#include "alembic/fpkg.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace alembic::fpkg::build {

namespace {

constexpr std::uint64_t kBodyOffset = 0x2000;
constexpr std::uint64_t kBlock = 0x10000;
constexpr std::size_t kHeaderSize = 0x5A0;

void be16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void be32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
void be64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
}
std::uint64_t align(std::uint64_t v, std::uint64_t a) { return v % a ? v + a - v % a : v; }

bool encrypted(const CntEntry& e) { return (e.flags1 & 0x80000000u) != 0; }
std::size_t stored_length(const CntEntry& e) { return encrypted(e) ? align(e.data.size(), 16) : e.data.size(); }

CntEntry* find(std::vector<CntEntry>& entries, std::uint32_t id) {
    for (CntEntry& e : entries)
        if (e.id == id) return &e;
    return nullptr;
}

// SHA3 of the concatenated per-entry SHA3s of `ids` that are present, or null.
std::optional<crypto::Digest> rollup(std::vector<CntEntry>& entries, std::initializer_list<std::uint32_t> ids) {
    std::vector<std::uint8_t> concat;
    for (std::uint32_t id : ids)
        if (CntEntry* e = find(entries, id)) {
            crypto::Digest d = crypto::sha3_256(e->data);
            concat.insert(concat.end(), d.begin(), d.end());
        }
    if (concat.empty()) return std::nullopt;
    return crypto::sha3_256(concat);
}

} // namespace

CntProfile cnt_entry_profile(std::uint32_t id, const std::string& relative_name, const std::string& drm_type) {
    auto protected_entry = [](std::uint32_t key, bool name) { return CntProfile{0x80000000u, key << 12, name}; };
    switch (id) {
    case 0x0001: return {0x40000000, 0, false};
    case 0x0010:
    case 0x0020:
    case 0x0080:
    case 0x0100: return {0x60000000, 0, false};
    case 0x0200: return {0x40000000, 0, false};
    case 0x0400: return protected_entry(3, false);
    case 0x0401: {
        std::string d = drm_type;
        for (char& c : d) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return protected_entry(d == "upgradable" ? 4 : 2, false);
    }
    case 0x0402:
    case 0x0403:
    case 0x0404:
    case 0x0406:
    case 0x0407:
    case 0x0408:
    case 0x0409: return protected_entry(3, false);
    case 0x2000: return {0, 0, true};
    default: {
        std::string n = relative_name;
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool nested_npbind = n == "npbind.dat" || (n.size() >= 11 && n.compare(n.size() - 11, 11, "/npbind.dat") == 0);
        if (nested_npbind) return protected_entry(3, true);
        return {0x08000000, 0, true};
    }
    }
}

Result<std::vector<std::uint8_t>> build_cnt(std::vector<CntEntry>& entries, const CntParameters& p,
                                            const PackageKeys& keys) {
    if (p.content_id.size() != 36) return make_error(Errc::invalid_argument, "the content ID must be 36 characters");
    CntEntry* metas = find(entries, 0x0100);
    CntEntry* digests = find(entries, 0x0001);
    CntEntry* names = find(entries, 0x0200);
    CntEntry* image_key = find(entries, 0x0020);
    CntEntry* mandatory = find(entries, 0x040A);
    CntEntry* general = find(entries, 0x0080);
    if (!metas || !digests || !names || !image_key || !mandatory || !general)
        return make_error(Errc::invalid_argument, "the CNT is missing a standard entry");
    const std::size_t count = entries.size();
    metas->data.assign(count * 32, 0);
    digests->data.assign(count * 32, 0);

    // Name table: offset 0 = "", then the named entries in ordinal order.
    std::map<const CntEntry*, std::uint32_t> name_offset;
    {
        std::vector<CntEntry*> named;
        for (CntEntry& e : entries)
            if (!e.name.empty()) named.push_back(&e);
        std::stable_sort(named.begin(), named.end(), [](const CntEntry* a, const CntEntry* b) { return a->name < b->name; });
        names->data.assign(1, 0);
        for (CntEntry* e : named) {
            name_offset[e] = static_cast<std::uint32_t>(names->data.size());
            names->data.insert(names->data.end(), e->name.begin(), e->name.end());
            names->data.push_back(0);
        }
    }

    // 1. Offsets, 16-byte aligned from the body offset, in list order.
    std::uint64_t cursor = kBodyOffset;
    for (CntEntry& e : entries) {
        e.data_offset = static_cast<std::uint32_t>(cursor);
        cursor = align(cursor + stored_length(e), 16);
    }
    const std::uint64_t body_end = cursor;
    const std::uint64_t image_offset = align(body_end, kBlock); // == body offset + body size
    const std::uint64_t body_size = image_offset - kBodyOffset;

    // 2. The entry table, sorted by id.
    std::vector<CntEntry*> sorted;
    for (CntEntry& e : entries) sorted.push_back(&e);
    std::stable_sort(sorted.begin(), sorted.end(), [](const CntEntry* a, const CntEntry* b) { return a->id < b->id; });
    std::vector<std::uint8_t> table(count * 32, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const CntEntry& e = *sorted[i];
        std::uint8_t* row = &table[i * 32];
        be32(row, e.id);
        auto it = name_offset.find(&e);
        be32(row + 4, it == name_offset.end() ? 0 : it->second);
        be32(row + 8, e.flags1);
        be32(row + 12, e.flags2);
        be32(row + 16, e.data_offset);
        be32(row + 20, static_cast<std::uint32_t>(e.data.size()));
    }
    metas->data = table;

    // Stored payloads: protected entries are AES-CBC encrypted, keyed by their row.
    auto stored = [&](const CntEntry& e) -> std::vector<std::uint8_t> {
        if (!encrypted(e)) return e.data;
        std::size_t index = static_cast<std::size_t>(std::find(sorted.begin(), sorted.end(), &e) - sorted.begin());
        std::uint32_t key_index = (e.flags2 & 0xF000u) >> 12;
        crypto::Digest package_key = compute_keys(p.content_id, p.passcode, key_index);
        std::uint8_t seed_input[64];
        std::memcpy(seed_input, &table[index * 32], 32);
        std::memcpy(seed_input + 32, package_key.data(), 32);
        crypto::Digest seed = crypto::sha3_256(Bytes(seed_input, 64));
        std::vector<std::uint8_t> buffer(align(e.data.size(), 16), 0);
        if (!e.data.empty()) std::memcpy(buffer.data(), e.data.data(), e.data.size());
        crypto::Aes128 aes(seed.data() + 16);
        crypto::cbc_encrypt(aes, seed.data(), buffer);
        return buffer;
    };

    const std::uint64_t mandatory_size = mandatory->data_offset;
    const std::uint64_t container_size = image_offset;
    const std::uint64_t mount_size = kBlock + p.pfs_image_size + container_size;
    const std::uint64_t cnt_region_offset = kBlock + p.pfs_image_size;
    std::vector<std::uint8_t> cnt(image_offset, 0);

    auto write_header = [&](const crypto::Digest& sc1, const crypto::Digest& sc2, const crypto::Digest& body_digest,
                            const crypto::Digest& table_hash) {
        std::uint8_t* h = cnt.data();
        std::fill(h, h + kHeaderSize, 0);
        h[0] = 0x7F;
        h[1] = 'C';
        h[2] = 'N';
        h[3] = 'T';
        be16(h + 4, 2);
        be16(h + 6, 1);
        be32(h + 8, 0x80000000);
        be32(h + 0xC, 0x0C);
        be32(h + 0x10, static_cast<std::uint32_t>(count));
        be16(h + 0x14, 6);
        be16(h + 0x16, static_cast<std::uint16_t>(count));
        be32(h + 0x18, metas->data_offset);
        std::uint32_t main_size = 0;
        for (const CntEntry& e : entries)
            if (e.id == 0x1 || e.id == 0x10 || e.id == 0x20 || e.id == 0x80 || e.id == 0x100)
                main_size += static_cast<std::uint32_t>(e.data.size());
        be32(h + 0x1C, main_size);
        be64(h + 0x20, kBodyOffset);
        be64(h + 0x28, body_size);
        be64(h + 0x30, mandatory_size);
        std::memcpy(h + 0x40, p.content_id.data(), 36);
        be32(h + 0x70, 0);
        be32(h + 0x74, p.content_type);
        be32(h + 0x78, p.content_flags);
        be32(h + 0x7C, static_cast<std::uint32_t>(image_offset));
        be32(h + 0x80, p.version_date);
        be32(h + 0x84, p.version_hash);
        std::memcpy(h + 0x100, sc1.data(), 32);
        std::memcpy(h + 0x120, sc2.data(), 32);
        std::memcpy(h + 0x140, table_hash.data(), 32);
        std::memcpy(h + 0x160, body_digest.data(), 32);
        std::memcpy(h + 0x200, p.content_id.data(), 36);
        be32(h + 0x400, 1);
        be32(h + 0x404, 1);
        be64(h + 0x408, 0xA00000000000030CULL);
        be64(h + 0x410, kBlock);
        be64(h + 0x418, p.pfs_image_size);
        be64(h + 0x420, 0);
        be64(h + 0x428, mount_size);
        be64(h + 0x430, mount_size);
        be32(h + 0x438, static_cast<std::uint32_t>(kBlock));
        std::memcpy(h + 0x440, p.pfs_image_digest.data(), 32);
        std::memcpy(h + 0x460, p.pfs_signed_digest.data(), 32);
        std::memcpy(h + 0x4A0, p.image_seed.data(), 16);
        be64(h + 0x4B0, cnt_region_offset);
        be64(h + 0x4B8, container_size);
        be32(h + 0x510, image_key->data_offset);
        be32(h + 0x514, static_cast<std::uint32_t>(image_key->data.size()));
        be32(h + 0x518, mandatory->data_offset);
        be32(h + 0x51C, static_cast<std::uint32_t>(mandatory->data.size()));
        crypto::Digest a = crypto::sha3_256(image_key->data), b = crypto::sha3_256(mandatory->data);
        std::memcpy(h + 0x520, a.data(), 32);
        std::memcpy(h + 0x540, b.data(), 32);
    };

    // 3. Skeleton header, then the general digests against it.
    const crypto::Digest zero{};
    write_header(zero, zero, zero, zero);
    {
        std::vector<std::uint8_t> descriptor(0x38, 0);
        std::memcpy(descriptor.data(), p.content_id.data(), 36);
        be32(&descriptor[0x30], 0);
        be32(&descriptor[0x34], p.content_type);
        std::vector<std::uint8_t> pre = descriptor;
        pre.insert(pre.end(), p.pfs_image_digest.begin(), p.pfs_image_digest.end());
        pre.resize(pre.size() + 32, 0);
        crypto::Digest content = crypto::sha3_256(pre);

        std::vector<std::uint8_t>& g = general->data;
        g.assign(0x1E0, 0);
        be16(&g[0], 0xD256);
        be16(&g[2], 0x0102);
        be32(&g[0x1C], 0x10DE);
        std::memcpy(&g[0x20], content.data(), 32);
        std::memcpy(&g[0x40], p.pfs_image_digest.data(), 32);
        std::vector<std::uint8_t> hd(cnt.begin(), cnt.begin() + 0x40);
        hd.insert(hd.end(), cnt.begin() + 0x400, cnt.begin() + 0x480);
        be64(&hd[0x40 + 0x10], kBlock);
        crypto::Digest header_digest = crypto::sha3_256(hd);
        std::memcpy(&g[0x60], header_digest.data(), 32);
        if (auto sys = rollup(entries, {0x1006, 0x100D, 0x1200, 0x1220, 0x1240, 0x1280, 0x12A0, 0x12C0, 0x2040, 0x2060}))
            std::memcpy(&g[0x80], sys->data(), 32);
        if (CntEntry* param = find(entries, 0x2000)) {
            crypto::Digest d = crypto::sha3_256(param->data);
            std::memcpy(&g[0xC0], d.data(), 32);
        }
        if (auto pg = rollup(entries, {0x1001, 0x2010, 0x2011})) std::memcpy(&g[0xE0], pg->data(), 32);
        std::memcpy(&g[0x180], p.pfs_image_digest.data(), 32); // target digest == game digest
    }

    // 5. Per-entry digests (the digests entry's own slot stays zero).
    std::vector<std::uint8_t> digest_table(count * 32, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (sorted[i] == digests) continue;
        crypto::Digest d = crypto::sha3_256(stored(*sorted[i]));
        std::memcpy(&digest_table[i * 32], d.data(), 32);
    }
    digests->data = digest_table;

    // 6. The body.
    for (const CntEntry& e : entries) {
        std::vector<std::uint8_t> payload = stored(e);
        if (!payload.empty()) std::memcpy(&cnt[e.data_offset], payload.data(), payload.size());
    }

    // 7. Rollups over the final body, then the real header.
    auto concat = [&](std::initializer_list<std::uint32_t> ids) {
        std::vector<std::uint8_t> out;
        for (std::uint32_t id : ids)
            if (CntEntry* e = find(entries, id)) out.insert(out.end(), e->data.begin(), e->data.end());
        return out;
    };
    crypto::Digest sc1 = crypto::sha3_256(concat({0x10, 0x20, 0x80, 0x100, 0x1}));
    std::vector<std::uint8_t> sc2_pre = concat({0x10, 0x20, 0x80});
    sc2_pre.insert(sc2_pre.end(), metas->data.begin(), metas->data.begin() + 6 * 32);
    crypto::Digest sc2 = crypto::sha3_256(sc2_pre);
    crypto::Digest body_digest = crypto::sha3_256(Bytes(cnt.data() + kBodyOffset, body_size));
    crypto::Digest table_hash = crypto::sha3_256(digest_table);
    write_header(sc1, sc2, body_digest, table_hash);

    // 8. The package digest over [0, 0xFE0) and the RSA wrap of [0, 0x1000).
    crypto::Digest package = crypto::sha3_256(Bytes(cnt.data(), 0xFE0));
    std::memcpy(&cnt[0xFE0], package.data(), 32);
    std::vector<std::uint8_t> wrap = build_cnt_header_wrap(keys, cnt.data());
    std::memcpy(&cnt[0x1000], wrap.data(), wrap.size());
    return cnt;
}

} // namespace alembic::fpkg::build

// SPDX-License-Identifier: GPL-3.0-or-later
// PlayGo entries of a debug package: playgo-chunk.dat (PLGX),
// playgo-hash-table.dat and playgo-ficm.dat. Ported from
// pearlxcore/ProsperoPkgTool (MIT) Containers/ProsperoPlayGoChunkBuilder.cs,
// Containers/ProsperoPlayGoBuilders.cs, Gp5/PlayGoChunk.cs (PlayGoChunkReader)
// and the fallbacks of Containers/ProsperoDebugPackageBuilder.cs
// (ResolvePlayGoProject, BuildPlayGoFamily).
#include "alembic/fpkg_build.hpp"

#include "alembic/bytes.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace alembic::fpkg::build {

namespace {

constexpr std::uint32_t kPlgx = 0x78676C70;
constexpr std::uint64_t kOuterBlock = 0x10000;

Error bad(const std::string& m) { return make_error(Errc::invalid_data, m); }

void section(std::vector<std::uint8_t>& d, std::size_t at, std::size_t start, std::size_t size) {
    put_u32(&d[at], static_cast<std::uint32_t>(start));
    put_u32(&d[at + 4], static_cast<std::uint32_t>(size));
}

std::vector<std::uint8_t> single_chunk_profile(const std::string& content_id, std::uint64_t language_mask,
                                               std::uint64_t mchunk0, std::uint64_t mchunk1) {
    std::vector<std::uint8_t> d(0x1A0, 0);
    put_u32(&d[0], kPlgx);
    put_u16(&d[4], 0x1000);
    put_u16(&d[8], 1);
    put_u16(&d[0xA], 1);
    put_u16(&d[0xE], 1);
    put_u32(&d[0x10], 0x1A0);
    put_u16(&d[0x16], 1);
    d[0x1E] = 0x85;
    d[0x20] = 0x02;
    d[0x24] = 0x01;
    d[0x30] = 0x11;
    std::fill(d.begin() + 0x38, d.begin() + 0x40, 0xFF);
    std::memcpy(&d[0x40], content_id.data(), std::min<std::size_t>(content_id.size(), 0x80));
    section(d, 0xC0, 0x100, 0x20);
    section(d, 0xC8, 0x120, 0x08);
    section(d, 0xD0, 0x130, 0x09);
    section(d, 0xD8, 0x140, 0x20);
    section(d, 0xE0, 0x160, 0x20);
    section(d, 0xE8, 0x180, 0x02);
    section(d, 0xF0, 0x190, 0x0C);
    d[0x100] = 0x80;
    d[0x102] = 0x03;
    d[0x104] = 0x02;
    d[0x108] = 0x11;
    put_u64(&d[0x110], language_mask);
    put_u32(&d[0x124], 1);
    std::memcpy(&d[0x130], "Chunk #0\0", 9);
    put_u64(&d[0x140], 0);
    put_u64(&d[0x148], mchunk0);
    put_u64(&d[0x150], mchunk0);
    put_u64(&d[0x158], mchunk1);
    put_u64(&d[0x160], 0x21);
    d[0x174] = 1;
    d[0x176] = 1;
    std::memcpy(&d[0x190], "Scenario #0\0", 12);
    return d;
}

bool printable(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < ' ' || c > '~') return false;
    return true;
}

Status validate(const std::string& content_id, const PlayGoProject& p) {
    if (p.chunks.empty() || p.chunks.size() > 255 || p.scenarios.empty() || p.scenarios.size() > 0xFFFF ||
        p.default_scenario < 0 || static_cast<std::size_t>(p.default_scenario) >= p.scenarios.size())
        return bad("unsupported PlayGo project counts or default scenario");
    if (!printable(content_id) || content_id.size() > 128) return bad("PlayGo content ID must be printable ASCII");
    for (const auto& c : p.chunks)
        if (!printable(c.label)) return bad("PlayGo chunk labels must be printable ASCII");
    for (const auto& s : p.scenarios) {
        std::set<int> seen;
        if (s.chunks.empty() || s.initial_chunk_count < 0 || static_cast<std::size_t>(s.initial_chunk_count) > s.chunks.size())
            return bad("invalid PlayGo scenario");
        for (int c : s.chunks)
            if (c < 0 || static_cast<std::size_t>(c) >= p.chunks.size() || !seen.insert(c).second) return bad("invalid PlayGo scenario");
        if (!printable(s.label)) return bad("PlayGo scenario labels must be printable ASCII");
    }
    return {};
}

struct Extent {
    std::uint64_t start, length;
    int chunk;
};

Result<std::vector<std::uint8_t>> build_chunk(const std::string& content_id, const PlayGoProject& project,
                                              const InnerImage& inner, std::uint64_t pfs_offset, std::uint64_t cnt_offset) {
    ALEMBIC_TRY(validate(content_id, project));
    if (pfs_offset % kOuterBlock != 0 || cnt_offset <= pfs_offset) return bad("PLGX needs ordered, aligned offsets");
    // Every file is in chunk 0 (no per-file assignments), so the extents are
    // [0, metadata) and [metadata, cnt), both owned by chunk 0.
    std::uint64_t metadata = pfs_offset + inner.metadata_on_disk_offset;
    if (metadata > cnt_offset || metadata % kOuterBlock != 0) return bad("PLGX metadata boundary is misaligned");
    std::vector<Extent> extents;
    auto add = [&](std::uint64_t s, std::uint64_t e, int owner) -> Status {
        if (e <= s || (e - s) % kOuterBlock != 0) return bad("PLGX extent is not a positive 64 KiB range");
        extents.push_back({s, e - s, owner});
        return {};
    };
    ALEMBIC_TRY(add(0, metadata, 0));
    ALEMBIC_TRY(add(metadata, cnt_offset, 0));
    std::vector<std::vector<std::uint32_t>> ids(project.chunks.size());
    for (std::size_t i = 0; i < extents.size(); ++i) ids[static_cast<std::size_t>(extents[i].chunk)].push_back(static_cast<std::uint32_t>(i));

    if (project.chunks.size() == 1 && project.scenarios.size() == 1) {
        std::uint64_t aligned = inner.image_length == 0 ? 0 : align_up(inner.image_length, kOuterBlock);
        std::uint64_t total = cnt_offset;
        std::uint64_t m0 = aligned > 0 && aligned < total ? aligned : total - kOuterBlock;
        return single_chunk_profile(content_id, project.chunks[0].language_mask, m0, total - m0);
    }

    // The general serializer.
    std::vector<std::uint8_t> chunk_labels, scenario_labels;
    std::vector<std::uint32_t> chunk_label_off, scenario_label_off;
    for (const auto& c : project.chunks) {
        chunk_label_off.push_back(static_cast<std::uint32_t>(chunk_labels.size()));
        chunk_labels.insert(chunk_labels.end(), c.label.begin(), c.label.end());
        chunk_labels.push_back(0);
    }
    for (const auto& s : project.scenarios) {
        scenario_label_off.push_back(static_cast<std::uint32_t>(scenario_labels.size()));
        scenario_labels.insert(scenario_labels.end(), s.label.begin(), s.label.end());
        scenario_labels.push_back(0);
    }
    const std::size_t rec = 0x20, header = 0x100;
    std::size_t c8_size = extents.size() * 4;
    std::size_t d0 = header + project.chunks.size() * rec + c8_size;
    std::size_t d8 = d0 + chunk_labels.size();
    std::size_t e0 = d8 + extents.size() * 16;
    std::size_t e8 = e0 + project.scenarios.size() * rec;
    std::size_t items = 0;
    for (const auto& s : project.scenarios) items += s.chunks.size();
    std::size_t f0 = e8 + items * 2;
    std::size_t f8 = f0 + scenario_labels.size();
    std::vector<std::uint8_t> d(f8, 0);
    put_u32(&d[0], kPlgx);
    put_u16(&d[4], 0x1000);
    put_u16(&d[8], 1);
    put_u16(&d[0xA], static_cast<std::uint16_t>(project.chunks.size()));
    put_u16(&d[0xE], static_cast<std::uint16_t>(project.scenarios.size()));
    put_u32(&d[0x10], static_cast<std::uint32_t>(d.size()));
    put_u16(&d[0x14], static_cast<std::uint16_t>(project.default_scenario));
    put_u16(&d[0x16], 1);
    put_u32(&d[0x1C], 0x85);
    put_u32(&d[0x20], 4);
    put_u32(&d[0x24], 1);
    put_u32(&d[0x30], 0x11);
    put_u64(&d[0x38], ~0ULL);
    std::memcpy(&d[0x40], content_id.data(), content_id.size());
    section(d, 0xC0, header, project.chunks.size() * rec);
    section(d, 0xC8, header + project.chunks.size() * rec, c8_size);
    section(d, 0xD0, d0, chunk_labels.size());
    section(d, 0xD8, d8, extents.size() * 16);
    section(d, 0xE0, e0, project.scenarios.size() * rec);
    section(d, 0xE8, e8, items * 2);
    section(d, 0xF0, f0, scenario_labels.size());
    section(d, 0xF8, f8, 0);
    std::size_t c8 = header + project.chunks.size() * rec, list = 0;
    for (std::size_t i = 0; i < project.chunks.size(); ++i) {
        std::size_t at = header + i * rec;
        put_u32(&d[at], 0x00030080);
        put_u32(&d[at + 4], static_cast<std::uint32_t>(ids[i].size()));
        put_u32(&d[at + 8], 0x11);
        put_u64(&d[at + 0x10], project.chunks[i].language_mask);
        put_u32(&d[at + 0x18], static_cast<std::uint32_t>(list));
        put_u32(&d[at + 0x1C], chunk_label_off[i]);
        for (std::uint32_t id : ids[i]) {
            put_u32(&d[c8 + list], id);
            list += 4;
        }
    }
    std::memcpy(&d[d0], chunk_labels.data(), chunk_labels.size());
    for (std::size_t i = 0; i < extents.size(); ++i) {
        put_u64(&d[d8 + i * 16], extents[i].start);
        put_u64(&d[d8 + i * 16 + 8], extents[i].length);
    }
    std::size_t seq = 0;
    for (std::size_t i = 0; i < project.scenarios.size(); ++i) {
        const auto& s = project.scenarios[i];
        std::size_t at = e0 + i * rec;
        put_u32(&d[at], 0x21);
        put_u16(&d[at + 0x14], static_cast<std::uint16_t>(s.initial_chunk_count));
        put_u16(&d[at + 0x16], static_cast<std::uint16_t>(s.chunks.size()));
        put_u32(&d[at + 0x18], static_cast<std::uint32_t>(seq));
        put_u32(&d[at + 0x1C], scenario_label_off[i]);
        for (int c : s.chunks) {
            put_u16(&d[e8 + seq], static_cast<std::uint16_t>(c));
            seq += 2;
        }
    }
    std::memcpy(&d[f0], scenario_labels.data(), scenario_labels.size());
    return d;
}

std::string normalize_path(const std::string& path) {
    std::string out;
    std::size_t i = 0;
    while (i < path.size() && (path[i] == '/' || path[i] == '\\')) ++i;
    for (; i < path.size(); ++i) {
        char c = path[i] == '\\' ? '/' : path[i];
        if (c == '/') {
            if (!out.empty() && out.back() != '/') out.push_back('/');
            continue;
        }
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        out.push_back(static_cast<unsigned char>(c) <= 0x7F ? c : '?');
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

} // namespace

PlayGoProject default_playgo_project() {
    PlayGoProject p;
    p.chunks.push_back({"Chunk #0", ~0ULL});
    p.scenarios.push_back({"Scenario #0", 1, {0}});
    return p;
}

Result<PlayGoProject> parse_playgo_chunk(const std::vector<std::uint8_t>& data_in) {
    if (data_in.size() < 0x100) return bad("playgo-chunk.dat is shorter than its header");
    const std::uint8_t* d = data_in.data();
    if (get_u32(d) != kPlgx) return bad("playgo-chunk.dat has no plgx magic");
    int images = get_u16(d + 8), chunks = get_u16(d + 0xA), scenarios = get_u16(d + 0xE);
    std::uint32_t declared = get_u32(d + 0x10);
    int def = get_u16(d + 0x14);
    if (declared < 0x100 || declared > data_in.size()) return bad("playgo-chunk.dat size is outside the file");
    const std::size_t size = declared;
    if (images < 1 || images > 2 || chunks < 1 || chunks > 1000 || scenarios < 1 || scenarios > 32 || def >= scenarios)
        return bad("implausible PlayGo counts");
    struct Sec {
        std::size_t off, size;
    };
    auto sec = [&](std::size_t at) -> Result<Sec> {
        std::uint32_t o = get_u32(d + at), s = get_u32(d + at + 4);
        if (o > size || static_cast<std::uint64_t>(o) + s > size) return bad("a PlayGo section is outside the file");
        return Sec{o, s};
    };
    ALEMBIC_ASSIGN(Sec chunk_attrs, sec(0xC0));
    ALEMBIC_ASSIGN(Sec extent_ids, sec(0xC8));
    ALEMBIC_ASSIGN(Sec chunk_labels, sec(0xD0));
    ALEMBIC_ASSIGN(Sec extents, sec(0xD8));
    ALEMBIC_ASSIGN(Sec scenario_attrs, sec(0xE0));
    ALEMBIC_ASSIGN(Sec scenario_chunks, sec(0xE8));
    ALEMBIC_ASSIGN(Sec scenario_labels, sec(0xF0));
    if (static_cast<std::size_t>(chunks) * 0x20 > chunk_attrs.size || static_cast<std::size_t>(scenarios) * 0x20 > scenario_attrs.size ||
        extents.size % 16 || extent_ids.size % 4)
        return bad("PlayGo sections are too short");
    std::size_t extent_count = extents.size / 16;
    auto label = [&](const Sec& s, std::uint32_t rel) -> Result<std::string> {
        if (rel >= s.size) return bad("a PlayGo label is outside its section");
        std::string out;
        for (std::size_t i = s.off + rel; i < s.off + s.size; ++i) {
            if (d[i] == 0) return out;
            if (d[i] < 0x20 || d[i] > 0x7E) return bad("a PlayGo label has control characters");
            out.push_back(static_cast<char>(d[i]));
        }
        return bad("a PlayGo label is not NUL terminated");
    };
    PlayGoProject p;
    p.default_scenario = def;
    for (int i = 0; i < chunks; ++i) {
        std::size_t at = chunk_attrs.off + static_cast<std::size_t>(i) * 0x20;
        PlayGoProject::Chunk c;
        c.language_mask = get_u64(d + at + 0x10);
        ALEMBIC_ASSIGN(c.label, label(chunk_labels, get_u32(d + at + 0x1C)));
        std::uint64_t count = get_u32(d + at + 4), list = get_u32(d + at + 0x18);
        if (list + count * 4 > extent_ids.size) return bad("a PlayGo extent list is outside its section");
        for (std::uint64_t e = 0; e < count; ++e)
            if (get_u32(d + extent_ids.off + list + e * 4) >= extent_count) return bad("a PlayGo chunk references a missing extent");
        p.chunks.push_back(std::move(c));
    }
    for (int i = 0; i < scenarios; ++i) {
        std::size_t at = scenario_attrs.off + static_cast<std::size_t>(i) * 0x20;
        PlayGoProject::Scenario s;
        s.initial_chunk_count = get_u16(d + at + 0x14);
        int count = get_u16(d + at + 0x16);
        std::uint32_t seq = get_u32(d + at + 0x18);
        if (s.initial_chunk_count > count || (seq & 1) || seq > scenario_chunks.size ||
            seq + static_cast<std::uint64_t>(count) * 2 > scenario_chunks.size)
            return bad("a PlayGo scenario is outside its section");
        std::set<int> seen;
        for (int k = 0; k < count; ++k) {
            int c = get_u16(d + scenario_chunks.off + seq + static_cast<std::size_t>(k) * 2);
            if (c >= chunks || !seen.insert(c).second) return bad("a PlayGo scenario references a bad chunk");
            s.chunks.push_back(c);
        }
        if (s.chunks.empty()) return bad("a PlayGo scenario is empty");
        ALEMBIC_ASSIGN(s.label, label(scenario_labels, get_u32(d + at + 0x1C)));
        p.scenarios.push_back(std::move(s));
    }
    return p;
}

PlayGoFiles build_playgo(const std::string& content_id, const PlayGoProject& project, const InnerImage& inner,
                         std::uint64_t fih_pfs_offset, std::uint64_t fih_cnt_offset) {
    PlayGoFiles out;
    auto chunk = build_chunk(content_id, project, inner, fih_pfs_offset, fih_cnt_offset);
    if (!chunk.ok()) {
        out.fell_back = true;
        out.warning = chunk.error().message;
        chunk = build_chunk(content_id, default_playgo_project(), inner, fih_pfs_offset, fih_cnt_offset);
    }
    if (chunk.ok()) out.chunk = std::move(*chunk);

    // Hash table and ficm: every regular file, by path hash (all in chunk 0).
    std::vector<std::uint64_t> hashes;
    for (const MetaNode& n : inner.nodes)
        if (!n.is_directory && !n.full_path.empty()) hashes.push_back(inner_path_hash(normalize_path(n.full_path)));
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    out.hash_table.assign(0x38 + hashes.size() * 8, 0);
    put_u32(&out.hash_table[0], 1);
    put_u32(&out.hash_table[4], 0x08000000);
    put_u32(&out.hash_table[8], 0x38);
    put_u32(&out.hash_table[0xC], static_cast<std::uint32_t>(hashes.size() * 8));
    put_u32(&out.hash_table[0x18], 0x544C467F);
    put_u32(&out.hash_table[0x24], static_cast<std::uint32_t>(hashes.size()));
    put_u64(&out.hash_table[0x28], 0x92CA8AAB26A24F51ULL);
    put_u64(&out.hash_table[0x30], 0x09BBB761A41BC44DULL);
    for (std::size_t i = 0; i < hashes.size(); ++i) put_u64(&out.hash_table[0x38 + i * 8], hashes[i]);
    out.ficm.assign(0x10 + hashes.size() * 2, 0);
    put_u32(&out.ficm[0], 1);
    put_u32(&out.ficm[8], 0x10);
    put_u32(&out.ficm[0xC], static_cast<std::uint32_t>(hashes.size() * 2));
    return out;
}

} // namespace alembic::fpkg::build

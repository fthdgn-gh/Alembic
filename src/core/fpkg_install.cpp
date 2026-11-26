// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/fpkg_install.hpp"

#include "alembic/bytes.hpp"
#include "alembic/crypto.hpp"
#include "alembic/fpkg_build.hpp"
#include "alembic/json.hpp"

#include <cstdio>
#include <cstring>

namespace alembic::fpkg::install {
namespace {

Error bad(const std::string& message) { return make_error(Errc::invalid_data, message); }

std::string hex_upper(const std::uint8_t* p, std::size_t n) {
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 15]);
    }
    return s;
}

// JSON string contents as the console's installer writes them: '/' escaped too.
std::string json_escape(const std::string& v) {
    std::string out;
    for (char c : v) {
        auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\' || c == '/') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", u);
            out += b;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void append(std::vector<std::uint8_t>& out, const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }

} // namespace

Result<std::vector<std::pair<std::string, std::vector<std::uint8_t>>>> read_stored_zip(Bytes zip) {
    // The end-of-central-directory record: the last 22 bytes (no comment).
    if (zip.size() < 22) return bad("the SI is too short for a ZIP");
    const std::uint8_t* end = zip.data() + zip.size() - 22;
    if (get_u32(end) != 0x06054b50) return bad("the SI has no ZIP end record");
    std::uint16_t count = get_u16(end + 10);
    std::uint32_t cd_size = get_u32(end + 12), cd_offset = get_u32(end + 16);
    if (static_cast<std::uint64_t>(cd_offset) + cd_size > zip.size() - 22) return bad("the SI's ZIP directory is outside it");
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> members;
    std::size_t at = cd_offset;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (at + 46 > cd_offset + cd_size || get_u32(zip.data() + at) != 0x02014b50) return bad("a broken ZIP directory entry in the SI");
        const std::uint8_t* e = zip.data() + at;
        std::uint16_t method = get_u16(e + 10), name_len = get_u16(e + 28), extra = get_u16(e + 30), comment = get_u16(e + 32);
        std::uint32_t size = get_u32(e + 20), stored = get_u32(e + 24), local = get_u32(e + 42);
        if (method != 0 || size != stored) return bad("the SI has a compressed member");
        if (at + 46 + name_len > cd_offset + cd_size) return bad("a ZIP name is outside the SI's directory");
        std::string name(reinterpret_cast<const char*>(e + 46), name_len);
        if (static_cast<std::uint64_t>(local) + 30 > zip.size() || get_u32(zip.data() + local) != 0x04034b50)
            return bad("a ZIP member of the SI has no local header");
        std::uint64_t data = local + 30ull + get_u16(zip.data() + local + 26) + get_u16(zip.data() + local + 28);
        if (data + size > zip.size()) return bad("a ZIP member is outside the SI");
        members.push_back({std::move(name), std::vector<std::uint8_t>(zip.begin() + static_cast<std::ptrdiff_t>(data),
                                                                       zip.begin() + static_cast<std::ptrdiff_t>(data + size))});
        at += 46u + name_len + extra + comment;
    }
    return members;
}

Result<Inputs> make_inputs(Bytes head, Bytes cnt_header, std::uint64_t app_pkg_size, Bytes param_json, Bytes si) {
    if (head.size() < 0x100 || std::memcmp(head.data(), "\x7F" "FIH", 4) != 0) return bad("app.pkg does not start with a FIH header");
    if (cnt_header.size() < 0x1000 || std::memcmp(cnt_header.data(), "\x7F" "CNT", 4) != 0) return bad("no CNT header");
    Inputs in;
    in.retail_signed = head[5] == 0x80;
    in.app_pkg_size = app_pkg_size;
    in.content_id.assign(reinterpret_cast<const char*>(cnt_header.data() + 0x40), 36);
    if (in.content_id.size() != 36 || in.content_id[6] != '-' || in.content_id[16] != '_')
        return bad("the CNT header's content ID is not one");
    in.title_id = in.content_id.substr(7, 9);
    std::memcpy(in.package_digest, cnt_header.data() + 0xFE0, 32);
    ALEMBIC_ASSIGN(auto props, json::top_level_properties(
                                   std::string_view(reinterpret_cast<const char*>(param_json.data()), param_json.size())));
    std::string content_version;
    for (const auto& p : props) {
        if (p.name == "masterVersion" && p.value) in.master_version = *p.value;
        if (p.name == "contentVersion" && p.value) content_version = *p.value;
    }
    // Without masterVersion: contentVersion's first part ("01.000.002" ->
    // "01.00"; a guess, every package seen has masterVersion).
    if (in.master_version.empty() && content_version.size() >= 5) in.master_version = content_version.substr(0, 5);
    if (in.master_version.size() != 5) return bad("param.json has no masterVersion like 01.00");
    ALEMBIC_ASSIGN(auto members, read_stored_zip(si));
    for (auto& [name, data] : members) {
        if (name == "config/" + in.content_id + "/playgo-chunk.crc") in.playgo_crc = std::move(data);
        else if (name == "common/etc/playgo-chunk.dat") in.playgo_chunk = std::move(data);
    }
    if (in.playgo_crc.empty() || in.playgo_chunk.empty()) return bad("the SI has no playgo-chunk.crc or playgo-chunk.dat");
    return in;
}

Result<Files> make_files(const Inputs& in, const std::string& url) {
    Files f;
    f.app_crc = in.playgo_crc;
    const crypto::Digest crc_sha = crypto::sha256(Bytes(in.playgo_crc.data(), in.playgo_crc.size()));
    const std::string size = std::to_string(in.app_pkg_size);

    // app.json: the keys in the order the installer writes them.
    std::string j = std::string("{\"fake\":true,\"numberOfSplitFiles\":1,\"originalFileSize\":") + size +
                    ",\"packageDigest\":\"" + hex_upper(in.package_digest, 32) + "\",\"pieces\":[{\"fileOffset\":0,\"fileSize\":" +
                    size + ",\"url\":\"" + json_escape(url) + "\"}],\"playgoChunkCrcHashValue\":\"" +
                    hex_upper(crc_sha.data(), crc_sha.size()) + "\"}";
    append(f.app_json, j);

    // app.pbm: a 0x100-byte header, one bit per 64 KiB block of app.pkg (all
    // there, most significant bit first), then SHA-256 of all before it.
    const std::uint64_t blocks = div_round_up(in.app_pkg_size, 0x10000);
    const std::uint64_t bitmap = div_round_up(blocks, 8);
    if (bitmap > 0xFFFFFFFFull) return bad("app.pkg is too large");
    std::vector<std::uint8_t> pbm(0x100 + bitmap, 0);
    std::memcpy(pbm.data(), "pdbm", 4);
    std::memcpy(pbm.data() + 4, in.title_id.data(), 9);       // NUL ended at 0x0D
    std::memcpy(pbm.data() + 0x0E, in.master_version.data(), 5); // NUL ended at 0x13
    put_u32(&pbm[0x14], 0x990);
    put_u32(&pbm[0x18], 0x10000);
    put_u32(&pbm[0x1C], in.retail_signed ? 0x310000 : 0x110000);
    put_u64(&pbm[0x20], in.app_pkg_size);
    std::memcpy(&pbm[0x60], in.package_digest, 32);
    put_u32(&pbm[0xA0], 1);
    put_u32(&pbm[0xA8], 0x100);
    put_u32(&pbm[0xAC], static_cast<std::uint32_t>(bitmap));
    std::memset(&pbm[0x100], 0xFF, static_cast<std::size_t>(bitmap));
    if (unsigned rest = static_cast<unsigned>(blocks % 8)) pbm[0x100 + bitmap - 1] = static_cast<std::uint8_t>(0xFF00u >> rest);
    const crypto::Digest pbm_sha = crypto::sha256(Bytes(pbm.data(), pbm.size()));
    pbm.insert(pbm.end(), pbm_sha.begin(), pbm_sha.end());
    f.app_pbm = std::move(pbm);

    // app.xml: every chunk there (locus 3). Consecutive chunks of the same
    // scenarios share one <chunks> element; its language_mask is the union of
    // the language-specific chunks' masks (those that are not all ones), or
    // all ones when every chunk is for every language. Seen: a package whose
    // chunks each name one of 31 languages, plus two for all, gets
    // 0xfffffffe00000000; one-chunk packages get all ones. The grouping by
    // scenario is a guess: every package seen has a single scenario.
    ALEMBIC_ASSIGN(build::PlayGoProject project, build::parse_playgo_chunk(in.playgo_chunk));
    std::vector<std::uint64_t> scenario_mask(project.chunks.size(), 0);
    for (std::size_t s = 0; s < project.scenarios.size() && s < 64; ++s)
        for (int c : project.scenarios[s].chunks)
            if (c >= 0 && static_cast<std::size_t>(c) < scenario_mask.size()) scenario_mask[c] |= 1ull << s;
    auto languages = [&](std::size_t from, std::size_t to) {
        std::uint64_t mask = 0;
        for (std::size_t i = from; i < to; ++i)
            if (project.chunks[i].language_mask != ~0ull) mask |= project.chunks[i].language_mask;
        return mask ? mask : ~0ull;
    };
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<psproject fmt=\"playgo-status\" version=\"2000\"><chunk_status chunk_count=\"" +
                    std::to_string(project.chunks.size()) + "\">";
    for (std::size_t i = 0; i < project.chunks.size(); ++i) {
        if (i == 0 || scenario_mask[i] != scenario_mask[i - 1]) {
            std::size_t end = i;
            while (end < project.chunks.size() && scenario_mask[end] == scenario_mask[i]) ++end;
            if (i) x += "</chunks>";
            x += "<chunks language_mask=\"" + std::to_string(languages(i, end)) + "\" scenario_mask=\"" +
                 std::to_string(scenario_mask[i]) + "\">";
        }
        x += "<chunk id=\"" + std::to_string(i) + "\" locus=\"3\" req_locus=\"3\"/>";
    }
    x += "</chunks><scenarios>";
    for (const auto& s : project.scenarios) {
        x += "<scenario>";
        for (std::size_t k = 0; k < s.chunks.size(); ++k) x += (k ? " " : "") + std::to_string(s.chunks[k]);
        x += "</scenario>";
    }
    x += "</scenarios></chunk_status></psproject>";
    append(f.app_xml, x);
    return f;
}

} // namespace alembic::fpkg::install

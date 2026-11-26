// SPDX-License-Identifier: GPL-3.0-or-later
// A PS5 debug package in the form the console's installer leaves it (NOTES,
// 2026-11-26: "installing without the extra copies"), so that an image can be
// written straight to where an install keeps it and then registered with
// sceAppInstUtilAppInstallPkg on its own app.pkg:
//
//   /user/app/<TITLE>/app.pkg          the package up to its SI segment
//   /user/app/<TITLE>/app.json         the installer's download record
//   /user/app/<TITLE>/app.pbm (+.backup)  which 64 KiB blocks of app.pkg are there
//   /user/app/<TITLE>/app.xml          PlayGo's chunk status
//   /user/app/<TITLE>/app.crc          the SI's config/<CID>/playgo-chunk.crc
//   /user/playgo/app/<CID>/00/app.si   the SI segment (a stored ZIP)
//
// Not made here: PlayGo's /user/playgo/app/<CID>/00/{d0,d1,f0}.pdb (state the
// console rewrites itself once they exist; not decoded yet) and config/<CID>/.
// Every format here was learned from installs on a console (no source).
#pragma once

#include "alembic/io.hpp"
#include "alembic/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alembic::fpkg::install {

// What the files are made of, read from app.pkg and the SI.
struct Inputs {
    std::string content_id;      // 36 characters
    std::string title_id;        // its characters 7..15
    std::string master_version;  // param.json's masterVersion ("01.00")
    std::uint64_t app_pkg_size = 0;
    std::uint8_t package_digest[32] = {}; // the CNT header's, at CNT + 0xFE0
    bool retail_signed = false;  // FIH byte 5 is 0x80 (packages from some other tools)
    std::vector<std::uint8_t> playgo_crc;   // the SI's config/<CID>/playgo-chunk.crc
    std::vector<std::uint8_t> playgo_chunk; // the SI's common/etc/playgo-chunk.dat
};

struct Files {
    std::vector<std::uint8_t> app_json, app_pbm, app_xml, app_crc;
};

// The members of a stored ZIP (what Alembic and ProsperoPkgTool write as the SI).
Result<std::vector<std::pair<std::string, std::vector<std::uint8_t>>>> read_stored_zip(Bytes zip);

// `head`: the first bytes of app.pkg (at least the FIH, 64 KiB); `cnt_header`:
// the CNT's first 4 KiB; `param_json`: the package's param.json; `si`: the SI.
Result<Inputs> make_inputs(Bytes head, Bytes cnt_header, std::uint64_t app_pkg_size, Bytes param_json, Bytes si);

// `url`: what app.json says the package was read from (the installer writes
// its download URL there; any value).
Result<Files> make_files(const Inputs& in, const std::string& url);

} // namespace alembic::fpkg::install

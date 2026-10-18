// SPDX-License-Identifier: GPL-3.0-or-later
// Which sce_sys files a package keeps as outer CNT entries. Ported from
// pearlxcore/ProsperoPkgTool (MIT) Content/ProsperoSceSysMedia.cs (BuildNameMap,
// IsOuterCntFile) with the ids of Containers/ProsperoCntBuilder.cs (CntEntryId).
#include "alembic/fpkg_build.hpp"

#include <cstdio>
#include <map>

namespace alembic::fpkg::build {

namespace {

const std::map<std::string, std::uint32_t>& name_map() {
    static const std::map<std::string, std::uint32_t> map = [] {
        std::map<std::string, std::uint32_t> m = {
            {".digests", 0x0001}, {".entry_keys", 0x0010}, {".image_key", 0x0020}, {".general_digests", 0x0080},
            {".metas", 0x0100}, {".entry_names", 0x0200}, {"license.dat", 0x0400}, {"license.info", 0x0401},
            {"nptitle.dat", 0x0402}, {"npbind.dat", 0x0403}, {"selfinfo.dat", 0x0404}, {"imageinfo.dat", 0x0406},
            {"target-deltainfo.dat", 0x0407}, {"origin-deltainfo.dat", 0x0408}, {"psreserved.dat", 0x0409},
            {"param.sfo", 0x1000}, {"playgo-chunk.dat", 0x1001}, {"playgo-chunk.sha", 0x1002},
            {"playgo-manifest.xml", 0x1003}, {"pronunciation.sig", 0x1005}, {"pronunciation.xml", 0x1004},
            {"pic1.png", 0x1006}, {"pubtoolinfo.dat", 0x1007}, {"app/playgo-chunk.dat", 0x1008},
            {"app/playgo-chunk.sha", 0x1009}, {"app/playgo-manifest.xml", 0x100A}, {"shareparam.json", 0x100B},
            {"shareoverlayimage.png", 0x100C}, {"save_data.png", 0x100D}, {"shareprivacyguardimage.png", 0x100E},
            {"icon0.png", 0x1200}, {"pic0.png", 0x1220}, {"snd0.at9", 0x1240}, {"changeinfo/changeinfo.xml", 0x1260},
            {"icon0.dds", 0x1280}, {"pic0.dds", 0x12A0}, {"pic1.dds", 0x12C0}, {"trophy2/trophy00.ucp", 0x1480},
            {"uds/uds00.ucp", 0x14A0}, {"uds/npbind.dat", 0x2020}, {"trophy2/npbind.dat", 0x2021},
            {"pic2.png", 0x2040}, {"pic2.dds", 0x2060},
        };
        char name[64];
        for (int i = 0; i < 31; ++i) {
            std::snprintf(name, sizeof name, "icon0_%02d.png", i);
            m[name] = 4609 + i;
            std::snprintf(name, sizeof name, "icon0_%02d.dds", i);
            m[name] = 4737 + i;
            std::snprintf(name, sizeof name, "pic1_%02d.png", i);
            m[name] = 4673 + i;
            std::snprintf(name, sizeof name, "pic1_%02d.dds", i);
            m[name] = 4801 + i;
            std::snprintf(name, sizeof name, "changeinfo/changeinfo_%02d.xml", i);
            m[name] = 4705 + i;
            if (i < 10) {
                std::snprintf(name, sizeof name, "keymap_rp/%02d.png", i + 1);
                m[name] = 5632 + i;
            }
            for (int j = 0; j < 10; ++j) {
                std::snprintf(name, sizeof name, "keymap_rp/%02d/%02d.png", i, j + 1);
                m[name] = static_cast<std::uint32_t>(5648 + 16 * i + j);
            }
        }
        for (int k = 0; k < 100; ++k) {
            std::snprintf(name, sizeof name, "trophy/trophy%02d.trp", k);
            m[name] = static_cast<std::uint32_t>(5120 + k);
        }
        return m;
    }();
    return map;
}

} // namespace

std::optional<std::uint32_t> sce_sys_entry_id(const std::string& rel) {
    auto it = name_map().find(rel);
    if (it == name_map().end()) return std::nullopt;
    return it->second;
}

bool is_outer_cnt_file(const std::string& rel) {
    return rel == "param.json" || rel == "pic2.png" || name_map().count(rel) != 0;
}

} // namespace alembic::fpkg::build

// SPDX-License-Identifier: GPL-3.0-or-later
#include "deps.hpp"
#include "platform.hpp"

#include "alembic/io.hpp"
#include "alembic/sha256.hpp"


namespace alembic::engine {

namespace {

std::string sha256_hex(Bytes data) {
    Sha256 h;
    h.update(data);
    return Sha256::hex(h.finish());
}

// "" when missing, else the file's SHA-256.
std::string file_hash(const std::string& path) {
    auto file = File::open(path, File::Mode::read);
    if (!file.ok()) return "";
    auto size = file->size();
    if (!size.ok() || *size > 64u * 1024 * 1024) return "?";
    std::vector<std::uint8_t> data(static_cast<std::size_t>(*size));
    if (!file->read_at(0, data).ok()) return "?";
    return sha256_hex(data);
}

const std::string kPptCommit = "f2c91cd7a5d8440b7b9ae5e0b1154334093bf613";

// maxton/LibOrbisPkg: Keys.cs, the PS4 package keys (fpkg_ps4.hpp kKeysFileSha256).
const std::string kLibOrbisPkgCommit = "643477263b2644e0803e0f58b8726ea4e3f3b7d4";

// Which AMPR emulator was chosen last (its id).
std::string in_use_file(const std::string& state_dir) {
    return state_dir + "/runtime/in-use";
}

std::string ppt_key(const std::string& name) {
    return "https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/" + kPptCommit + "/ProsperoPkgTool/Keys/" + name;
}

} // namespace

const std::vector<Dependency>& dependencies() {
    static const std::vector<Dependency> list = {
        {
            "ampr_runtime",
            "AMPR emulator (ampr_emu)",
            "Stands in for the console's APR file reader (fakelib/libSceAmpr.sprx), so games that use it start from "
            "an image or folder; also reads LZ4 packs. Added to such games when converting, or to an existing game.",
            "drakmor/ampr_emu release 0.4.2.1 (official release asset)",
            "GPL-3.0",
            {{"https://github.com/drakmor/ampr_emu/releases/download/0.4.2.1/libSceAmpr.sprx",
              "69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a", 423350, "runtime/libSceAmpr.sprx"}},
            kAmprRuntimeGroup,
            "ampr_emu 0.4.2.1",
        },
        {
            "ampr_runtime_ng",
            "AMPR emulator (ampr-emu-ng)",
            "A rewrite of ampr_emu that reads faster and uses less memory; reads the same LZ4 packs. Conversions add "
            "the AMPR emulator marked “in use”. ShadowMount+ swaps a game's libSceAmpr.sprx for the one in its emulators folder "
            "while “update emulators” is on: turn that off if a game should keep this one.",
            "quer3q/ampr-emu-ng release v0.1.0 (official release asset)",
            "GPL-3.0",
            {{"https://github.com/quer3q/ampr-emu-ng/releases/download/v0.1.0/libSceAmpr.sprx",
              "69ee3b420c6798fa1b5fb5a9fed75479403bc8a64fe382db301477b1bf705956", 667408,
              "runtime/ampr-emu-ng/libSceAmpr.sprx"}},
            kAmprRuntimeGroup,
            "ampr-emu-ng 0.1.0",
        },
        {
            "fpkg_keys",
            "FPKG key files",
            "The public RSA-3072 moduli a debug FPKG is built with (passcode key wraps, image key wrap, metadata "
            "header). Needed for PKG output.",
            "pearlxcore/ProsperoPkgTool, commit " + kPptCommit.substr(0, 12) +
                " (Keys/). That project describes them as public constants from Sony's publishing tools.",
            "distributed by ProsperoPkgTool (MIT); not part of Alembic",
            {
                {ppt_key("passcode.bin"), "81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e", 2688,
                 "keys/passcode.bin"},
                {ppt_key("mount_image.bin"), "4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0", 384,
                 "keys/mount_image.bin"},
                {ppt_key("metadata_modulus.bin"), "4a585ee47fea88d6b121d5e8afc9829438a27ee325cebec56a2ff537129a37c5",
                 384, "keys/metadata_modulus.bin"},
            },
            "",
            "",
        },
        {
            "ps4_keys",
            "Licence and PS4 package keys",
            "The debug licence key, for packages of games that have no licence (without one, the title shows a lock), "
            "and the keys a PS4 fake package is built with (Sony's public package keys, the fake image key's modulus). "
            "Needed for PKG output.",
            "maxton/LibOrbisPkg, commit " + kLibOrbisPkgCommit.substr(0, 12) +
                " (LibOrbisPkg/Util/Keys.cs). That project collected them from the PS4 and its SDK tools.",
            "distributed by LibOrbisPkg (LGPL-3.0); not part of Alembic",
            {{"https://raw.githubusercontent.com/maxton/LibOrbisPkg/" + kLibOrbisPkgCommit + "/LibOrbisPkg/Util/Keys.cs",
              "48b44dd03e653a5c0991b0a2975477802ea2639c4e6ea3270b2ff20a1856f8a7", 38180, "keys/ps4/Keys.cs"}},
            "",
            "",
        },
    };
    return list;
}

const Dependency* find_dependency(const std::string& id) {
    for (const Dependency& d : dependencies())
        if (d.id == id) return &d;
    return nullptr;
}

std::string dependency_state(const Dependency& dep, const std::string& state_dir) {
    std::size_t good = 0, missing = 0;
    for (const DependencyFile& f : dep.files) {
        std::string hash = file_hash(state_dir + "/" + f.path);
        if (hash.empty()) ++missing;
        else if (hash == f.sha256) ++good;
    }
    if (good == dep.files.size()) return "installed";
    if (missing == dep.files.size()) return "missing";
    if (good + missing == dep.files.size()) return "partial";
    return "modified";
}

JsonValue dependency_json(const Dependency& dep, const std::string& state_dir) {
    JsonValue v = JsonValue::object();
    v.set("id", dep.id);
    v.set("label", dep.label);
    v.set("purpose", dep.purpose);
    v.set("source", dep.source);
    v.set("license", dep.license);
    v.set("state", dependency_state(dep, state_dir));
    JsonValue files = JsonValue::array();
    std::uint64_t total = 0;
    for (const DependencyFile& f : dep.files) {
        JsonValue fj = JsonValue::object();
        fj.set("path", state_dir + "/" + f.path);
        fj.set("url", f.url);
        fj.set("sha256", f.sha256);
        fj.set("size", static_cast<std::uint64_t>(f.size));
        files.push(std::move(fj));
        total += f.size;
    }
    v.set("files", std::move(files));
    v.set("bytes", total);
    if (!dep.group.empty()) {
        v.set("group", dep.group);
        v.set("in_use", ampr_runtime_in_use(state_dir) == &dep);
    }
    return v;
}

Status install_dependency(const Dependency& dep, const std::string& state_dir) {
    // Download and check everything before writing anything.
    std::vector<std::vector<std::uint8_t>> contents;
    for (const DependencyFile& f : dep.files) {
        ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, platform::http_get(f.url, f.size + 1));
        std::string hash = sha256_hex(data);
        if (hash != f.sha256)
            return make_error(Errc::invalid_data, "the download of " + f.path + " has an unexpected SHA-256 (" + hash +
                                                      "); nothing was written");
        contents.push_back(std::move(data));
    }
    for (std::size_t i = 0; i < dep.files.size(); ++i) {
        std::string path = state_dir + "/" + dep.files[i].path;
        ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, true));
        ALEMBIC_TRY(out.file().write_at(0, contents[i]));
        ALEMBIC_TRY(out.commit());
    }
    if (dep.group == kAmprRuntimeGroup) ALEMBIC_TRY(use_dependency(dep, state_dir));
    return {};
}

Status remove_dependency(const Dependency& dep, const std::string& state_dir) {
    for (const DependencyFile& f : dep.files) {
        std::string path = state_dir + "/" + f.path;
        if (path_exists(path)) ALEMBIC_TRY(remove_file(path));
    }
    return {};
}

const Dependency* ampr_runtime_in_use(const std::string& state_dir) {
    std::string chosen;
    if (auto file = File::open(in_use_file(state_dir), File::Mode::read); file.ok()) {
        auto size = file->size();
        std::vector<std::uint8_t> data(size.ok() && *size <= 64 ? static_cast<std::size_t>(*size) : 0);
        if (!data.empty() && file->read_at(0, data).ok()) chosen.assign(data.begin(), data.end());
    }
    const Dependency* first = nullptr;
    for (const Dependency& d : dependencies()) {
        if (d.group != kAmprRuntimeGroup || !is_regular_file(state_dir + "/" + d.files[0].path)) continue;
        if (d.id == chosen) return &d;
        if (!first) first = &d;
    }
    return first;
}

Status use_dependency(const Dependency& dep, const std::string& state_dir) {
    if (dep.group != kAmprRuntimeGroup) return make_error(Errc::invalid_argument, dep.label + " is not an AMPR emulator");
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(in_use_file(state_dir), true));
    ALEMBIC_TRY(out.file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(dep.id.data()), dep.id.size())));
    return out.commit();
}

const Dependency* ampr_runtime_by_hash(const std::string& sha256) {
    for (const Dependency& d : dependencies())
        if (d.group == kAmprRuntimeGroup && d.files[0].sha256 == sha256) return &d;
    return nullptr;
}

} // namespace alembic::engine

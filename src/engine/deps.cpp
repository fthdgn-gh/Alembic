// SPDX-License-Identifier: GPL-3.0-or-later
#include "deps.hpp"
#include "platform.hpp"

#include "alembic/io.hpp"
#include "alembic/sha256.hpp"

#include <unistd.h>

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
    return {};
}

Status remove_dependency(const Dependency& dep, const std::string& state_dir) {
    for (const DependencyFile& f : dep.files) {
        std::string path = state_dir + "/" + f.path;
        if (path_exists(path) && ::unlink(path.c_str()) != 0)
            return make_error(Errc::io, "cannot delete " + path);
    }
    return {};
}

} // namespace alembic::engine

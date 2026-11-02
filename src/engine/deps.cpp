// SPDX-License-Identifier: GPL-3.0-or-later
#include "deps.hpp"
#include "jobs.hpp"
#include "platform.hpp"
#include "update.hpp"

#include "alembic/io.hpp"
#include "alembic/sha256.hpp"

#include <atomic>
#include <map>
#include <mutex>

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

// GitHub's latest release of a followed dependency, from the last check.
struct Latest {
    std::string version; // without a leading "v"
    std::string url;
    std::string sha256;  // GitHub's digest of the asset ("" when it gives none)
    std::uint64_t size = 0;
    std::string sums_url; // the release's SHA256SUMS ("" when it has none)
    std::string error;    // why the last check failed
    std::int64_t checked = 0;
};

std::mutex g_mutex; // components.json and g_latest
std::map<std::string, Latest> g_latest;
std::atomic<bool> g_checking{false};

std::string strip_v(const std::string& v) { return !v.empty() && (v[0] == 'v' || v[0] == 'V') ? v.substr(1) : v; }

std::string components_file(const std::string& state_dir) { return state_dir + "/components.json"; }

// {"auto_update": bool, "installed": {id: {version, sha256, size, url}},
//  "known": [{id, version, sha256}]}; callers hold g_mutex.
JsonValue load_components(const std::string& state_dir) {
    auto file = File::open(components_file(state_dir), File::Mode::read);
    if (file.ok()) {
        auto size = file->size();
        if (size.ok() && *size <= 1u << 20) {
            std::vector<std::uint8_t> data(static_cast<std::size_t>(*size));
            if (file->read_at(0, data).ok()) {
                auto v = parse_json(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
                if (v.ok() && v->is_object()) return *v;
            }
        }
    }
    return JsonValue::object();
}

Status save_components(const std::string& state_dir, const JsonValue& v) {
    std::string text = v.dump();
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(components_file(state_dir), true));
    ALEMBIC_TRY(out.file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    return out.commit();
}

// What was downloaded for a followed release ({} when nothing is recorded).
JsonValue installed_record(const Dependency& dep, const std::string& state_dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return load_components(state_dir)["installed"][dep.id];
}

// The SHA-256 the file of a followed release should have, and its version.
std::pair<std::string, std::string> expected_build(const Dependency& dep, const std::string& state_dir) {
    JsonValue rec = installed_record(dep, state_dir);
    if (!rec["sha256"].str().empty()) return {rec["sha256"].str(), rec["version"].str()};
    return {dep.files[0].sha256, dep.fallback_version};
}

// Asks GitHub for the latest release of `dep`.
Latest fetch_latest(const Dependency& dep) {
    Latest l;
    l.checked = unix_now();
    auto body = platform::http_get("https://api.github.com/repos/" + dep.feed.repo + "/releases/latest", 1u << 20);
    if (!body.ok()) {
        l.error = body.error().message;
        return l;
    }
    auto v = parse_json(std::string_view(reinterpret_cast<const char*>(body->data()), body->size()));
    if (!v.ok()) {
        l.error = "GitHub's answer is not readable";
        return l;
    }
    l.version = strip_v((*v)["tag_name"].str());
    for (const JsonValue& a : (*v)["assets"].items()) {
        std::string name = a["name"].str();
        if (name == "SHA256SUMS") l.sums_url = a["browser_download_url"].str();
        if (name != dep.feed.asset) continue;
        l.url = a["browser_download_url"].str();
        l.size = static_cast<std::uint64_t>(a["size"].integer(0));
        std::string digest = a["digest"].str();
        if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) l.sha256 = digest.substr(7);
    }
    if (l.version.empty()) l.error = "the latest release has no version";
    else if (l.url.empty()) l.error = "the latest release " + l.version + " has no " + dep.feed.asset;
    return l;
}

// A newer build than the one downloaded (by version, else by hash).
bool is_update(const Latest& l, const std::string& version, const std::string& sha256) {
    if (!l.error.empty() || l.version.empty()) return false;
    int c = update::compare_versions(l.version, version);
    if (c != 0) return c > 0;
    return !l.sha256.empty() && l.sha256 != sha256;
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
            "drakmor/ampr_emu: its latest release (official release asset)",
            "GPL-3.0",
            {{"https://github.com/drakmor/ampr_emu/releases/download/0.4.2.1/libSceAmpr.sprx",
              "69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a", 423350, "runtime/libSceAmpr.sprx"}},
            kAmprRuntimeGroup,
            "ampr_emu",
            {"drakmor/ampr_emu", "libSceAmpr.sprx"},
            "0.4.2.1",
        },
        {
            "ampr_runtime_ng",
            "AMPR emulator (ampr-emu-ng)",
            "A rewrite of ampr_emu that reads faster and uses less memory; reads the same LZ4 packs and its own "
            "zstd packs. Conversions add the AMPR emulator marked “in use”. ShadowMount+ swaps a game's libSceAmpr.sprx for the one in its emulators folder "
            "while “update emulators” is on: turn that off if a game should keep this one.",
            "quer3q/ampr-emu-ng: its latest release (official release asset)",
            "GPL-3.0",
            {{"https://github.com/quer3q/ampr-emu-ng/releases/download/v0.2.0/libSceAmpr.sprx",
              "7acb2173d9691b5dfb94ea4007660920b7250111c5fd7a86c6eb91b763ad3d76", 678416,
              "runtime/ampr-emu-ng/libSceAmpr.sprx"}},
            kAmprRuntimeGroup,
            "ampr-emu-ng",
            {"quer3q/ampr-emu-ng", "libSceAmpr.sprx"},
            "0.2.0",
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
    if (!dep.feed.repo.empty()) {
        std::string hash = file_hash(state_dir + "/" + dep.files[0].path);
        if (hash.empty()) return "missing";
        return hash == expected_build(dep, state_dir).first ? "installed" : "modified";
    }
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
    const std::string state = dependency_state(dep, state_dir);
    v.set("state", state);
    JsonValue files = JsonValue::array();
    std::uint64_t total = 0;
    if (!dep.feed.repo.empty()) {
        // The build downloaded, else the one a download would get.
        auto [sha, version] = expected_build(dep, state_dir);
        JsonValue rec = installed_record(dep, state_dir);
        Latest latest;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_latest.find(dep.id);
            if (it != g_latest.end()) latest = it->second;
        }
        const bool have = state != "missing";
        JsonValue fj = JsonValue::object();
        fj.set("path", state_dir + "/" + dep.files[0].path);
        std::uint64_t size = dep.files[0].size;
        std::string url = dep.files[0].url;
        if (have && !rec["url"].str().empty()) {
            url = rec["url"].str();
            size = static_cast<std::uint64_t>(rec["size"].integer(0));
        } else if (!have && latest.error.empty() && !latest.url.empty()) {
            url = latest.url;
            sha = latest.sha256;
            size = latest.size;
            version = latest.version;
        }
        fj.set("url", url);
        fj.set("sha256", sha);
        fj.set("size", size);
        files.push(std::move(fj));
        total = size;
        v.set("follows", dep.feed.repo);
        v.set("version", have ? version : std::string());
        v.set("latest", latest.version);
        v.set("checked", latest.checked);
        v.set("check_error", latest.error);
        v.set("update", have && is_update(latest, version, sha));
    } else {
        for (const DependencyFile& f : dep.files) {
            JsonValue fj = JsonValue::object();
            fj.set("path", state_dir + "/" + f.path);
            fj.set("url", f.url);
            fj.set("sha256", f.sha256);
            fj.set("size", static_cast<std::uint64_t>(f.size));
            files.push(std::move(fj));
            total += f.size;
        }
    }
    v.set("files", std::move(files));
    v.set("bytes", total);
    if (!dep.group.empty()) {
        v.set("group", dep.group);
        v.set("in_use", ampr_runtime_in_use(state_dir) == &dep);
    }
    return v;
}

JsonValue dependencies_status_json(const std::string& state_dir) {
    JsonValue v = JsonValue::object();
    v.set("auto_update", auto_update_dependencies(state_dir));
    v.set("checking", dependency_check_running());
    return v;
}

namespace {

Status write_file(const std::string& path, const std::vector<std::uint8_t>& data) {
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, true));
    ALEMBIC_TRY(out.file().write_at(0, data));
    return out.commit();
}

// The latest build of a followed release, checked, or the fallback build
// when GitHub's API can't be reached.
Status install_release(const Dependency& dep, const std::string& state_dir) {
    Latest l = fetch_latest(dep);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest[dep.id] = l;
    }
    std::string url, sha, version;
    std::uint64_t size = 0;
    if (l.error.empty()) {
        url = l.url;
        sha = l.sha256;
        size = l.size;
        version = l.version;
    } else {
        url = dep.files[0].url;
        sha = dep.files[0].sha256;
        size = dep.files[0].size;
        version = dep.fallback_version;
    }
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, platform::http_get(url, size + 1));
    const std::string hash = sha256_hex(data);
    if (data.size() != size)
        return make_error(Errc::invalid_data, "the download of " + dep.files[0].path + " has an unexpected size; nothing was written");
    // GitHub's digest, and the release's own SHA256SUMS when it has one: at
    // least one of them must vouch for the file.
    std::string listed;
    if (l.error.empty() && !l.sums_url.empty()) {
        auto sums = platform::http_get(l.sums_url, 1u << 20);
        if (sums.ok()) listed = update::checksum_for(std::string(sums->begin(), sums->end()), dep.feed.asset);
    }
    if (sha.empty() && listed.empty() && hash != dep.files[0].sha256)
        return make_error(Errc::invalid_data, "the release gives no SHA-256 for " + dep.feed.asset + "; nothing was written");
    if ((!sha.empty() && hash != sha) || (!listed.empty() && hash != listed))
        return make_error(Errc::invalid_data, "the download of " + dep.files[0].path + " has an unexpected SHA-256 (" + hash +
                                                  "); nothing was written");
    ALEMBIC_TRY(write_file(state_dir + "/" + dep.files[0].path, data));
    std::lock_guard<std::mutex> lock(g_mutex);
    JsonValue c = load_components(state_dir);
    JsonValue installed = c["installed"].is_object() ? c["installed"] : JsonValue::object();
    JsonValue rec = JsonValue::object();
    rec.set("version", version);
    rec.set("sha256", hash);
    rec.set("size", static_cast<std::uint64_t>(data.size()));
    rec.set("url", url);
    rec.set("downloaded", unix_now());
    installed.set(dep.id, std::move(rec));
    c.set("installed", std::move(installed));
    // Remembered, so a game that got this build is still named after an update.
    JsonValue known = JsonValue::array();
    bool seen = false;
    for (const JsonValue& k : c["known"].items()) {
        if (k["sha256"].str() == hash) seen = true;
        known.push(k);
    }
    if (!seen) {
        JsonValue k = JsonValue::object();
        k.set("id", dep.id);
        k.set("version", version);
        k.set("sha256", hash);
        known.push(std::move(k));
    }
    c.set("known", std::move(known));
    return save_components(state_dir, c);
}

} // namespace

Status install_dependency(const Dependency& dep, const std::string& state_dir, bool make_in_use) {
    if (!dep.feed.repo.empty()) {
        ALEMBIC_TRY(install_release(dep, state_dir));
    } else {
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
        for (std::size_t i = 0; i < dep.files.size(); ++i)
            ALEMBIC_TRY(write_file(state_dir + "/" + dep.files[i].path, contents[i]));
    }
    if (make_in_use && dep.group == kAmprRuntimeGroup) ALEMBIC_TRY(use_dependency(dep, state_dir));
    return {};
}

Status remove_dependency(const Dependency& dep, const std::string& state_dir) {
    for (const DependencyFile& f : dep.files) {
        std::string path = state_dir + "/" + f.path;
        if (path_exists(path)) ALEMBIC_TRY(remove_file(path));
    }
    if (dep.feed.repo.empty()) return {};
    std::lock_guard<std::mutex> lock(g_mutex);
    JsonValue c = load_components(state_dir);
    if (!c["installed"].has(dep.id)) return {};
    JsonValue installed = JsonValue::object();
    for (const auto& [id, rec] : c["installed"].fields())
        if (id != dep.id) installed.set(id, rec);
    c.set("installed", std::move(installed));
    return save_components(state_dir, c);
}

void check_dependency_updates(const std::string& state_dir, bool apply, const std::function<void(const std::string&)>& log) {
    if (g_checking.exchange(true)) return;
    for (const Dependency& dep : dependencies()) {
        if (dep.feed.repo.empty()) continue;
        Latest l = fetch_latest(dep);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_latest[dep.id] = l;
        }
        if (!l.error.empty()) {
            if (log) log(dep.name + ": update check failed: " + l.error);
            continue;
        }
        if (dependency_state(dep, state_dir) != "installed") continue;
        auto [sha, version] = expected_build(dep, state_dir);
        if (!is_update(l, version, sha)) continue;
        if (log) log(dep.name + " " + l.version + " is out (" + version + " here)");
        if (!apply || !auto_update_dependencies(state_dir)) continue;
        Status s = install_dependency(dep, state_dir, false);
        if (log) log(s.ok() ? dep.name + " updated to " + l.version : dep.name + " update failed: " + s.error().message);
    }
    g_checking = false;
}

bool dependency_check_running() { return g_checking; }

bool auto_update_dependencies(const std::string& state_dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return load_components(state_dir)["auto_update"].boolean(true);
}

Status set_auto_update_dependencies(const std::string& state_dir, bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    JsonValue c = load_components(state_dir);
    c.set("auto_update", on);
    return save_components(state_dir, c);
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

std::optional<KnownBuild> ampr_runtime_by_hash(const std::string& sha256, const std::string& state_dir) {
    if (sha256.empty()) return std::nullopt;
    for (const Dependency& d : dependencies())
        if (d.group == kAmprRuntimeGroup && d.files[0].sha256 == sha256) return KnownBuild{&d, d.fallback_version};
    std::lock_guard<std::mutex> lock(g_mutex);
    JsonValue c = load_components(state_dir);
    for (const JsonValue& k : c["known"].items()) {
        if (k["sha256"].str() != sha256) continue;
        const Dependency* d = find_dependency(k["id"].str());
        if (d && d->group == kAmprRuntimeGroup) return KnownBuild{d, k["version"].str()};
    }
    return std::nullopt;
}

} // namespace alembic::engine

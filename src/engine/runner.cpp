// SPDX-License-Identifier: GPL-3.0-or-later
#include "runner.hpp"
#include "deps.hpp"
#include "fs_ops.hpp"
#include "platform.hpp"
#include "smp.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

#include "alembic/ampr_emu.hpp"
#include "alembic/convert.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/lz4pack.hpp"
#include "alembic/resume.hpp"

namespace alembic::engine {

namespace {

std::string strip_slashes(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string extension_for(convert::Format f) {
    switch (f) {
    case convert::Format::exfat: return ".exfat";
    case convert::Format::ffpfsc: return ".ffpfsc";
    case convert::Format::ffpkg: return ".ffpkg";
    case convert::Format::ffpfs: return ".ffpfs";
    case convert::Format::fpkg: return ".pkg";
    case convert::Format::folder: return "/";
    }
    return "";
}

Result<convert::Format> target_format(const JobSpec& spec) {
    if (!spec.to.empty()) {
        auto f = convert::parse_format(spec.to);
        if (!f) return make_error(Errc::invalid_argument, "to must be folder, exfat, ffpkg, ffpfs, ffpfsc or fpkg");
        return *f;
    }
    if (spec.output.empty()) return make_error(Errc::invalid_argument, "give an output path or a target format (to)");
    auto f = convert::guess_output_format(spec.output);
    if (!f) return make_error(Errc::invalid_argument, "cannot tell the output format from its name; set to");
    return *f;
}

} // namespace

std::size_t mount_name_limit(const std::string& output) {
    auto ends = [&](const char* ext) {
        std::size_t n = std::char_traits<char>::length(ext);
        if (output.size() < n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            char c = output[output.size() - n + i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            if (c != ext[i]) return false;
        }
        return true;
    };
    constexpr std::size_t kMountPointMax = 87; // MNAMELEN 88 with the NUL
    constexpr std::size_t kHash = 9;           // "_%08x"
    if (ends(".ffpfsc")) return kMountPointMax - std::char_traits<char>::length("/mnt/shadowmnt/pfsc/") - kHash; // 58
    if (ends(".exfat") || ends(".ffpkg") || ends(".ffpfs"))
        return kMountPointMax - std::char_traits<char>::length("/mnt/shadowmnt/") - kHash; // 63
    return 0;
}

std::string mount_name_warning(const std::string& output) {
    std::size_t limit = mount_name_limit(output);
    if (!limit) return "";
    std::string name = output.substr(output.rfind('/') + 1);
    std::size_t dot = name.rfind('.');
    std::size_t length = dot == std::string::npos ? name.size() : dot;
    if (length <= limit) return "";
    return "ShadowMount+ cannot mount this image: its name has " + std::to_string(length) +
           " characters before the extension, at most " + std::to_string(limit) + " work";
}

std::string shorten_name(const std::string& name, std::size_t limit) {
    if (name.size() <= limit) return name;
    auto is_alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string title, version, tag;
    for (std::size_t i = 0; i + 9 <= name.size() && title.empty(); ++i) {
        bool ok = (i == 0 || !is_alpha(name[i - 1]));
        for (std::size_t k = 0; ok && k < 9; ++k) ok = k < 4 ? is_alpha(name[i + k]) : is_digit(name[i + k]);
        if (ok && (i + 9 == name.size() || !is_digit(name[i + 9]))) title = name.substr(i, 9);
    }
    // -A0100-V0100 (app and version numbers in package names)
    for (std::size_t i = 0; i + 12 <= name.size(); ++i) {
        if (name[i] == '-' && name[i + 1] == 'A' && name[i + 6] == '-' && name[i + 7] == 'V') {
            bool ok = true;
            for (std::size_t k : {2u, 3u, 4u, 5u, 8u, 9u, 10u, 11u}) ok = ok && is_digit(name[i + k]);
            if (ok) version = name.substr(i, 12);
        }
    }
    for (const char* t : {"-lz4", "-unpacked"}) {
        std::size_t n = std::char_traits<char>::length(t);
        if (name.size() >= n && name.compare(name.size() - n, n, t) == 0) tag = t;
    }
    std::string candidate = title.empty() ? "" : title + version + tag;
    if (!candidate.empty() && candidate.size() <= limit) return candidate;
    // Keep the tag, cut the rest.
    std::string body = name.substr(0, name.size() - tag.size());
    std::size_t keep = limit > tag.size() ? limit - tag.size() : limit;
    return body.substr(0, keep) + (limit > tag.size() ? tag : "");
}

Result<std::string> resolve_target(const JobSpec& spec) {
    if (spec.kind != "convert") return std::string();
    ALEMBIC_ASSIGN(convert::Format f, target_format(spec));
    switch (f) {
    case convert::Format::folder: return std::string("folder");
    case convert::Format::exfat: return std::string("exfat");
    case convert::Format::ffpkg: return std::string("ffpkg");
    case convert::Format::ffpfs: return std::string("ffpfs");
    case convert::Format::fpkg: return std::string("fpkg");
    default: return std::string("ffpfsc");
    }
}

Result<std::string> resolve_output(const JobSpec& spec) {
    if (!spec.output.empty() || spec.kind != "convert") return spec.output;
    ALEMBIC_ASSIGN(convert::Format to, target_format(spec));
    std::string input = strip_slashes(spec.input);
    std::size_t slash = input.rfind('/');
    std::string dir = slash == std::string::npos ? "." : input.substr(0, slash);
    if (!spec.output_dir.empty()) dir = strip_slashes(spec.output_dir);
    std::string out;
    if (!spec.output_name.empty()) {
        out = dir + "/" + spec.output_name;
        if (to == convert::Format::folder) out += "/";
    } else {
        std::string name = slash == std::string::npos ? input : input.substr(slash + 1);
        if (!is_directory(input)) {
            std::size_t dot = name.rfind('.');
            if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
        }
        if (spec.lz4) name += "-lz4";
        if (spec.lz4_unpack) name += "-unpacked";
        // Default names stay mountable by ShadowMount+.
        if (std::size_t limit = mount_name_limit(extension_for(to))) name = shorten_name(name, limit);
        out = (dir == "/" ? "" : dir) + "/" + name + extension_for(to);
    }
    if (strip_slashes(out) == input) return make_error(Errc::invalid_argument, "the output would replace the input");
    return out;
}

std::vector<std::string> EngineConfig::available_roots() const {
    std::vector<std::string> out;
    for (const std::string& r : roots)
        if (platform::root_available(strip_slashes(r))) out.push_back(strip_slashes(r));
    return out;
}

bool path_allowed(const std::string& path, const std::vector<std::string>& roots) {
    if (path.empty() || path[0] != '/') return false;
    std::size_t start = 1;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string_view part(path.data() + start, end - start);
        if (part == "." || part == "..") return false;
        start = end + 1;
    }
    for (const std::string& r : roots) {
        std::string root = strip_slashes(r);
        if (root == "/") return true;
        if (path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/'))
            return true;
    }
    return false;
}

bool input_allowed(const std::string& path, const std::vector<std::string>& roots) {
    return path_allowed(path, roots) || convert::is_provided(path);
}

namespace {

std::string checkpoint_path(const std::string& output, const EngineConfig& config) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : output) h = (h ^ c) * 0x100000001b3ULL;
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return config.state_dir + "/resume/" + buf + ".json";
}

// What must not change between a checkpoint and the run that continues it.
std::string fingerprint(const JobSpec& spec, const std::string& output, const EngineConfig& config) {
    struct stat st{};
    std::string input = strip_slashes(spec.input);
    // A shared file's path names its size and time already.
    if (::stat(input.c_str(), &st) != 0 && !convert::is_provided(input)) return "";
    JsonValue v = JsonValue::object();
    v.set("layout", static_cast<std::int64_t>(resume::kLayoutVersion));
    v.set("input", input);
    v.set("input_size", static_cast<std::int64_t>(st.st_size));
    v.set("input_mtime", static_cast<std::int64_t>(st.st_mtime));
    v.set("output", output);
    v.set("to", spec.to);
    v.set("level", spec.level);
    v.set("rebuild", spec.rebuild);
    v.set("cluster_size", spec.cluster_size);
    v.set("lz4_unpack", spec.lz4_unpack);
    v.set("passcode", spec.passcode);
    v.set("ampr_emu", spec.ampr_emu);
    if (spec.lz4) {
        // Packing: its settings, and the runtime file it puts in the game.
        v.set("lz4_profile", spec.lz4_profile);
        v.set("lz4_level", spec.lz4_level);
        std::string runtime = spec.lz4_runtime.empty() ? config.runtime_path : spec.lz4_runtime;
        struct stat rs{};
        v.set("lz4_runtime", runtime);
        if (::stat(runtime.c_str(), &rs) == 0) {
            v.set("lz4_runtime_size", static_cast<std::int64_t>(rs.st_size));
            v.set("lz4_runtime_mtime", static_cast<std::int64_t>(rs.st_mtime));
        }
    }
    return v.dump();
}

bool is_folder_output(const std::string& output) { return !output.empty() && output.back() == '/'; }

// The partial output's name, also for a folder (its side files use it).
std::string partial_name(const std::string& output) { return strip_slashes(output) + kPartialSuffix; }

// What a checkpoint continues: the partial file, or for a folder the folder
// itself; while packing LZ4 (pass 1) only its side file exists.
bool has_partial(const std::string& output) {
    if (is_regular_file(partial_name(output) + ".lz4")) return true;
    return is_folder_output(output) ? is_directory(output) : is_regular_file(output + kPartialSuffix);
}

Result<JsonValue> read_json_file(const std::string& path) {
    auto file = File::open(path, File::Mode::read);
    if (!file.ok()) return file.error();
    ALEMBIC_ASSIGN(std::uint64_t size, file->size());
    if (size > 1 << 20) return make_error(Errc::too_large, "too large");
    std::string text(static_cast<std::size_t>(size), '\0');
    ALEMBIC_TRY(file->read_at(0, MutableBytes(reinterpret_cast<std::uint8_t*>(text.data()), text.size())));
    return parse_json(text);
}

Status write_json_file(const std::string& path, const JsonValue& v) {
    std::string text = v.dump();
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, true));
    ALEMBIC_TRY(out.file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    return out.commit();
}

} // namespace

bool keeps_progress(const JobSpec& spec) {
    if (spec.kind != "convert") return false;
    auto to = target_format(spec);
    return to.ok();
}

std::optional<CheckpointInfo> checkpoint_info(const JobSpec& spec, const EngineConfig& config) {
    if (spec.kind != "convert") return std::nullopt;
    auto output = resolve_output(spec);
    if (!output.ok() || !has_partial(*output)) return std::nullopt;
    auto v = read_json_file(checkpoint_path(*output, config));
    if (!v.ok() || (*v)["fingerprint"].str() != fingerprint(spec, *output, config)) return std::nullopt;
    CheckpointInfo info;
    info.done = static_cast<std::uint64_t>((*v)["done"].integer());
    info.total = static_cast<std::uint64_t>((*v)["total"].integer());
    return info;
}

void discard_checkpoint(const JobSpec& spec, const EngineConfig& config) {
    if (spec.kind != "convert") return;
    auto output = resolve_output(spec);
    if (!output.ok()) return;
    // A folder keeps what it has; only side files go.
    if (is_folder_output(*output)) {
        for (const char* suffix : resume::kSidecarSuffixes) ::unlink((partial_name(*output) + suffix).c_str());
    } else {
        resume::remove_partial(*output + kPartialSuffix);
    }
    ::unlink(checkpoint_path(*output, config).c_str());
}

namespace {

smp::Endpoint shadowmount_endpoint(const EngineConfig& config) {
    if (!config.shadowmount_address.empty())
        if (auto e = smp::parse_endpoint(config.shadowmount_address)) return *e;
    if (!config.shadowmount_config.empty()) return smp::read_endpoint(config.shadowmount_config);
    return smp::Endpoint{};
}

// After a conversion into something ShadowMount+ mounts: hide the source when
// asked, tell ShadowMount+ to rescan, and say when it keeps another source
// of the title (which it prefers while that one exists).
void shadowmount_follow_up(const JobSpec& spec, const EngineConfig& config, const std::string& title_id,
                           const std::string& output, JsonValue& r, std::vector<std::string>& warnings) {
    if (spec.hide_source && !spec.delete_source) {
        std::string input = strip_slashes(spec.input);
        std::string hidden = smp::hidden_path(input);
        if (smp::is_hidden(input)) {
            // Already hidden.
        } else if (config.needed_by_other_jobs && config.needed_by_other_jobs(input)) {
            warnings.push_back("the source was not hidden: another queued job uses it");
        } else if (path_exists(hidden)) {
            warnings.push_back("the source was not hidden: " + hidden + " already exists");
        } else if (::rename(input.c_str(), hidden.c_str()) != 0) {
            warnings.push_back("the source was not hidden: " + os_error_message(errno));
        } else {
            r.set("source_hidden", hidden);
        }
    }
    smp::Endpoint endpoint = shadowmount_endpoint(config);
    if (!endpoint.enabled) return;
    if (!title_id.empty()) {
        auto source = smp::game_source(endpoint, title_id);
        if (source.ok() && *source && strip_slashes(**source) != strip_slashes(output) && path_exists(**source))
            r.set("smp_source", **source);
    }
    // Retry counters reset: earlier images of the title may have failed.
    r.set("smp_rescan", smp::rescan(endpoint, true).ok());
}

} // namespace

bool shadowmount_rescan(const EngineConfig& config, std::string* why) {
    smp::Endpoint endpoint = shadowmount_endpoint(config);
    Status s = endpoint.enabled ? smp::rescan(endpoint, true) : Status(make_error(Errc::unsupported, "ShadowMount+ API is not available"));
    if (!s.ok() && why) *why = s.error().message;
    return s.ok();
}

Result<JsonValue> run_conversion(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(convert::Format to, target_format(spec));
    ALEMBIC_ASSIGN(std::string output, resolve_output(spec));
    if (!input_allowed(strip_slashes(spec.input), config.available_roots()) || !path_allowed(strip_slashes(output), config.available_roots()))
        return make_error(Errc::invalid_argument, "the input and output must be inside the allowed folders");

    convert::Options options;
    options.rebuild = spec.rebuild;
    options.overwrite = spec.overwrite;
    if (!spec.passcode.empty()) options.passcode = spec.passcode;
    if (spec.cluster_size) options.exfat.cluster_size = spec.cluster_size;
    options.ffpfsc.compression.level = spec.level;
    options.ffpfsc.compression.threads = config.threads;
    options.lz4_unpack = spec.lz4_unpack;
    options.fpkg.keys_folder = config.state_dir + "/keys"; // the fpkg_keys component

    // The AMPR emulator, from the runtime component (missing_dependencies
    // asks for it when the game needs it).
    if (!spec.lz4 && spec.ampr_emu != "off" && to != convert::Format::fpkg && is_regular_file(config.runtime_path)) {
        ALEMBIC_ASSIGN(File file, File::open(config.runtime_path, File::Mode::read));
        options.ampr_emulator = std::make_shared<File>(std::move(file));
        options.ampr_emulator_always = spec.ampr_emu == "always";
    }

    JsonValue profile_json;
    if (spec.lz4) {
        if (progress && !progress({"Choosing LZ4 profile", 0, 0})) return make_error(Errc::cancelled, "cancelled");
        ALEMBIC_ASSIGN(convert::Input input, convert::open(spec.input, options.passcode));
        lz4pack::ProfileRequest request;
        request.profile = spec.lz4_profile.empty() ? "auto" : spec.lz4_profile;
        request.level = spec.lz4_level;
        ALEMBIC_ASSIGN(lz4pack::ProfileChoice choice, lz4pack::choose_profile(*input.tree, request));
        lz4pack::PackOptions pack;
        pack.config = std::move(choice.config);
        pack.config.workers = config.threads;
        std::string runtime = spec.lz4_runtime.empty() ? config.runtime_path : spec.lz4_runtime;
        if (!runtime.empty() && is_regular_file(runtime)) {
            ALEMBIC_ASSIGN(File file, File::open(runtime, File::Mode::read));
            pack.runtime_module = std::make_shared<File>(std::move(file));
        } else if (!spec.lz4_runtime.empty()) {
            return make_error(Errc::invalid_argument, "the LZ4 runtime file does not exist: " + spec.lz4_runtime);
        }
        options.lz4 = std::move(pack);
        profile_json = JsonValue::object();
        profile_json.set("name", choice.name);
        profile_json.set("description", choice.description);
        profile_json.set("tested", choice.tested);
        JsonValue notes = JsonValue::array();
        for (const std::string& n : choice.notes) notes.push(n);
        profile_json.set("notes", std::move(notes));
    }

    // Resumable outputs write into a partial file and save checkpoints; a
    // checkpoint from an earlier run of the same job is continued.
    resume::Session session;
    std::uint64_t last_done = 0, last_total = 0;
    ProgressFn tracked = [&](const ProgressEvent& e) {
        last_done = e.done;
        last_total = e.total;
        return !progress || progress(e);
    };
    const std::string cp_path = checkpoint_path(output, config);
    const std::string fp = fingerprint(spec, output, config);
    // The partial output and its side files (a folder output has only the latter).
    auto drop_partial = [&] {
        resume::remove_partial(session.partial_path);
        if (is_folder_output(output))
            for (const char* suffix : resume::kSidecarSuffixes) ::unlink((partial_name(output) + suffix).c_str());
    };
    if (spec.kind == "convert" && !fp.empty()) {
        if (!is_folder_output(output)) session.partial_path = output + kPartialSuffix;
        auto saved = read_json_file(cp_path);
        if (saved.ok() && (*saved)["fingerprint"].str() == fp && has_partial(output)) {
            resume::State state;
            for (const auto& [k, v] : (*saved)["state"].fields()) state[k] = static_cast<std::uint64_t>(v.integer());
            session.from = std::move(state);
        } else {
            drop_partial();
            ::unlink(cp_path.c_str());
        }
        session.save = [&](const resume::State& state) -> Status {
            JsonValue v = JsonValue::object(), st = JsonValue::object();
            for (const auto& [k, n] : state) st.set(k, static_cast<std::int64_t>(n));
            v.set("fingerprint", fp);
            v.set("partial", session.partial_path);
            v.set("state", std::move(st));
            v.set("done", static_cast<std::int64_t>(last_done));
            v.set("total", static_cast<std::int64_t>(last_total));
            v.set("saved", unix_now());
            ALEMBIC_TRY(make_parent_directories(cp_path));
            return write_json_file(cp_path, v);
        };
        options.resume = &session;
    }
    bool resumed = session.from.has_value();
    std::optional<Result<convert::Report>> result;
    result.emplace(convert::run(spec.input, output, to, options, tracked));
    if (!result->ok() && resumed && result->error().code == Errc::invalid_data &&
        result->error().message.find("checkpoint") != std::string::npos) {
        // The partial file or checkpoint is unusable: start over.
        drop_partial();
        ::unlink(cp_path.c_str());
        session.from.reset();
        resumed = false;
        result.emplace(convert::run(spec.input, output, to, options, tracked));
    }
    Result<convert::Report>& ran = *result;
    if (ran.ok() || ran.error().code != Errc::cancelled) {
        // Done, or failed for good: a retry starts from a clean slate unless
        // it failed after a checkpoint (a full disk, a pulled drive).
        if (ran.ok() || !is_regular_file(cp_path)) {
            drop_partial();
            ::unlink(cp_path.c_str());
        }
    }
    if (!ran.ok()) return ran.error();
    convert::Report report = std::move(*ran);
    JsonValue r = JsonValue::object();
    r.set("from", convert::name(report.from));
    r.set("to", convert::name(report.to));
    r.set("output", output);
    r.set("method", report.method);
    if (resumed) r.set("resumed", true);
    if (!report.detail.empty()) r.set("detail", report.detail);
    if (report.ampr_emulator_added) r.set("ampr_emulator", report.ampr_emulator_path);
    if (!report.title_id.empty()) r.set("title_id", report.title_id);
    if (!report.warnings.empty()) {
        JsonValue warnings = JsonValue::array();
        for (const std::string& w : report.warnings) warnings.push(w);
        r.set("warnings", std::move(warnings));
    }
    if (is_regular_file(output)) {
        auto file = File::open(output, File::Mode::read);
        if (file.ok()) {
            auto size = file->size();
            if (size.ok()) r.set("output_bytes", *size);
        }
    }
    if (report.lz4) {
        const lz4pack::PackStats& st = *report.lz4;
        JsonValue l = JsonValue::object();
        l.set("profile", std::move(profile_json));
        l.set("files_total", st.files_total);
        l.set("files_packed", st.files_packed);
        l.set("files_auto_loose", st.files_auto_loose);
        l.set("packed_bytes", st.logical_bytes);
        l.set("volume_bytes", st.volume_bytes);
        l.set("loose_bytes", st.loose_bytes);
        l.set("volumes", st.volumes);
        JsonValue warnings = JsonValue::array();
        for (const std::string& w : st.warnings) warnings.push(w);
        l.set("warnings", std::move(warnings));
        r.set("lz4", std::move(l));
    }

    if (spec.delete_source) {
        if (spec.verify) {
            ALEMBIC_ASSIGN(convert::Comparison cmp, convert::compare(spec.input, output, options.passcode, progress));
            // Packing adds the runtime when the game had none.
            std::vector<std::string> extra;
            for (const std::string& p : cmp.only_in_b)
                if (!(spec.lz4 && p == lz4pack::kRuntimeRelativePath) && p != report.ampr_emulator_path)
                    extra.push_back(p);
            // A package build writes its own sce_sys/playgo-chunk.dat (as ProsperoPkgTool does).
            if (to == convert::Format::fpkg)
                cmp.different.erase(std::remove_if(cmp.different.begin(), cmp.different.end(),
                                                   [](const std::string& p) { return p == "sce_sys/playgo-chunk.dat"; }),
                                    cmp.different.end());
            JsonValue v = JsonValue::object();
            v.set("files", cmp.files);
            v.set("bytes", cmp.bytes);
            r.set("verify", std::move(v));
            if (!cmp.only_in_a.empty() || !cmp.different.empty() || !extra.empty()) {
                std::string first = !cmp.different.empty() ? "differs: " + cmp.different[0]
                                    : !cmp.only_in_a.empty() ? "missing in the output: " + cmp.only_in_a[0]
                                                             : "only in the output: " + extra[0];
                return make_error(Errc::invalid_data, "the output does not match the source (" + first +
                                                          "); the source was kept");
            }
        }
        std::string input = strip_slashes(spec.input);
        if (config.needed_by_other_jobs && config.needed_by_other_jobs(input)) {
            r.set("source_kept", "another queued job uses it");
        } else {
            if (progress) progress({"Deleting the source", 0, 0});
            ALEMBIC_TRY(remove_tree(input));
            r.set("source_deleted", true);
        }
    }
    if (to != convert::Format::fpkg) {
        std::vector<std::string> more;
        shadowmount_follow_up(spec, config, report.title_id, output, r, more);
        if (!more.empty()) {
            JsonValue warnings = r["warnings"].is_null() ? JsonValue::array() : r["warnings"];
            for (const std::string& w : more) warnings.push(w);
            r.set("warnings", std::move(warnings));
        }
    }
    return r;
}

std::optional<AmprState> ampr_state(const std::string& path, const std::string& passcode) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 && !convert::is_provided(path)) return std::nullopt;
    // Opening a large package and reading its eboot.bin takes a moment.
    std::string key = path + "|" + std::to_string(st.st_size) + "|" + std::to_string(st.st_mtime);
    static std::mutex mutex;
    static std::map<std::string, std::optional<AmprState>> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    std::optional<AmprState> state;
    if (auto input = convert::open(path, passcode); input.ok()) {
        if (auto c = ampr_emu::check(*input->tree); c.ok()) {
            AmprState a;
            a.imports = c->imports_ampr;
            a.ships = c->ships_emulator;
            const tree::Node* fakelib = tree::find_child_icase(input->tree->root(), "fakelib");
            const tree::Node* sprx =
                fakelib && fakelib->is_directory ? tree::find_child_icase(*fakelib, ampr_emu::kFileName) : nullptr;
            a.ships_in_fakelib = sprx && !sprx->is_directory;
            state = a;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 256) cache.clear();
    cache[key] = state;
    return state;
}

std::vector<std::string> missing_dependencies(const JobSpec& spec, const EngineConfig& config) {
    std::vector<std::string> out;
    const std::string passcode = spec.passcode.empty() ? std::string(32, '0') : spec.passcode;
    const std::string input = strip_slashes(spec.input);
    const bool have_runtime = is_regular_file(config.runtime_path);
    if (spec.kind == "ampr_emu" && !have_runtime) out.push_back("ampr_runtime");
    if (spec.kind == "convert") {
        auto to = target_format(spec);
        if (!have_runtime) {
            if (spec.lz4 && spec.lz4_runtime.empty()) {
                auto a = ampr_state(input, passcode);
                if (!a || !a->ships_in_fakelib) out.push_back("ampr_runtime");
            } else if (!spec.lz4 && spec.ampr_emu != "off" && to.ok() && *to != convert::Format::fpkg) {
                auto a = ampr_state(input, passcode);
                if (a && (spec.ampr_emu == "always" ? !a->ships : a->needed())) out.push_back("ampr_runtime");
            }
        }
        const Dependency* keys = find_dependency("fpkg_keys");
        if (to.ok() && *to == convert::Format::fpkg && keys && dependency_state(*keys, config.state_dir) != "installed")
            out.push_back("fpkg_keys");
    }
    return out;
}

std::string backports_root_for(const std::string& image, const std::string& title_id, const EngineConfig& config) {
    // ShadowMount+ prefers <scan path>/backports/<TITLE_ID>; the image sits
    // in a scan path or one folder below it.
    std::string dir = image.substr(0, image.rfind('/'));
    for (int up = 0; up < 2 && !dir.empty(); ++up) {
        if (is_directory(dir + "/backports/" + title_id)) return dir + "/backports";
        dir = dir.substr(0, dir.rfind('/'));
    }
    return config.backports_root;
}

Result<JsonValue> run_ampr_emu(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    std::string input = strip_slashes(spec.input);
    if (!path_allowed(input, config.available_roots())) return make_error(Errc::invalid_argument, "outside the allowed folders");
    if (!is_regular_file(config.runtime_path))
        return make_error(Errc::invalid_argument, "the AMPR emulator is not downloaded; get it in Components first");
    ALEMBIC_ASSIGN(File emulator, File::open(config.runtime_path, File::Mode::read));
    if (progress) progress({"Checking eboot.bin", 0, 0});
    ampr_emu::AttachResult result;
    if (is_directory(input)) {
        ALEMBIC_ASSIGN(result, ampr_emu::attach_to_folder(input, emulator));
    } else {
        const std::string passcode = spec.passcode.empty() ? std::string(32, '0') : spec.passcode;
        ALEMBIC_ASSIGN(convert::Input game, convert::open(input, passcode));
        if (game.format == convert::Format::fpkg)
            return make_error(Errc::invalid_argument,
                              "a package does not need it: installed, it uses the console's own APR");
        std::optional<std::string> title_id = ffpfsc::read_title_id(*game.tree, game.tree->root());
        if (!title_id) return make_error(Errc::invalid_argument, "the game has no title ID in sce_sys/param.json");
        std::string root = backports_root_for(input, *title_id, config);
        if (root.empty()) return make_error(Errc::invalid_argument, "no backports folder is set");
        ALEMBIC_ASSIGN(result, ampr_emu::attach_backport(*game.tree, root, emulator));
    }
    JsonValue r = JsonValue::object();
    r.set("method", result.already_there ? "nothing to do: the emulator is already in place"
                    : is_directory(input) ? "added the AMPR emulator to the game folder"
                                          : "added the AMPR emulator as a ShadowMount+ backport");
    r.set("output", result.folder);
    JsonValue written = JsonValue::array();
    for (const std::string& w : result.written) written.push(w);
    r.set("written", std::move(written));
    JsonValue notes = JsonValue::array();
    for (const std::string& n : result.notes) notes.push(n);
    r.set("warnings", std::move(notes));
    return r;
}

Result<JsonValue> run_transfer(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    std::string input = strip_slashes(spec.input), output = strip_slashes(spec.output);
    if (!path_allowed(input, config.available_roots()) || !path_allowed(output, config.available_roots()))
        return make_error(Errc::invalid_argument, "the source and destination must be inside the allowed folders");
    if (spec.kind == "move" && config.needed_by_other_jobs && config.needed_by_other_jobs(input))
        return make_error(Errc::invalid_argument, "another queued job uses " + input);
    if (spec.kind == "copy") ALEMBIC_TRY(copy_tree(input, output, spec.overwrite, progress));
    else ALEMBIC_TRY(move_tree(input, output, spec.overwrite, progress));
    JsonValue r = JsonValue::object();
    r.set("method", spec.kind == "copy" ? "copied" : "moved");
    r.set("output", output);
    auto size = tree_size(output);
    if (size.ok()) r.set("output_bytes", *size);
    return r;
}

Result<JsonValue> run_install(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    std::string input = strip_slashes(spec.input);
    if (!path_allowed(input, config.available_roots())) return make_error(Errc::invalid_argument, "outside the allowed folders");
    // Only packages Alembic can read: a debug FPKG (the console refuses others anyway).
    if (progress) progress({"Checking the package", 0, 0});
    ALEMBIC_ASSIGN(convert::Format format, convert::detect(input));
    if (format != convert::Format::fpkg) return make_error(Errc::invalid_argument, "not a PKG file");
    if (progress) progress({"Handing it to the system installer", 0, 0});
    ALEMBIC_TRY(platform::install_package(input));
    JsonValue r = JsonValue::object();
    r.set("method", "handed to the system installer; it installs in the background (see Downloads)");
    return r;
}

Result<JsonValue> run_job(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    if (spec.kind == "install") return run_install(spec, config, progress);
    if (spec.kind == "ampr_emu") return run_ampr_emu(spec, config, progress);
    if (spec.kind == "copy" || spec.kind == "move") return run_transfer(spec, config, progress);
    return run_conversion(spec, config, progress);
}

} // namespace alembic::engine

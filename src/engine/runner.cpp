// SPDX-License-Identifier: GPL-3.0-or-later
#include "runner.hpp"
#include "deps.hpp"
#include "fs_ops.hpp"
#include "platform.hpp"

#include <map>
#include <mutex>
#include <sys/stat.h>

#include "alembic/convert.hpp"
#include "alembic/lz4pack.hpp"

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
    case convert::Format::fpkg: return ".pkg";
    case convert::Format::folder: return "/";
    }
    return "";
}

Result<convert::Format> target_format(const JobSpec& spec) {
    if (!spec.to.empty()) {
        auto f = convert::parse_format(spec.to);
        if (!f || (*f != convert::Format::folder && *f != convert::Format::exfat && *f != convert::Format::ffpfsc))
            return make_error(Errc::invalid_argument, "to must be folder, exfat or ffpfsc");
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

Result<JsonValue> run_conversion(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(convert::Format to, target_format(spec));
    ALEMBIC_ASSIGN(std::string output, resolve_output(spec));
    if (!path_allowed(strip_slashes(spec.input), config.available_roots()) || !path_allowed(strip_slashes(output), config.available_roots()))
        return make_error(Errc::invalid_argument, "the input and output must be inside the allowed folders");

    convert::Options options;
    options.rebuild = spec.rebuild;
    options.overwrite = spec.overwrite;
    if (!spec.passcode.empty()) options.passcode = spec.passcode;
    if (spec.cluster_size) options.exfat.cluster_size = spec.cluster_size;
    options.ffpfsc.compression.level = spec.level;
    options.ffpfsc.compression.threads = config.threads;
    options.lz4_unpack = spec.lz4_unpack;

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

    ALEMBIC_ASSIGN(convert::Report report, convert::run(spec.input, output, to, options, progress));
    JsonValue r = JsonValue::object();
    r.set("from", convert::name(report.from));
    r.set("to", convert::name(report.to));
    r.set("output", output);
    r.set("method", report.method);
    if (!report.detail.empty()) r.set("detail", report.detail);
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
                if (!(spec.lz4 && p == lz4pack::kRuntimeRelativePath)) extra.push_back(p);
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
    return r;
}

namespace {

// Whether the game at `path` ships fakelib/libSceAmpr.sprx. Opening a large
// package takes a moment, so answers are kept per path, size and mtime.
bool ships_ampr_runtime(const std::string& path, const std::string& passcode) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    std::string key = path + "|" + std::to_string(st.st_size) + "|" + std::to_string(st.st_mtime);
    static std::mutex mutex;
    static std::map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    bool ships = false;
    if (auto input = convert::open(path, passcode); input.ok()) {
        const tree::Node* fakelib = tree::find_child_icase(input->tree->root(), "fakelib");
        const tree::Node* sprx = fakelib && fakelib->is_directory ? tree::find_child_icase(*fakelib, "libSceAmpr.sprx") : nullptr;
        ships = sprx && !sprx->is_directory;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 256) cache.clear();
    cache[key] = ships;
    return ships;
}

} // namespace

std::vector<std::string> missing_dependencies(const JobSpec& spec, const EngineConfig& config) {
    std::vector<std::string> out;
    if (spec.kind == "convert" && spec.lz4 && spec.lz4_runtime.empty() && !is_regular_file(config.runtime_path) &&
        !ships_ampr_runtime(strip_slashes(spec.input), spec.passcode.empty() ? std::string(32, '0') : spec.passcode))
        out.push_back("ampr_runtime");
    return out;
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
    if (spec.kind == "copy" || spec.kind == "move") return run_transfer(spec, config, progress);
    return run_conversion(spec, config, progress);
}

} // namespace alembic::engine

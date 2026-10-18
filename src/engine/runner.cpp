// SPDX-License-Identifier: GPL-3.0-or-later
#include "runner.hpp"
#include "fs_ops.hpp"

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
    std::string name = slash == std::string::npos ? input : input.substr(slash + 1);
    if (!is_directory(input)) {
        std::size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    }
    if (spec.lz4) name += "-lz4";
    if (spec.lz4_unpack) name += "-unpacked";
    std::string out = (dir.empty() ? "" : dir + "/") + name + extension_for(to);
    if (strip_slashes(out) == input) return make_error(Errc::invalid_argument, "the output would replace the input");
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
    if (!path_allowed(strip_slashes(spec.input), config.roots) || !path_allowed(strip_slashes(output), config.roots))
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

Result<JsonValue> run_transfer(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    std::string input = strip_slashes(spec.input), output = strip_slashes(spec.output);
    if (!path_allowed(input, config.roots) || !path_allowed(output, config.roots))
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

Result<JsonValue> run_job(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress) {
    if (spec.kind == "copy" || spec.kind == "move") return run_transfer(spec, config, progress);
    return run_conversion(spec, config, progress);
}

} // namespace alembic::engine

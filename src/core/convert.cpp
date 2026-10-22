// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/convert.hpp"
#include "alembic/sha256.hpp"

#include "alembic/bytes.hpp"
#include "alembic/exfat_reader.hpp"
#include "alembic/fpkg_inner.hpp"
#include "alembic/resume.hpp"
#include "alembic/pfs.hpp"
#include "alembic/ufs2.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace alembic::convert {

namespace {

constexpr std::uint64_t kPfsMagic = 20130315;

bool ends_with_icase(const std::string& s, const char* suffix) {
    std::size_t n = std::strlen(suffix);
    return s.size() >= n && unicode::ascii_iequals(std::string_view(s).substr(s.size() - n), suffix);
}

std::string stem(const std::string& path) {
    std::string base = base_name(path);
    std::size_t dot = base.rfind('.');
    return dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
}

Error unsupported(Format from, Format to, const std::string& hint = "") {
    std::string message = std::string("converting ") + name(from) + " to " + name(to) + " is not supported";
    return make_error(Errc::unsupported, hint.empty() ? message : message + "; " + hint);
}

Status copy_image(const RandomAccess& image, const std::string& output, bool overwrite, const ProgressFn& progress,
                  resume::Session* session) {
    ALEMBIC_ASSIGN(std::uint64_t length, image.size());
    ALEMBIC_ASSIGN(std::uint64_t start, resume::sequential_start(session, length));
    ALEMBIC_ASSIGN(AtomicOutput out, session ? AtomicOutput::create_partial(output, session->partial_path, session->from.has_value(), overwrite)
                                             : AtomicOutput::create(output, overwrite));
    if (session) ALEMBIC_TRY(out.file().truncate(start)); // drop what came after the checkpoint
    resume::CheckpointSink sink(out.file(), start, length, session);
    std::vector<std::uint8_t> buffer(1 << 20);
    for (std::uint64_t done = start; done < length;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - done));
        Status s = image.read_at(done, MutableBytes(buffer.data(), n));
        if (s.ok()) s = sink.write(Bytes(buffer.data(), n));
        done += n;
        if (s.ok() && progress && !progress({"Copying image", done, length})) s = make_error(Errc::cancelled, "cancelled");
        if (!s.ok()) {
            (void)sink.save_now(); // a pause loses nothing already written
            return s;
        }
    }
    return out.commit();
}

} // namespace

namespace {

OutputProvider& out_provider(); // below

// A stream into a provided output (an FTP upload), copying a whole source.
Status stream_copy(const RandomAccess& source, OutputStream& out, const char* stage, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(std::uint64_t length, source.size());
    std::vector<std::uint8_t> buffer(4 << 20);
    for (std::uint64_t done = 0; done < length;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - done));
        ALEMBIC_TRY(source.read_at(done, MutableBytes(buffer.data(), n)));
        ALEMBIC_TRY(out.write(Bytes(buffer.data(), n)));
        done += n;
        if (progress && !progress({stage, done, length})) return make_error(Errc::cancelled, "cancelled");
    }
    return {};
}

// A tree as a folder on a provided output: folders first made, files streamed.
Status upload_tree(const tree::Source& source, const tree::Node& dir, const std::string& target, bool overwrite,
                   std::uint64_t& done, std::uint64_t total, const ProgressFn& progress) {
    ALEMBIC_TRY(out_provider().make_directory(target));
    std::vector<std::uint8_t> buffer(4 << 20);
    for (const tree::Node& child : dir.children) {
        if (tree::is_os_junk(child.name)) continue;
        std::string path = target + "/" + child.name;
        if (child.is_directory) {
            ALEMBIC_TRY(upload_tree(source, child, path, overwrite, done, total, progress));
            continue;
        }
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, source.open(child));
        ALEMBIC_ASSIGN(std::unique_ptr<OutputStream> out, out_provider().create(path, overwrite));
        for (std::uint64_t at = 0; at < child.size;) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), child.size - at));
            ALEMBIC_TRY(in->read_at(at, MutableBytes(buffer.data(), n)));
            ALEMBIC_TRY(out->write(Bytes(buffer.data(), n)));
            at += n;
            done += n;
            if (progress && !progress({"Uploading", done, total})) return make_error(Errc::cancelled, "cancelled");
        }
        ALEMBIC_TRY(out->commit());
    }
    return {};
}

// A planned image (exFAT, UFS2 or PFS builder) streamed into a provided output.
template <class Builder>
Status upload_image(const Builder& builder, const std::string& output, bool overwrite, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(std::unique_ptr<OutputStream> out, out_provider().create(output, overwrite));
    ALEMBIC_TRY(builder.write(*out, progress));
    return out->commit();
}

} // namespace

const char* name(Format format) {
    switch (format) {
    case Format::folder: return "folder";
    case Format::exfat: return "exFAT";
    case Format::ffpfsc: return "FFPFSC";
    case Format::fpkg: return "FPKG";
    case Format::ffpkg: return "FFPKG (UFS2)";
    case Format::ffpfs: return "FFPFS (PFS)";
    }
    return "?";
}

std::optional<Format> parse_format(const std::string& text) {
    if (unicode::ascii_iequals(text, "folder") || unicode::ascii_iequals(text, "dir")) return Format::folder;
    if (unicode::ascii_iequals(text, "exfat")) return Format::exfat;
    if (unicode::ascii_iequals(text, "ffpfsc")) return Format::ffpfsc;
    if (unicode::ascii_iequals(text, "fpkg") || unicode::ascii_iequals(text, "pkg")) return Format::fpkg;
    if (unicode::ascii_iequals(text, "ffpkg") || unicode::ascii_iequals(text, "ufs2")) return Format::ffpkg;
    if (unicode::ascii_iequals(text, "ffpfs") || unicode::ascii_iequals(text, "pfs")) return Format::ffpfs;
    return std::nullopt;
}

std::optional<Format> guess_output_format(const std::string& path) {
    if (!path.empty() && path.back() == '/') return Format::folder;
    if (ends_with_icase(path, ".exfat") || ends_with_icase(path, ".img")) return Format::exfat;
    if (ends_with_icase(path, ".ffpfsc")) return Format::ffpfsc;
    if (ends_with_icase(path, ".ffpkg")) return Format::ffpkg;
    if (ends_with_icase(path, ".ffpfs")) return Format::ffpfs;
    if (ends_with_icase(path, ".pkg")) return Format::fpkg;
    if (is_directory(path)) return Format::folder;
    return std::nullopt;
}

namespace {

FileProvider& provider() {
    static FileProvider p;
    return p;
}

// A file input: one the provider has for this path, else the file on disk.
Result<std::shared_ptr<const RandomAccess>> open_input_file(const std::string& path) {
    if (provider())
        if (std::shared_ptr<const RandomAccess> f = provider()(path)) return f;
    ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
    return std::shared_ptr<const RandomAccess>(std::make_shared<File>(std::move(file)));
}

} // namespace

std::function<bool(const std::string&)>& provider_handles() {
    static std::function<bool(const std::string&)> h;
    return h;
}

void set_file_provider(FileProvider p, std::function<bool(const std::string&)> handles) {
    provider() = std::move(p);
    provider_handles() = std::move(handles);
}

namespace {
OutputProvider& out_provider() {
    static OutputProvider p;
    return p;
}
} // namespace

void set_output_provider(OutputProvider p) { out_provider() = std::move(p); }
bool is_output_provided(const std::string& path) { return out_provider().handles && out_provider().handles(path); }
bool output_exists(const std::string& path) {
    if (is_output_provided(path)) return out_provider().exists && out_provider().exists(path);
    return path_exists(path);
}

bool is_provided(const std::string& path) {
    if (provider_handles()) return provider_handles()(path);
    return provider() && provider()(path) != nullptr;
}

Result<Format> detect(const std::string& path) {
    const bool provided = is_provided(path);
    if (!provided && is_directory(path)) return Format::folder;
    if (!provided && !is_regular_file(path)) return make_error(Errc::invalid_argument, "no such file or folder: " + path);
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> opened, open_input_file(path));
    const RandomAccess& file = *opened;
    ALEMBIC_ASSIGN(std::uint64_t size, file.size());
    std::uint8_t head[512] = {};
    ALEMBIC_TRY(file.read_at(0, MutableBytes(head, static_cast<std::size_t>(std::min<std::uint64_t>(size, sizeof head)))));
    if (size >= 4 && (std::memcmp(head, "\x7F" "CNT", 4) == 0 || std::memcmp(head, "\x7F" "FIH", 4) == 0))
        return Format::fpkg;
    // Both are PFS: an FFPFSC holds one compressed image, an .ffpfs the game.
    if (size >= 0x10 && get_u64(head) == 2 && get_u64(head + 8) == kPfsMagic)
        return ffpfsc::inspect(file).ok() ? Format::ffpfsc : Format::ffpfs;
    if (exfat::looks_like_exfat(file)) return Format::exfat;
    if (ufs2::looks_like_ufs2(file)) return Format::ffpkg;
    return make_error(Errc::unsupported, "unrecognised input (not a folder, exFAT, UFS2, PFS, FFPFSC or FPKG): " + path);
}

Result<Input> open(const std::string& path, const std::string& passcode) {
    Input input;
    input.path = path;
    ALEMBIC_ASSIGN(input.format, detect(path));
    switch (input.format) {
    case Format::folder: {
        ALEMBIC_ASSIGN(input.tree, tree::open_folder(path));
        return input;
    }
    case Format::exfat: {
        ALEMBIC_ASSIGN(input.image, open_input_file(path));
        ALEMBIC_ASSIGN(input.tree, exfat::open_image(input.image, base_name(path)));
        return input;
    }
    case Format::ffpfsc: {
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, open_input_file(path));
        ALEMBIC_ASSIGN(ffpfsc::Payload payload, ffpfsc::open_payload(std::move(file)));
        input.ffpfsc = payload.info;
        input.image = payload.image;
        if (exfat::looks_like_exfat(*input.image)) {
            ALEMBIC_ASSIGN(input.tree, exfat::open_image(input.image, stem(payload.info.inner_file_name)));
        } else if (ufs2::looks_like_ufs2(*input.image)) {
            input.image_is_ufs2 = true;
            ALEMBIC_ASSIGN(input.tree, ufs2::open_image(input.image, stem(payload.info.inner_file_name)));
        } else if (pfs::looks_like_pfs(*input.image)) {
            input.image_is_pfs = true;
            ALEMBIC_ASSIGN(input.tree, pfs::open_image(input.image, stem(payload.info.inner_file_name)));
        } else {
            return make_error(Errc::unsupported, "the FFPFSC payload '" + payload.info.inner_file_name +
                                                     "' is not an exFAT, UFS2 or PFS image");
        }
        return input;
    }
    case Format::ffpkg: {
        ALEMBIC_ASSIGN(input.image, open_input_file(path));
        input.image_is_ufs2 = true;
        ALEMBIC_ASSIGN(input.tree, ufs2::open_image(input.image, stem(path)));
        return input;
    }
    case Format::ffpfs: {
        ALEMBIC_ASSIGN(input.image, open_input_file(path));
        input.image_is_pfs = true;
        ALEMBIC_ASSIGN(input.tree, pfs::open_image(input.image, stem(path)));
        return input;
    }
    case Format::fpkg: {
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, open_input_file(path));
        ALEMBIC_ASSIGN(input.tree, fpkg::open_tree(std::move(file), stem(path), passcode));
        return input;
    }
    }
    return make_error(Errc::unsupported, "unknown format");
}

Result<Report> run(const std::string& input_path, const std::string& output, Format to, const Options& options,
                   const ProgressFn& progress) {
    ALEMBIC_ASSIGN(std::string in_full, absolute_path(input_path));
    ALEMBIC_ASSIGN(std::string out_full, absolute_path(output));
    if (in_full == out_full) return make_error(Errc::invalid_argument, "the output must differ from the input");

    const bool remote_out = is_output_provided(output);
    if (remote_out && (to == Format::ffpfsc || to == Format::fpkg))
        return make_error(Errc::unsupported, std::string(to == Format::ffpfsc ? "FFPFSC" : "PKG") +
                                                 " output cannot be written straight to an FTP server (the writer goes "
                                                 "back to earlier parts of the file); save it on the console, then copy it there");
    if (to != Format::folder && !options.overwrite && output_exists(output))
        return make_error(Errc::invalid_argument, "the output file already exists: " + output);

    ALEMBIC_ASSIGN(Input input, open(input_path, options.passcode));
    Report report;
    report.from = input.format;
    report.to = to;
    const tree::Source* tree_ptr = input.tree.get();
    std::unique_ptr<lz4pack::PackedTree> packed;
    std::unique_ptr<tree::Source> unpacked;
    // Packing with checkpoints: pass 1 saves its own (format kLz4Format);
    // afterwards the output writer's carry the LZ4 keys too, so a run can
    // replay pass 1 from its side file and continue the output.
    resume::Session packed_session;
    std::int64_t lz4_stamp = 0;
    std::uint64_t lz4_chunks = 0;
    auto add_lz4_keys = [&](resume::State& st, bool done) {
        st["lz4_timestamp"] = static_cast<std::uint64_t>(lz4_stamp);
        st["lz4_chunks"] = lz4_chunks;
        st["lz4_done"] = done ? 1 : 0;
    };
    if (options.lz4 && options.lz4_unpack)
        return make_error(Errc::invalid_argument, "choose either LZ4 packing or unpacking");
    if (options.lz4_unpack) {
        if (!lz4pack::is_packed(tree_ptr->root()))
            return make_error(Errc::invalid_argument, "the input has no LZ4 packs (no ampr_assets.index)");
        ALEMBIC_ASSIGN(unpacked, lz4pack::unpack(*tree_ptr));
        tree_ptr = unpacked.get();
    } else if (options.lz4) {
        lz4pack::PackOptions pack = *options.lz4;
        if (resume::Session* s = options.resume) {
            std::string base = s->partial_path;
            if (base.empty()) {
                base = output;
                while (base.size() > 1 && base.back() == '/') base.pop_back();
                base += resume::kPartialSuffix;
            }
            std::optional<resume::State> from = s->from;
            if (from && resume::get(*from, "lz4_timestamp") == ~std::uint64_t{0}) from.reset();
            bool done = false;
            if (from) {
                lz4_stamp = static_cast<std::int64_t>(resume::get(*from, "lz4_timestamp"));
                lz4_chunks = resume::get(*from, "lz4_chunks");
                done = resume::get(*from, "lz4_done") == 1;
            } else {
                lz4_stamp = pack.timestamp.value_or(static_cast<std::int64_t>(std::time(nullptr)));
            }
            pack.timestamp = lz4_stamp;
            lz4pack::PackCheckpoints cp;
            cp.sidecar_path = base + ".lz4";
            cp.resume_chunks = lz4_chunks;
            cp.interval = s->interval;
            cp.save = [s, &lz4_chunks, &add_lz4_keys](std::uint64_t chunks, bool finished) -> Status {
                lz4_chunks = chunks;
                resume::State st;
                st["format"] = resume::kLz4Format;
                add_lz4_keys(st, finished);
                return s->save ? s->save(st) : Status{};
            };
            pack.checkpoints = std::move(cp);
            packed_session.partial_path = s->partial_path;
            packed_session.interval = s->interval;
            if (from && done && resume::get(*from, "format") != resume::kLz4Format) {
                resume::State w = *from;
                for (const char* k : {"lz4_timestamp", "lz4_chunks", "lz4_done"}) w.erase(k);
                packed_session.from = std::move(w);
            }
            packed_session.save = [s, &add_lz4_keys](const resume::State& w) -> Status {
                resume::State st = w;
                add_lz4_keys(st, true);
                return s->save ? s->save(st) : Status{};
            };
        }
        ALEMBIC_ASSIGN(packed, lz4pack::pack(*tree_ptr, pack, progress));
        report.lz4 = packed->stats();
        report.lz4_config_toml = lz4pack::to_toml(packed->config());
        report.lz4_forced_loose = packed->forced_loose_patterns();
        tree_ptr = packed.get();
    }
    // The AMPR emulator: not for packages (installed, they have the console's
    // APR) nor when packing (the packer adds its runtime).
    std::unique_ptr<tree::Source> with_emulator;
    if (to != Format::fpkg && !packed) {
        if (progress && !progress({"Checking eboot.bin", 0, 0})) return make_error(Errc::cancelled, "cancelled");
        // A stage without a byte count: its bytes are not the conversion's.
        ProgressFn stage_only = [&](const ProgressEvent& e) { return !progress || progress({e.stage, 0, 0}); };
        ALEMBIC_ASSIGN(ampr_emu::Check check, ampr_emu::check(*tree_ptr, stage_only));
        report.ampr_needed = check.needed();
        bool add = options.ampr_emulator && (check.needed() || (options.ampr_emulator_always && !check.ships_emulator));
        if (add) {
            ALEMBIC_ASSIGN(with_emulator, ampr_emu::add(*tree_ptr, check, options.ampr_emulator, tree_ptr->root().mtime));
            tree_ptr = with_emulator.get();
            report.ampr_emulator_added = true;
            report.ampr_emulator_path = check.fakelib + "/" + ampr_emu::kFileName;
        } else if (check.needed()) {
            report.warnings.push_back("eboot.bin uses the console's APR (libSceAmpr) and the game ships no ampr_emu: "
                                      "it will likely not start from an image or folder (add the emulator)");
        }
    }
    const tree::Source& tree = *tree_ptr;
    report.title_id = ffpfsc::read_title_id(tree, tree.root()).value_or("");
    // A transformed tree no longer matches the input image byte for byte.
    bool transformed = tree_ptr != input.tree.get();
    bool rebuild = options.rebuild || transformed;
    std::string prefix_text = std::string(packed ? "LZ4-packed and " : unpacked ? "LZ4-unpacked and " : "") +
                              (with_emulator ? "added the AMPR emulator and " : "");
    const char* prefix = prefix_text.c_str();
    resume::Session* session = packed ? (options.resume ? &packed_session : nullptr) : options.resume;
    if (remote_out) session = nullptr; // an upload starts over when stopped
    report.resumable = session != nullptr;

    switch (to) {
    case Format::folder: {
        if (input.format == Format::folder && !transformed) return unsupported(input.format, to, "copy the folder instead");
        if (remote_out) {
            std::string target = output;
            while (target.size() > 1 && target.back() == '/') target.pop_back();
            std::uint64_t done = 0, total = tree::totals(tree.root()).bytes;
            ALEMBIC_TRY(upload_tree(tree, tree.root(), target, options.overwrite, done, total, progress));
            report.method = std::string(prefix) + "uploaded the file tree";
            return report;
        }
        ALEMBIC_TRY(tree::extract(tree, tree.root(), output, options.overwrite, progress, session));
        report.method = std::string(prefix) + "extracted the file tree";
        return report;
    }
    case Format::exfat: {
        if (input.format == Format::exfat && !rebuild)
            return unsupported(input.format, to, "pass --rebuild to re-lay out the image");
        if (remote_out) {
            if (input.image && !input.image_is_ufs2 && !input.image_is_pfs && !rebuild) {
                ALEMBIC_ASSIGN(std::unique_ptr<OutputStream> out, out_provider().create(output, options.overwrite));
                ALEMBIC_TRY(stream_copy(*input.image, *out, "Copying image", progress));
                ALEMBIC_TRY(out->commit());
                report.method = "unwrapped the exFAT image unchanged";
                return report;
            }
            ALEMBIC_ASSIGN(exfat::ImageBuilder builder, exfat::ImageBuilder::from_source(tree, tree.root(), options.exfat));
            ALEMBIC_TRY(upload_image(builder, output, options.overwrite, progress));
            report.method = std::string(prefix) + "built a new exFAT image from the file tree";
            return report;
        }
        if (input.image && !input.image_is_ufs2 && !input.image_is_pfs && !rebuild) {
            ALEMBIC_TRY(copy_image(*input.image, output, options.overwrite, progress, session));
            report.method = "unwrapped the exFAT image unchanged";
            return report;
        }
        ALEMBIC_TRY(exfat::write_image(tree, tree.root(), output, options.exfat, options.overwrite, progress, session));
        report.method = std::string(prefix) + "built a new exFAT image from the file tree";
        return report;
    }
    case Format::ffpfsc: {
        ffpfsc::BuildOptions ff = options.ffpfsc;
        ff.overwrite_existing = options.overwrite;
        ff.resume = session;
        if (input.format == Format::folder && !transformed) {
            ALEMBIC_ASSIGN(ffpfsc::BuildResult r,
                           ffpfsc::create_from_directory(input_path, output, ff, options.exfat, progress));
            report.method = "built a new exFAT image from the folder and compressed it";
            report.detail = r.inner_file_name;
            return report;
        }
        if (input.image && !rebuild) {
            std::string inner = input.ffpfsc ? input.ffpfsc->inner_file_name : base_name(input_path);
            ALEMBIC_ASSIGN(ffpfsc::BuildResult r, ffpfsc::create_from_payload(*input.image, inner, output, ff, progress));
            const char* kind = input.image_is_ufs2 ? "UFS2" : input.image_is_pfs ? "PFS" : "exFAT";
            report.method = std::string(input.ffpfsc ? "recompressed the " : "compressed the ") + kind + " image unchanged";
            report.detail = r.inner_file_name;
            return report;
        }
        ALEMBIC_ASSIGN(ffpfsc::BuildResult r,
                       ffpfsc::create_from_tree(tree, tree.root(), output, ff, options.exfat, progress));
        report.method = std::string(prefix) + "built a new exFAT image from the file tree and compressed it";
        report.detail = r.inner_file_name;
        return report;
    }
    case Format::ffpkg: {
        if (input.image && input.image_is_ufs2 && !rebuild) {
            if (!input.ffpfsc) return unsupported(input.format, to, "pass --rebuild to re-lay out the image");
            if (remote_out) {
                ALEMBIC_ASSIGN(std::unique_ptr<OutputStream> out, out_provider().create(output, options.overwrite));
                ALEMBIC_TRY(stream_copy(*input.image, *out, "Copying image", progress));
                ALEMBIC_TRY(out->commit());
            } else {
                ALEMBIC_TRY(copy_image(*input.image, output, options.overwrite, progress, session));
            }
            report.method = "unwrapped the UFS2 image unchanged";
            return report;
        }
        ufs2::BuildOptions u = options.ufs2;
        if (u.volume_name.empty()) {
            // PS5PKGTool: the title ID for a dump, else the file name.
            if (auto id = ffpfsc::read_title_id(tree, tree.root())) u.volume_name = *id;
            else {
                std::string name = base_name(output);
                std::size_t dot = name.rfind('.');
                u.volume_name = dot == std::string::npos ? name : name.substr(0, dot);
            }
        }
        if (remote_out) {
            ALEMBIC_ASSIGN(ufs2::ImageBuilder builder, ufs2::ImageBuilder::from_source(tree, tree.root(), u));
            ALEMBIC_TRY(upload_image(builder, output, options.overwrite, progress));
        } else {
            ALEMBIC_TRY(ufs2::write_image(tree, tree.root(), output, u, options.overwrite, progress, session));
        }
        report.method = std::string(prefix) + "built a new UFS2 image from the file tree";
        return report;
    }
    case Format::ffpfs: {
        if (input.image && input.image_is_pfs && !rebuild) {
            if (!input.ffpfsc) return unsupported(input.format, to, "pass --rebuild to re-lay out the image");
            if (remote_out) {
                ALEMBIC_ASSIGN(std::unique_ptr<OutputStream> out, out_provider().create(output, options.overwrite));
                ALEMBIC_TRY(stream_copy(*input.image, *out, "Copying image", progress));
                ALEMBIC_TRY(out->commit());
            } else {
                ALEMBIC_TRY(copy_image(*input.image, output, options.overwrite, progress, session));
            }
            report.method = "unwrapped the PFS image unchanged";
            return report;
        }
        pfs::BuildOptions p;
        p.block_size = options.ffpfsc.pfs_block_size;
        p.case_insensitive = options.ffpfsc.case_insensitive;
        p.timestamp = options.ffpfsc.build_timestamp;
        p.generate_ampr_index = options.exfat.generate_ampr_index;
        p.rebuild_ampr_index = options.exfat.rebuild_ampr_index;
        p.ampr_scheme = options.exfat.ampr_scheme;
        if (remote_out) {
            ALEMBIC_ASSIGN(pfs::ImageBuilder builder, pfs::ImageBuilder::from_source(tree, tree.root(), p));
            ALEMBIC_TRY(upload_image(builder, output, options.overwrite, progress));
        } else {
            ALEMBIC_TRY(pfs::write_image(tree, tree.root(), output, p, options.overwrite, progress, session));
        }
        report.method = std::string(prefix) + "built a new PFS image from the file tree";
        return report;
    }
    case Format::fpkg: {
        if (input.format == Format::fpkg && !rebuild) return unsupported(input.format, to, "pass --rebuild to build it again");
        fpkg::build::PackageOptions o = options.fpkg;
        o.overwrite = options.overwrite;
        if (!o.threads) o.threads = options.ffpfsc.compression.threads;
        ALEMBIC_ASSIGN(fpkg::build::PackageResult r, fpkg::build::write_package(tree, tree.root(), output, o, progress, session));
        report.method = std::string(prefix) + "built a debug package from the file tree";
        report.detail = r.content_id;
        report.warnings = std::move(r.warnings);
        return report;
    }
    }
    return unsupported(input.format, to);
}

namespace {

// A game's files for comparing: opened, LZ4 packs unpacked, sorted by path,
// without ampr_emu.index (conversions may write their own).
struct GameFiles {
    Input input;
    std::unique_ptr<tree::Source> unpacked;
    const tree::Source* tree = nullptr;
    std::vector<std::pair<std::string, const tree::Node*>> files;
};

Status open_game_files(const std::string& path, const std::string& passcode, GameFiles& g) {
    ALEMBIC_ASSIGN(g.input, open(path, passcode));
    g.tree = g.input.tree.get();
    if (lz4pack::is_packed(g.tree->root())) {
        ALEMBIC_ASSIGN(g.unpacked, lz4pack::unpack(*g.tree));
        g.tree = g.unpacked.get();
    }
    std::vector<std::pair<std::string, const tree::Node*>> stack{{"", &g.tree->root()}};
    while (!stack.empty()) {
        auto [prefix, node] = stack.back();
        stack.pop_back();
        for (const tree::Node& c : node->children) {
            std::string p = prefix.empty() ? c.name : prefix + "/" + c.name;
            if (c.is_directory) stack.push_back({p, &c});
            else if (p != "ampr_emu.index") g.files.push_back({p, &c});
        }
    }
    std::sort(g.files.begin(), g.files.end());
    return {};
}

} // namespace

Result<Comparison> compare(const std::string& a, const std::string& b, const std::string& passcode,
                           const ProgressFn& progress) {
    GameFiles g[2];
    ALEMBIC_TRY(open_game_files(a, passcode, g[0]));
    ALEMBIC_TRY(open_game_files(b, passcode, g[1]));
    const auto& files = g;
    Comparison result;
    std::uint64_t total = 0, done = 0;
    for (auto& f : files[0].files) total += f.second->size;
    std::size_t j = 0;
    std::vector<std::uint8_t> x(4 << 20), y(4 << 20);
    for (auto& [path, node] : files[0].files) {
        while (j < files[1].files.size() && files[1].files[j].first < path) result.only_in_b.push_back(files[1].files[j++].first);
        if (j >= files[1].files.size() || files[1].files[j].first != path) {
            result.only_in_a.push_back(path);
            done += node->size;
            continue;
        }
        const tree::Node* other = files[1].files[j++].second;
        ++result.files;
        if (other->size != node->size) {
            result.different.push_back(path);
            done += node->size;
            continue;
        }
        ALEMBIC_ASSIGN(auto fa, g[0].tree->open(*node));
        ALEMBIC_ASSIGN(auto fb, g[1].tree->open(*other));
        for (std::uint64_t at = 0; at < node->size;) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(x.size(), node->size - at));
            ALEMBIC_TRY(fa->read_at(at, MutableBytes(x.data(), n)));
            ALEMBIC_TRY(fb->read_at(at, MutableBytes(y.data(), n)));
            at += n;
            done += n;
            result.bytes += n;
            if (std::memcmp(x.data(), y.data(), n) != 0) {
                result.different.push_back(path);
                done += node->size - at;
                break;
            }
            if (progress && !progress({"Comparing", done, total})) return make_error(Errc::cancelled, "cancelled");
        }
    }
    for (; j < files[1].files.size(); ++j) result.only_in_b.push_back(files[1].files[j].first);
    return result;
}

Result<std::vector<FileDigest>> digest_tree(const std::string& path, const std::string& passcode, const ProgressFn& progress) {
    GameFiles g;
    ALEMBIC_TRY(open_game_files(path, passcode, g));
    std::uint64_t total = 0, done = 0;
    for (auto& f : g.files) total += f.second->size;
    std::vector<FileDigest> out;
    out.reserve(g.files.size());
    std::vector<std::uint8_t> buffer(4 << 20);
    for (auto& [p, node] : g.files) {
        ALEMBIC_ASSIGN(auto f, g.tree->open(*node));
        Sha256 h;
        for (std::uint64_t at = 0; at < node->size;) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), node->size - at));
            ALEMBIC_TRY(f->read_at(at, MutableBytes(buffer.data(), n)));
            h.update(Bytes(buffer.data(), n));
            at += n;
            done += n;
            if (progress && !progress({"Hashing", done, total})) return make_error(Errc::cancelled, "cancelled");
        }
        out.push_back({p, node->size, h.finish()});
    }
    if (progress) progress({"Hashing", done, total});
    return out;
}

Comparison compare_digests(const std::vector<FileDigest>& a, const std::vector<FileDigest>& b) {
    Comparison result;
    std::size_t j = 0;
    for (const FileDigest& f : a) {
        while (j < b.size() && b[j].path < f.path) result.only_in_b.push_back(b[j++].path);
        if (j >= b.size() || b[j].path != f.path) {
            result.only_in_a.push_back(f.path);
            continue;
        }
        const FileDigest& o = b[j++];
        ++result.files;
        if (o.size != f.size || o.sha256 != f.sha256) result.different.push_back(f.path);
        else result.bytes += f.size;
    }
    for (; j < b.size(); ++j) result.only_in_b.push_back(b[j].path);
    return result;
}

std::string digests_to_text(const std::vector<FileDigest>& list) {
    std::string out;
    for (const FileDigest& f : list) {
        Sha256::Digest d;
        std::copy(f.sha256.begin(), f.sha256.end(), d.begin());
        std::string path; // '%', CR and LF escaped: names from images may hold anything
        for (char c : f.path) path += c == '%' ? "%25" : c == '\n' ? "%0A" : c == '\r' ? "%0D" : std::string(1, c);
        out += Sha256::hex(d) + " " + std::to_string(f.size) + " " + path + "\n";
    }
    return out;
}

Result<std::vector<FileDigest>> digests_from_text(const std::string& text) {
    std::vector<FileDigest> out;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;
        std::size_t sp1 = line.find(' '), sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
        if (sp1 != 64 || sp2 == std::string::npos) return make_error(Errc::invalid_data, "a digest line is not readable");
        FileDigest f;
        for (int i = 0; i < 32; ++i) {
            int hi = nibble(line[2 * i]), lo = nibble(line[2 * i + 1]);
            if (hi < 0 || lo < 0) return make_error(Errc::invalid_data, "a digest line is not readable");
            f.sha256[i] = static_cast<std::uint8_t>(hi << 4 | lo);
        }
        f.size = std::strtoull(line.c_str() + sp1 + 1, nullptr, 10);
        std::string escaped = line.substr(sp2 + 1);
        for (std::size_t i = 0; i < escaped.size(); ++i) {
            if (escaped.compare(i, 3, "%25") == 0) { f.path += '%'; i += 2; }
            else if (escaped.compare(i, 3, "%0A") == 0) { f.path += '\n'; i += 2; }
            else if (escaped.compare(i, 3, "%0D") == 0) { f.path += '\r'; i += 2; }
            else f.path += escaped[i];
        }
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const FileDigest& x, const FileDigest& y) { return x.path < y.path; });
    return out;
}

} // namespace alembic::convert

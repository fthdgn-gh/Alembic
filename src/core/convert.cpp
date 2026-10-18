// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/convert.hpp"

#include "alembic/bytes.hpp"
#include "alembic/exfat_reader.hpp"
#include "alembic/fpkg_inner.hpp"
#include "alembic/resume.hpp"
#include "alembic/pfs.hpp"
#include "alembic/ufs2.hpp"
#include "alembic/unicode.hpp"

#include <cstring>

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

Result<Format> detect(const std::string& path) {
    if (is_directory(path)) return Format::folder;
    if (!is_regular_file(path)) return make_error(Errc::invalid_argument, "no such file or folder: " + path);
    ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
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
        ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
        input.image = std::make_shared<File>(std::move(file));
        ALEMBIC_ASSIGN(input.tree, exfat::open_image(input.image, base_name(path)));
        return input;
    }
    case Format::ffpfsc: {
        ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
        ALEMBIC_ASSIGN(ffpfsc::Payload payload, ffpfsc::open_payload(std::make_shared<File>(std::move(file))));
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
        ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
        input.image = std::make_shared<File>(std::move(file));
        input.image_is_ufs2 = true;
        ALEMBIC_ASSIGN(input.tree, ufs2::open_image(input.image, stem(path)));
        return input;
    }
    case Format::ffpfs: {
        ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
        input.image = std::make_shared<File>(std::move(file));
        input.image_is_pfs = true;
        ALEMBIC_ASSIGN(input.tree, pfs::open_image(input.image, stem(path)));
        return input;
    }
    case Format::fpkg: {
        ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
        ALEMBIC_ASSIGN(input.tree, fpkg::open_tree(std::make_shared<File>(std::move(file)), stem(path), passcode));
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

    if (to != Format::folder && !options.overwrite && path_exists(output))
        return make_error(Errc::invalid_argument, "the output file already exists: " + output);

    ALEMBIC_ASSIGN(Input input, open(input_path, options.passcode));
    Report report{input.format, to, "", "", std::nullopt, "", {}, {}, false};
    const tree::Source* tree_ptr = input.tree.get();
    std::unique_ptr<lz4pack::PackedTree> packed;
    std::unique_ptr<tree::Source> unpacked;
    if (options.lz4 && options.lz4_unpack)
        return make_error(Errc::invalid_argument, "choose either LZ4 packing or unpacking");
    if (options.lz4_unpack) {
        if (!lz4pack::is_packed(tree_ptr->root()))
            return make_error(Errc::invalid_argument, "the input has no LZ4 packs (no ampr_assets.index)");
        ALEMBIC_ASSIGN(unpacked, lz4pack::unpack(*tree_ptr));
        tree_ptr = unpacked.get();
    } else if (options.lz4) {
        ALEMBIC_ASSIGN(packed, lz4pack::pack(*tree_ptr, *options.lz4, progress));
        report.lz4 = packed->stats();
        report.lz4_config_toml = lz4pack::to_toml(packed->config());
        report.lz4_forced_loose = packed->forced_loose_patterns();
        tree_ptr = packed.get();
    }
    const tree::Source& tree = *tree_ptr;
    // A transformed tree no longer matches the input image byte for byte.
    bool transformed = tree_ptr != input.tree.get();
    bool rebuild = options.rebuild || transformed;
    const char* prefix = packed ? "LZ4-packed and " : unpacked ? "LZ4-unpacked and " : "";
    // LZ4 packing stamps the time it runs, so it cannot continue a checkpoint;
    // neither can the FPKG writer yet.
    resume::Session* session = packed || to == Format::fpkg ? nullptr : options.resume;
    report.resumable = session != nullptr;

    switch (to) {
    case Format::folder: {
        if (input.format == Format::folder && !transformed) return unsupported(input.format, to, "copy the folder instead");
        ALEMBIC_TRY(tree::extract(tree, tree.root(), output, options.overwrite, progress, session));
        report.method = std::string(prefix) + "extracted the file tree";
        return report;
    }
    case Format::exfat: {
        if (input.format == Format::exfat && !rebuild)
            return unsupported(input.format, to, "pass --rebuild to re-lay out the image");
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
            ALEMBIC_TRY(copy_image(*input.image, output, options.overwrite, progress, session));
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
        ALEMBIC_TRY(ufs2::write_image(tree, tree.root(), output, u, options.overwrite, progress, session));
        report.method = std::string(prefix) + "built a new UFS2 image from the file tree";
        return report;
    }
    case Format::ffpfs: {
        if (input.image && input.image_is_pfs && !rebuild) {
            if (!input.ffpfsc) return unsupported(input.format, to, "pass --rebuild to re-lay out the image");
            ALEMBIC_TRY(copy_image(*input.image, output, options.overwrite, progress, session));
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
        ALEMBIC_TRY(pfs::write_image(tree, tree.root(), output, p, options.overwrite, progress, session));
        report.method = std::string(prefix) + "built a new PFS image from the file tree";
        return report;
    }
    case Format::fpkg: {
        if (input.format == Format::fpkg && !rebuild) return unsupported(input.format, to, "pass --rebuild to build it again");
        fpkg::build::PackageOptions o = options.fpkg;
        o.overwrite = options.overwrite;
        ALEMBIC_ASSIGN(fpkg::build::PackageResult r, fpkg::build::write_package(tree, tree.root(), output, o, progress));
        report.method = std::string(prefix) + "built a debug package from the file tree";
        report.detail = r.content_id;
        report.warnings = std::move(r.warnings);
        return report;
    }
    }
    return unsupported(input.format, to);
}

Result<Comparison> compare(const std::string& a, const std::string& b, const std::string& passcode,
                           const ProgressFn& progress) {
    Input inputs[2];
    std::unique_ptr<tree::Source> unpacked[2];
    const tree::Source* trees[2];
    const std::string* paths[2] = {&a, &b};
    for (int i = 0; i < 2; ++i) {
        ALEMBIC_ASSIGN(inputs[i], open(*paths[i], passcode));
        trees[i] = inputs[i].tree.get();
        if (lz4pack::is_packed(trees[i]->root())) {
            ALEMBIC_ASSIGN(unpacked[i], lz4pack::unpack(*trees[i]));
            trees[i] = unpacked[i].get();
        }
    }
    std::vector<std::pair<std::string, const tree::Node*>> files[2];
    for (int i = 0; i < 2; ++i) {
        std::vector<std::pair<std::string, const tree::Node*>> stack{{"", &trees[i]->root()}};
        while (!stack.empty()) {
            auto [prefix, node] = stack.back();
            stack.pop_back();
            for (const tree::Node& c : node->children) {
                std::string path = prefix.empty() ? c.name : prefix + "/" + c.name;
                if (c.is_directory) stack.push_back({path, &c});
                else if (path != "ampr_emu.index") files[i].push_back({path, &c});
            }
        }
        std::sort(files[i].begin(), files[i].end());
    }
    Comparison result;
    std::uint64_t total = 0, done = 0;
    for (auto& f : files[0]) total += f.second->size;
    std::size_t j = 0;
    std::vector<std::uint8_t> x(4 << 20), y(4 << 20);
    for (auto& [path, node] : files[0]) {
        while (j < files[1].size() && files[1][j].first < path) result.only_in_b.push_back(files[1][j++].first);
        if (j >= files[1].size() || files[1][j].first != path) {
            result.only_in_a.push_back(path);
            done += node->size;
            continue;
        }
        const tree::Node* other = files[1][j++].second;
        ++result.files;
        if (other->size != node->size) {
            result.different.push_back(path);
            done += node->size;
            continue;
        }
        ALEMBIC_ASSIGN(auto fa, trees[0]->open(*node));
        ALEMBIC_ASSIGN(auto fb, trees[1]->open(*other));
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
    for (; j < files[1].size(); ++j) result.only_in_b.push_back(files[1][j].first);
    return result;
}

} // namespace alembic::convert

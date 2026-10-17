// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/convert.hpp"

#include "alembic/bytes.hpp"
#include "alembic/exfat_reader.hpp"
#include "alembic/fpkg_inner.hpp"
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

Status copy_image(const RandomAccess& image, const std::string& output, bool overwrite, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(std::uint64_t length, image.size());
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(output, overwrite));
    std::vector<std::uint8_t> buffer(1 << 20);
    for (std::uint64_t done = 0; done < length;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - done));
        ALEMBIC_TRY(image.read_at(done, MutableBytes(buffer.data(), n)));
        ALEMBIC_TRY(out.file().write_at(done, Bytes(buffer.data(), n)));
        done += n;
        if (progress && !progress({"Copying image", done, length})) return make_error(Errc::cancelled, "cancelled");
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
    }
    return "?";
}

std::optional<Format> parse_format(const std::string& text) {
    if (unicode::ascii_iequals(text, "folder") || unicode::ascii_iequals(text, "dir")) return Format::folder;
    if (unicode::ascii_iequals(text, "exfat")) return Format::exfat;
    if (unicode::ascii_iequals(text, "ffpfsc")) return Format::ffpfsc;
    if (unicode::ascii_iequals(text, "fpkg") || unicode::ascii_iequals(text, "pkg")) return Format::fpkg;
    if (unicode::ascii_iequals(text, "ffpkg") || unicode::ascii_iequals(text, "ufs2")) return Format::ffpkg;
    return std::nullopt;
}

std::optional<Format> guess_output_format(const std::string& path) {
    if (!path.empty() && path.back() == '/') return Format::folder;
    if (ends_with_icase(path, ".exfat") || ends_with_icase(path, ".img")) return Format::exfat;
    if (ends_with_icase(path, ".ffpfsc")) return Format::ffpfsc;
    if (ends_with_icase(path, ".ffpkg")) return Format::ffpkg;
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
    if (size >= 0x10 && get_u64(head) == 2 && get_u64(head + 8) == kPfsMagic) return Format::ffpfsc;
    if (exfat::looks_like_exfat(file)) return Format::exfat;
    if (ufs2::looks_like_ufs2(file)) return Format::ffpkg;
    return make_error(Errc::unsupported, "unrecognised input (not a folder, exFAT, UFS2, FFPFSC or FPKG): " + path);
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
        } else {
            return make_error(Errc::unsupported, "the FFPFSC payload '" + payload.info.inner_file_name +
                                                     "' is neither exFAT nor UFS2 (nested PFS is not supported yet)");
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
    if (to == Format::fpkg) return make_error(Errc::unsupported, "writing FPKG files is not supported yet");

    if (to != Format::folder && !options.overwrite && path_exists(output))
        return make_error(Errc::invalid_argument, "the output file already exists: " + output);

    ALEMBIC_ASSIGN(Input input, open(input_path, options.passcode));
    Report report{input.format, to, "", ""};
    const tree::Source& tree = *input.tree;

    switch (to) {
    case Format::folder: {
        if (input.format == Format::folder) return unsupported(input.format, to, "copy the folder instead");
        ALEMBIC_TRY(tree::extract(tree, tree.root(), output, options.overwrite, progress));
        report.method = "extracted the file tree";
        return report;
    }
    case Format::exfat: {
        if (input.format == Format::exfat && !options.rebuild)
            return unsupported(input.format, to, "pass --rebuild to re-lay out the image");
        if (input.image && !input.image_is_ufs2 && !options.rebuild) {
            ALEMBIC_TRY(copy_image(*input.image, output, options.overwrite, progress));
            report.method = "unwrapped the exFAT image unchanged";
            return report;
        }
        ALEMBIC_TRY(exfat::write_image(tree, tree.root(), output, options.exfat, options.overwrite, progress));
        report.method = "built a new exFAT image from the file tree";
        return report;
    }
    case Format::ffpfsc: {
        ffpfsc::BuildOptions ff = options.ffpfsc;
        ff.overwrite_existing = options.overwrite;
        if (input.format == Format::folder) {
            ALEMBIC_ASSIGN(ffpfsc::BuildResult r,
                           ffpfsc::create_from_directory(input_path, output, ff, options.exfat, progress));
            report.method = "built a new exFAT image from the folder and compressed it";
            report.detail = r.inner_file_name;
            return report;
        }
        if (input.image && !options.rebuild) {
            std::string inner = input.ffpfsc ? input.ffpfsc->inner_file_name : base_name(input_path);
            ALEMBIC_ASSIGN(ffpfsc::BuildResult r, ffpfsc::create_from_payload(*input.image, inner, output, ff, progress));
            const char* kind = input.image_is_ufs2 ? "UFS2" : "exFAT";
            report.method = std::string(input.ffpfsc ? "recompressed the " : "compressed the ") + kind + " image unchanged";
            report.detail = r.inner_file_name;
            return report;
        }
        ALEMBIC_ASSIGN(ffpfsc::BuildResult r,
                       ffpfsc::create_from_tree(tree, tree.root(), output, ff, options.exfat, progress));
        report.method = "built a new exFAT image from the file tree and compressed it";
        report.detail = r.inner_file_name;
        return report;
    }
    case Format::ffpkg: {
        // Only unwrapping is possible until there is a UFS2 writer.
        if (input.image && input.image_is_ufs2 && input.ffpfsc && !options.rebuild) {
            ALEMBIC_TRY(copy_image(*input.image, output, options.overwrite, progress));
            report.method = "unwrapped the UFS2 image unchanged";
            return report;
        }
        return make_error(Errc::unsupported, "writing FFPKG (UFS2) images is not supported yet");
    }
    case Format::fpkg: break;
    }
    return unsupported(input.format, to);
}

} // namespace alembic::convert

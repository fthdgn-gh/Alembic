// SPDX-License-Identifier: GPL-3.0-or-later
// alembic: command-line front end for the core library.
#include "alembic/ampr.hpp"
#include "alembic/convert.hpp"
#include "alembic/exfat.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/fpkg_inner.hpp"
#include "alembic/unicode.hpp"
#include "alembic/io.hpp"
#include "alembic/pfsc.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace alembic;

namespace {

void usage() {
    std::fputs(
        "usage: alembic <command> [options]\n"
        "\n"
        "commands:\n"
        "  convert <input> <output> [--to folder|exfat|ffpfsc] [--rebuild] [--overwrite] [options]\n"
        "        convert a dump folder, exFAT, FFPKG (UFS2), FFPFSC or debug FPKG\n"
        "        (--passcode P) to a folder, exFAT or FFPFSC; the input format is\n"
        "        detected, the output format comes from --to or the output extension\n"
        "        (.exfat/.img, .ffpfsc, or a trailing '/' for a folder). Images are\n"
        "        copied unchanged unless --rebuild asks for a fresh exFAT layout.\n"
        "        Takes the ffpfsc and exfat options below.\n"
        "  ls <input> [--passcode P]\n"
        "        list the files in a folder, exFAT, FFPKG, FFPFSC or debug FPKG\n"
        "  exfat <dump-dir> <out.exfat> [--cluster-size N] [--no-ampr] [--ampr-scheme S]\n"
        "        build an exFAT image from a dump folder\n"
        "  ampr-index <dump-dir> <out.index> [--ampr-scheme S]\n"
        "        build ampr_emu.index for a dump folder\n"
        "  ffpfsc <dump-dir|image> <out.ffpfsc> [options]\n"
        "        pack a dump folder (streamed, no temporary image) or an image file\n"
        "        --inner-name NAME   payload name (default <TITLEID>.exfat or the file name)\n"
        "        --timestamp UNIX    fixed PFS timestamp (default: now)\n"
        "        --block-size N      PFS block size (default 65536)\n"
        "        --case-sensitive    clear the PFS case-insensitive flag\n"
        "        --overwrite         replace an existing output file\n"
        "        plus --level, --min-gain, --threads N (0 = all cores, the default),\n"
        "        and for folders the exfat options\n"
        "  ffpfsc-info <in.ffpfsc>\n"
        "        print the PFS and PFSC layout\n"
        "  ffpfsc-extract <in.ffpfsc> <out-image>\n"
        "        write the decoded payload (the inner exFAT/UFS2 image)\n"
        "  ffpfsc-verify <in.ffpfsc> [--original IMAGE]\n"
        "        decode every block, print the payload SHA-256, compare with IMAGE\n"
        "  fpkg-info <pkg> [--passcode P]\n"
        "        show an FPKG's header, CNT entries, digest checks and outer files\n"
        "  fpkg-dump <pkg> <what> <out> [--passcode P]\n"
        "        write decrypted data: <what> is mount (the decoded inner PFS image),\n"
        "        inner-image (pfs_image.dat as stored),\n"
        "        an outer file name (e.g. naps_pkg_layout.dat), or entry:<name|0xID>\n"
        "  pfsc-encode <in> <out> [--level 0-9 (default 6)] [--min-gain PERCENT] [--threads N]\n"
        "        wrap a file in a PFSC block container\n"
        "  pfsc-decode <in> <out> [--length N]\n"
        "        unpack a PFSC container (by default to its padded length)\n"
        "  pfsc-info <in>\n"
        "        print PFSC header details\n"
        "\n"
        "--ampr-scheme ampr-emu    hash names the way ampr_emu reads them (default)\n"
        "--ampr-scheme ps5pkgtool  match PS5PKGTool byte for byte; its index is\n"
        "                          rejected by ampr_emu when a name is not ASCII\n",
        stderr);
}

class ProgressPrinter {
public:
    ProgressFn callback() {
        // Live percentages only on a terminal; logs and pipes stay clean.
        if (!::isatty(STDERR_FILENO)) return {};
        return [this](const ProgressEvent& event) {
            int percent = event.total == 0 ? 100 : static_cast<int>(event.done * 100 / event.total);
            if (percent != last_percent_ || event.stage != stage_) {
                if (last_percent_ >= 0 && event.stage != stage_) std::fputc('\n', stderr);
                last_percent_ = percent;
                stage_ = std::string(event.stage);
                std::fprintf(stderr, "\r%s: %3d%%", stage_.c_str(), percent);
                std::fflush(stderr);
            }
            return true;
        };
    }
    void done() {
        if (last_percent_ >= 0) std::fputc('\n', stderr);
    }

private:
    int last_percent_ = -1;
    std::string stage_;
};

int fail(const Error& error) {
    std::fprintf(stderr, "\nerror: %s\n", error.message.c_str());
    return 1;
}

std::optional<std::uint64_t> parse_number(const char* text) {
    char* end = nullptr;
    errno = 0;
    unsigned long long value = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') return std::nullopt;
    return value;
}

// Splits arguments into positionals and `--name value` / `--flag` options.
struct Args {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> options;

    std::optional<std::string> get(const std::string& name) const {
        for (const auto& [key, value] : options)
            if (key == name) return value;
        return std::nullopt;
    }
};

Args parse(int argc, char** argv, const std::vector<std::string>& flags) {
    Args args;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            bool is_flag = std::find(flags.begin(), flags.end(), arg) != flags.end();
            std::string value = (!is_flag && i + 1 < argc) ? argv[++i] : "";
            args.options.emplace_back(arg, value);
        } else {
            args.positional.push_back(arg);
        }
    }
    return args;
}

bool parse_scheme(const Args& args, ampr::Scheme& scheme) {
    auto text = args.get("--ampr-scheme");
    if (!text) return true;
    if (*text == "ampr-emu") scheme = ampr::Scheme::ampr_emu;
    else if (*text == "ps5pkgtool") scheme = ampr::Scheme::ps5pkgtool;
    else return std::fputs("error: --ampr-scheme must be ampr-emu or ps5pkgtool\n", stderr), false;
    return true;
}

bool parse_compression(const Args& args, pfsc::CompressionOptions& options) {
    if (auto threads = args.get("--threads")) {
        auto value = parse_number(threads->c_str());
        if (!value || *value > 64) return std::fputs("error: bad --threads (0-64, 0 = all cores)\n", stderr), false;
        options.threads = static_cast<unsigned>(*value);
    }
    if (auto level = args.get("--level")) {
        auto value = parse_number(level->c_str());
        if (!value || *value > 9) return std::fputs("error: bad --level\n", stderr), false;
        options.level = static_cast<int>(*value);
    }
    if (auto gain = args.get("--min-gain")) {
        auto value = parse_number(gain->c_str());
        if (!value || *value > 100) return std::fputs("error: bad --min-gain\n", stderr), false;
        options.minimum_gain_percent = static_cast<int>(*value);
    }
    return true;
}

bool parse_exfat_options(const Args& args, exfat::BuildOptions& options) {
    if (auto size = args.get("--cluster-size")) {
        auto value = parse_number(size->c_str());
        if (!value || *value > UINT32_MAX) return std::fputs("error: bad --cluster-size\n", stderr), false;
        options.cluster_size = static_cast<std::uint32_t>(*value);
    }
    if (args.get("--no-ampr")) options.generate_ampr_index = false;
    return parse_scheme(args, options.ampr_scheme);
}

int cmd_ampr_index(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    ampr::Scheme scheme = ampr::Scheme::ampr_emu;
    if (!parse_scheme(args, scheme)) return 2;
    auto index = exfat::build_ampr_index(args.positional[0], scheme);
    if (!index.ok()) return fail(index.error());
    if (index->empty()) return std::fputs("error: the folder has no files to index\n", stderr), 1;
    auto output = AtomicOutput::create(args.positional[1]);
    if (!output.ok()) return fail(output.error());
    Status status = output->file().write_at(0, *index);
    if (status.ok()) status = output->commit();
    if (!status.ok()) return fail(status.error());
    return 0;
}

bool parse_ffpfsc_options(const Args& args, ffpfsc::BuildOptions& options);

int cmd_exfat(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    exfat::BuildOptions options;
    if (!parse_exfat_options(args, options)) return 2;
    ProgressPrinter printer;
    Status status = exfat::write_directory_image(args.positional[0], args.positional[1], options, printer.callback());
    printer.done();
    if (!status.ok()) return fail(status.error());
    return 0;
}

int cmd_ffpfsc(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    ffpfsc::BuildOptions options;
    exfat::BuildOptions exfat_options;
    if (!parse_ffpfsc_options(args, options) || !parse_exfat_options(args, exfat_options)) return 2;
    if (args.get("--overwrite")) options.overwrite_existing = true;

    const std::string& source = args.positional[0];
    ProgressPrinter printer;
    auto result = is_directory(source)
        ? ffpfsc::create_from_directory(source, args.positional[1], options, exfat_options, printer.callback())
        : ffpfsc::create_from_image(source, args.positional[1], options, printer.callback());
    printer.done();
    if (!result.ok()) return fail(result.error());
    if (!result->game_root.empty() && result->game_root != ".")
        std::printf("game root:   %s\n", result->game_root.c_str());
    std::printf("inner name:  %s\n", result->inner_file_name.c_str());
    std::printf("payload:     %llu bytes -> %llu stored (%u of %u blocks compressed)\n",
                static_cast<unsigned long long>(result->source_length),
                static_cast<unsigned long long>(result->pfsc_stored_length), result->compressed_block_count,
                result->pfsc_block_count);
    std::printf("container:   %llu bytes\n", static_cast<unsigned long long>(result->container_length));
    return 0;
}

bool parse_ffpfsc_options(const Args& args, ffpfsc::BuildOptions& options) {
    if (!parse_compression(args, options.compression)) return false;
    if (auto name = args.get("--inner-name")) options.inner_file_name = *name;
    if (auto text = args.get("--timestamp")) {
        auto value = parse_number(text->c_str());
        if (!value || *value > INT64_MAX) return std::fputs("error: bad --timestamp\n", stderr), false;
        options.build_timestamp = static_cast<std::int64_t>(*value);
    }
    if (auto text = args.get("--block-size")) {
        auto value = parse_number(text->c_str());
        if (!value || *value > UINT32_MAX) return std::fputs("error: bad --block-size\n", stderr), false;
        options.pfs_block_size = static_cast<std::uint32_t>(*value);
    }
    if (args.get("--case-sensitive")) options.case_insensitive = false;
    return true;
}

int cmd_convert(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    convert::Options options;
    if (!parse_ffpfsc_options(args, options.ffpfsc) || !parse_exfat_options(args, options.exfat)) return 2;
    options.rebuild = args.get("--rebuild").has_value();
    if (auto passcode = args.get("--passcode")) options.passcode = *passcode;
    options.overwrite = args.get("--overwrite").has_value();
    std::optional<convert::Format> to;
    if (auto text = args.get("--to")) {
        to = convert::parse_format(*text);
        if (!to) return std::fputs("error: --to must be folder, exfat or ffpfsc\n", stderr), 2;
    } else {
        to = convert::guess_output_format(args.positional[1]);
        if (!to) return std::fputs("error: cannot tell the output format from its name; pass --to\n", stderr), 2;
    }
    ProgressPrinter printer;
    auto report = convert::run(args.positional[0], args.positional[1], *to, options, printer.callback());
    printer.done();
    if (!report.ok()) return fail(report.error());
    std::printf("%s -> %s: %s\n", convert::name(report->from), convert::name(report->to), report->method.c_str());
    if (!report->detail.empty()) std::printf("inner name: %s\n", report->detail.c_str());
    return 0;
}

void list_tree(const tree::Node& node, const std::string& prefix) {
    for (const tree::Node& child : node.children) {
        std::string path = prefix.empty() ? child.name : prefix + "/" + child.name;
        if (child.is_directory) {
            std::printf("%14s  %s/\n", "", path.c_str());
            list_tree(child, path);
        } else {
            std::printf("%14llu  %s\n", static_cast<unsigned long long>(child.size), path.c_str());
        }
    }
}

int cmd_ls(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    auto input = convert::open(args.positional[0], args.get("--passcode").value_or(std::string(32, '0')));
    if (!input.ok()) return fail(input.error());
    list_tree(input->tree->root(), "");
    tree::Totals t = tree::totals(input->tree->root());
    std::printf("%s: %llu files, %llu folders, %llu bytes\n", convert::name(input->format),
                static_cast<unsigned long long>(t.files), static_cast<unsigned long long>(t.directories),
                static_cast<unsigned long long>(t.bytes));
    return 0;
}

int cmd_fpkg_info(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    auto opened = File::open(args.positional[0], File::Mode::read);
    if (!opened.ok()) return fail(opened.error());
    std::shared_ptr<const RandomAccess> file = std::make_shared<File>(std::move(*opened));
    auto package = fpkg::read_package(*file);
    if (!package.ok()) return fail(package.error());
    const fpkg::Package& p = *package;
    std::printf("kind:            %s\n", fpkg::name(p.kind));
    std::printf("content id:      %s\n", p.content_id.c_str());
    std::printf("content type:    0x%x  drm type: 0x%x  flags: 0x%x\n", p.content_type, p.drm_type, p.content_flags);
    for (const fpkg::Segment& s : p.segments)
        std::printf("segment %-4s     0x%llx + 0x%llx\n", s.name.c_str(), static_cast<unsigned long long>(s.offset),
                    static_cast<unsigned long long>(s.size));
    for (const std::string& w : p.warnings) std::printf("warning:         %s\n", w.c_str());
    std::printf("CNT entries:\n");
    for (const fpkg::CntEntry& e : p.entries)
        std::printf("  0x%08X  0x%08X bytes  %s  %s\n", e.id, e.data_size, e.encrypted() ? "protected" : "plain    ",
                    e.name.c_str());
    auto digests = fpkg::check_digests(*file, p);
    if (digests.ok()) {
        std::printf("FIH digest:      %s\n", digests->fih_digest_ok ? "ok" : "MISMATCH");
        std::printf("entry digests:   %s%s%s\n", digests->entry_digests_ok ? "ok" : "MISMATCH",
                    digests->detail.empty() ? "" : " - ", digests->detail.c_str());
    }
    if (p.kind != fpkg::Kind::finalized_debug) return 0;
    auto pfs = fpkg::OuterPfs::open(file, p, args.get("--passcode").value_or(fpkg::kDefaultPasscode));
    if (!pfs.ok()) return fail(pfs.error());
    std::printf("outer PFS:       %u blocks, superblock at %u, %s, ICV %s\n", (*pfs)->block_count(),
                (*pfs)->superblock_index(), (*pfs)->plain_no_auth() ? "unencrypted" : "AES-XTS",
                (*pfs)->superblock_icv_ok() ? "ok" : "MISMATCH");
    for (const fpkg::OuterFileInfo& f : (*pfs)->files())
        std::printf("  %-24s %s size %llu (logical %llu)\n", f.name.c_str(), f.is_directory ? "dir " : "file",
                    static_cast<unsigned long long>(f.size), static_cast<unsigned long long>(f.size_compressed));
    return 0;
}

int cmd_fpkg_dump(const Args& args) {
    if (args.positional.size() != 3) return usage(), 2;
    auto opened = File::open(args.positional[0], File::Mode::read);
    if (!opened.ok()) return fail(opened.error());
    std::shared_ptr<const RandomAccess> file = std::make_shared<File>(std::move(*opened));
    auto package = fpkg::read_package(*file);
    if (!package.ok()) return fail(package.error());
    const std::string& what = args.positional[1];
    std::string passcode = args.get("--passcode").value_or(fpkg::kDefaultPasscode);
    std::shared_ptr<const RandomAccess> source;
    if (what.rfind("entry:", 0) == 0) {
        std::string key = what.substr(6);
        const fpkg::CntEntry* entry = nullptr;
        for (const fpkg::CntEntry& e : package->entries) {
            char id[16];
            std::snprintf(id, sizeof id, "0x%x", e.id);
            if (e.name == key || unicode::ascii_iequals(id, key)) entry = &e;
        }
        if (!entry) return std::fprintf(stderr, "error: no CNT entry '%s'\n", key.c_str()), 1;
        auto data = fpkg::read_entry(*file, *package, *entry, passcode);
        if (!data.ok()) return fail(data.error());
        auto out = AtomicOutput::create(args.positional[2]);
        if (!out.ok()) return fail(out.error());
        Status status = out->file().write_at(0, *data);
        if (status.ok()) status = out->commit();
        return status.ok() ? 0 : fail(status.error());
    }
    auto pfs = fpkg::OuterPfs::open(file, *package, passcode);
    if (!pfs.ok()) return fail(pfs.error());
    Result<std::shared_ptr<const RandomAccess>> opened_source = make_error(Errc::invalid_argument, "");
    if (what == "mount") {
        auto opened_package = fpkg::open_package(file, passcode);
        if (!opened_package.ok()) return fail(opened_package.error());
        std::printf("layout: %s, %zu blocks, mount %llu bytes\n", opened_package->plan.dialect.c_str(),
                    opened_package->plan.blocks.size(),
                    static_cast<unsigned long long>(opened_package->plan.mount_size));
        opened_source = std::shared_ptr<const RandomAccess>(opened_package->mount);
    } else {
        opened_source = what == "inner-image" ? (*pfs)->open_inner_image() : (*pfs)->open_file(what);
    }
    if (!opened_source.ok()) return fail(opened_source.error());
    auto length = (*opened_source)->size();
    auto out = AtomicOutput::create(args.positional[2]);
    if (!out.ok()) return fail(out.error());
    std::vector<std::uint8_t> buffer(1 << 20);
    for (std::uint64_t done = 0; done < *length;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), *length - done));
        Status status = (*opened_source)->read_at(done, MutableBytes(buffer.data(), n));
        if (status.ok()) status = out->file().write_at(done, Bytes(buffer.data(), n));
        if (!status.ok()) return fail(status.error());
        done += n;
    }
    Status status = out->commit();
    return status.ok() ? 0 : fail(status.error());
}

int cmd_ffpfsc_info(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    auto input = File::open(args.positional[0], File::Mode::read);
    if (!input.ok()) return fail(input.error());
    auto info = ffpfsc::inspect(*input);
    if (!info.ok()) return fail(info.error());
    std::printf("PFS:             version %u, mode 0x%x, %u-byte blocks, %llu blocks\n", info->pfs_version,
                info->pfs_mode, info->pfs_block_size, static_cast<unsigned long long>(info->pfs_block_count));
    std::printf("timestamp:       %lld\n", static_cast<long long>(info->timestamp));
    std::printf("payload:         %s\n", info->inner_file_name.c_str());
    std::printf("payload offset:  0x%llx\n", static_cast<unsigned long long>(info->payload_offset));
    std::printf("logical length:  %llu\n", static_cast<unsigned long long>(info->logical_length));
    std::printf("stored length:   %llu\n", static_cast<unsigned long long>(info->stored_length));
    std::uint32_t compressed = 0;
    for (std::uint32_t i = 0; i < info->pfsc.block_count(); ++i)
        if (info->pfsc.block_offsets[i + 1] - info->pfsc.block_offsets[i] < pfsc::kBlockSize) ++compressed;
    std::printf("PFSC blocks:     %u (%u compressed)\n", info->pfsc.block_count(), compressed);
    return 0;
}

int cmd_ffpfsc_extract(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    ProgressPrinter printer;
    Status status = ffpfsc::extract_payload(args.positional[0], args.positional[1], printer.callback());
    printer.done();
    if (!status.ok()) return fail(status.error());
    return 0;
}

int cmd_ffpfsc_verify(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    ProgressPrinter printer;
    auto result = ffpfsc::verify(args.positional[0], args.get("--original"), printer.callback());
    printer.done();
    if (!result.ok()) return fail(result.error());
    std::printf("structure:       ok\nevery block:     decodes\n");
    std::printf("payload sha256:  %s\n", result->decoded_sha256.c_str());
    if (result->source_matches) {
        std::printf("original sha256: %s\n", result->source_sha256->c_str());
        std::printf("matches:         %s\n", *result->source_matches ? "yes" : "NO");
        return *result->source_matches ? 0 : 1;
    }
    return 0;
}

int cmd_pfsc_encode(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    pfsc::CompressionOptions options;
    if (!parse_compression(args, options)) return 2;
    auto input = File::open(args.positional[0], File::Mode::read);
    if (!input.ok()) return fail(input.error());
    auto output = AtomicOutput::create(args.positional[1]);
    if (!output.ok()) return fail(output.error());
    ProgressPrinter printer;
    auto result = pfsc::encode_file(*input, output->file(), 0, options, printer.callback());
    printer.done();
    if (!result.ok()) return fail(result.error());
    Status committed = output->commit();
    if (!committed.ok()) return fail(committed.error());
    std::printf("blocks=%u compressed=%u stored=%llu\n", result->block_count, result->compressed_block_count,
                static_cast<unsigned long long>(result->stored_length));
    return 0;
}

int cmd_pfsc_decode(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    std::optional<std::uint64_t> length;
    if (auto text = args.get("--length")) {
        length = parse_number(text->c_str());
        if (!length) return std::fputs("error: bad --length\n", stderr), 2;
    }
    auto input = File::open(args.positional[0], File::Mode::read);
    if (!input.ok()) return fail(input.error());
    auto output = AtomicOutput::create(args.positional[1]);
    if (!output.ok()) return fail(output.error());
    FileSink sink(output->file(), 0);
    ProgressPrinter printer;
    Status status = pfsc::decode(*input, sink, length, 0, std::nullopt, printer.callback());
    printer.done();
    if (!status.ok()) return fail(status.error());
    status = output->commit();
    if (!status.ok()) return fail(status.error());
    return 0;
}

int cmd_pfsc_info(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    auto input = File::open(args.positional[0], File::Mode::read);
    if (!input.ok()) return fail(input.error());
    auto info = pfsc::inspect(*input);
    if (!info.ok()) return fail(info.error());
    std::uint32_t compressed = 0;
    for (std::uint32_t i = 0; i < info->block_count(); ++i)
        if (info->block_offsets[i + 1] - info->block_offsets[i] < pfsc::kBlockSize) ++compressed;
    std::printf("data offset:     0x%llx\n", static_cast<unsigned long long>(info->data_offset));
    std::printf("logical length:  %llu\n", static_cast<unsigned long long>(info->padded_logical_length));
    std::printf("stored length:   %llu\n", static_cast<unsigned long long>(info->block_offsets.back()));
    std::printf("blocks:          %u (%u compressed)\n", info->block_count(), compressed);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(), 2;
    std::string command = argv[1];
    if (command == "convert")
        return cmd_convert(parse(argc, argv, {"--no-ampr", "--case-sensitive", "--overwrite", "--rebuild"}));
    if (command == "ls") return cmd_ls(parse(argc, argv, {}));
    if (command == "exfat") return cmd_exfat(parse(argc, argv, {"--no-ampr"}));
    if (command == "ampr-index") return cmd_ampr_index(parse(argc, argv, {}));
    if (command == "ffpfsc") return cmd_ffpfsc(parse(argc, argv, {"--no-ampr", "--case-sensitive", "--overwrite"}));
    if (command == "ffpfsc-info") return cmd_ffpfsc_info(parse(argc, argv, {}));
    if (command == "ffpfsc-extract") return cmd_ffpfsc_extract(parse(argc, argv, {}));
    if (command == "ffpfsc-verify") return cmd_ffpfsc_verify(parse(argc, argv, {}));
    if (command == "fpkg-info") return cmd_fpkg_info(parse(argc, argv, {}));
    if (command == "fpkg-dump") return cmd_fpkg_dump(parse(argc, argv, {}));
    if (command == "pfsc-encode") return cmd_pfsc_encode(parse(argc, argv, {}));
    if (command == "pfsc-decode") return cmd_pfsc_decode(parse(argc, argv, {}));
    if (command == "pfsc-info") return cmd_pfsc_info(parse(argc, argv, {}));
    usage();
    return 2;
}

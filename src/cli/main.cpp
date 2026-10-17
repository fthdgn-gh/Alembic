// SPDX-License-Identifier: GPL-3.0-or-later
// alembic: command-line front end for the core library.
#include "alembic/ampr.hpp"
#include "alembic/exfat.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/io.hpp"
#include "alembic/pfsc.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
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
        "        plus --level, --min-gain, and for folders the exfat options\n"
        "  ffpfsc-info <in.ffpfsc>\n"
        "        print the PFS and PFSC layout\n"
        "  ffpfsc-extract <in.ffpfsc> <out-image>\n"
        "        write the decoded payload (the inner exFAT/UFS2 image)\n"
        "  ffpfsc-verify <in.ffpfsc> [--original IMAGE]\n"
        "        decode every block, print the payload SHA-256, compare with IMAGE\n"
        "  pfsc-encode <in> <out> [--level 0-9] [--min-gain PERCENT]\n"
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
        return [this](const ProgressEvent& event) {
            int percent = event.total == 0 ? 100 : static_cast<int>(event.done * 100 / event.total);
            if (percent != last_percent_ || event.stage != stage_) {
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
    if (!parse_compression(args, options.compression) || !parse_exfat_options(args, exfat_options)) return 2;
    if (auto name = args.get("--inner-name")) options.inner_file_name = *name;
    if (auto text = args.get("--timestamp")) {
        auto value = parse_number(text->c_str());
        if (!value || *value > INT64_MAX) return std::fputs("error: bad --timestamp\n", stderr), 2;
        options.build_timestamp = static_cast<std::int64_t>(*value);
    }
    if (auto text = args.get("--block-size")) {
        auto value = parse_number(text->c_str());
        if (!value || *value > UINT32_MAX) return std::fputs("error: bad --block-size\n", stderr), 2;
        options.pfs_block_size = static_cast<std::uint32_t>(*value);
    }
    if (args.get("--case-sensitive")) options.case_insensitive = false;
    if (args.get("--overwrite")) options.overwrite_existing = true;

    const std::string& source = args.positional[0];
    ProgressPrinter printer;
    auto result = is_directory(source)
        ? ffpfsc::create_from_directory(source, args.positional[1], options, exfat_options, printer.callback())
        : ffpfsc::create_from_image(source, args.positional[1], options, printer.callback());
    printer.done();
    if (!result.ok()) return fail(result.error());
    if (!result->game_root.empty()) std::printf("game root:   %s\n", result->game_root.c_str());
    std::printf("inner name:  %s\n", result->inner_file_name.c_str());
    std::printf("payload:     %llu bytes -> %llu stored (%u of %u blocks compressed)\n",
                static_cast<unsigned long long>(result->source_length),
                static_cast<unsigned long long>(result->pfsc_stored_length), result->compressed_block_count,
                result->pfsc_block_count);
    std::printf("container:   %llu bytes\n", static_cast<unsigned long long>(result->container_length));
    return 0;
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
    if (command == "exfat") return cmd_exfat(parse(argc, argv, {"--no-ampr"}));
    if (command == "ampr-index") return cmd_ampr_index(parse(argc, argv, {}));
    if (command == "ffpfsc") return cmd_ffpfsc(parse(argc, argv, {"--no-ampr", "--case-sensitive", "--overwrite"}));
    if (command == "ffpfsc-info") return cmd_ffpfsc_info(parse(argc, argv, {}));
    if (command == "ffpfsc-extract") return cmd_ffpfsc_extract(parse(argc, argv, {}));
    if (command == "ffpfsc-verify") return cmd_ffpfsc_verify(parse(argc, argv, {}));
    if (command == "pfsc-encode") return cmd_pfsc_encode(parse(argc, argv, {}));
    if (command == "pfsc-decode") return cmd_pfsc_decode(parse(argc, argv, {}));
    if (command == "pfsc-info") return cmd_pfsc_info(parse(argc, argv, {}));
    usage();
    return 2;
}

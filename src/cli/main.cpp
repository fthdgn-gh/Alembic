// SPDX-License-Identifier: GPL-3.0-or-later
// alembic: command-line front end for the core library.
#include "alembic/exfat.hpp"
#include "alembic/io.hpp"
#include "alembic/pfsc.hpp"

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
        "  exfat <dump-dir> <out.exfat> [--cluster-size N] [--no-ampr]\n"
        "        build an exFAT image from a dump folder\n"
        "  pfsc-encode <in> <out> [--level 0-9] [--min-gain PERCENT]\n"
        "        wrap a file in a PFSC block container\n"
        "  pfsc-decode <in> <out> [--length N]\n"
        "        unpack a PFSC container (by default to its padded length)\n"
        "  pfsc-info <in>\n"
        "        print PFSC header details\n",
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

int cmd_exfat(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    exfat::BuildOptions options;
    if (auto size = args.get("--cluster-size")) {
        auto value = parse_number(size->c_str());
        if (!value || *value > UINT32_MAX) return std::fputs("error: bad --cluster-size\n", stderr), 2;
        options.cluster_size = static_cast<std::uint32_t>(*value);
    }
    if (args.get("--no-ampr")) options.generate_ampr_index = false;
    ProgressPrinter printer;
    Status status = exfat::write_directory_image(args.positional[0], args.positional[1], options, printer.callback());
    printer.done();
    if (!status.ok()) return fail(status.error());
    return 0;
}

int cmd_pfsc_encode(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    pfsc::CompressionOptions options;
    if (auto level = args.get("--level")) {
        auto value = parse_number(level->c_str());
        if (!value || *value > 9) return std::fputs("error: bad --level\n", stderr), 2;
        options.level = static_cast<int>(*value);
    }
    if (auto gain = args.get("--min-gain")) {
        auto value = parse_number(gain->c_str());
        if (!value || *value > 100) return std::fputs("error: bad --min-gain\n", stderr), 2;
        options.minimum_gain_percent = static_cast<int>(*value);
    }
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
    if (command == "pfsc-encode") return cmd_pfsc_encode(parse(argc, argv, {}));
    if (command == "pfsc-decode") return cmd_pfsc_decode(parse(argc, argv, {}));
    if (command == "pfsc-info") return cmd_pfsc_info(parse(argc, argv, {}));
    usage();
    return 2;
}

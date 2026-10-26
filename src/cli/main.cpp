// SPDX-License-Identifier: GPL-3.0-or-later
// alembic: command-line front end for the core library.
#include "alembic/ampr.hpp"
#include "alembic/convert.hpp"
#include "alembic/fpkg_build.hpp"
#include "alembic/json.hpp"
#include "alembic/exfat.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/fpkg_inner.hpp"
#include "alembic/lz4pack.hpp"
#include "alembic/unicode.hpp"
#include "alembic/io.hpp"
#include "alembic/pfsc.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
        "  convert <input> <output> [--to folder|exfat|ffpkg|ffpfs|ffpfsc|fpkg] [--rebuild] [--overwrite] [options]\n"
        "        convert a dump folder, exFAT, FFPKG (UFS2), FFPFS (PFS), FFPFSC or debug\n"
        "        FPKG (--passcode P) to a folder, exFAT, FFPKG, FFPFS, FFPFSC or FPKG; the input format is\n"
        "        detected, the output format comes from --to or the output extension\n"
        "        (.ffpkg: UFS2, byte-identical to PS5PKGTool; --ufs2-timestamp, --ufs2-pid and\n"
        "        --volume-name fix what it takes from the clock, process and param.json)\n"
        "        (.ffpfs: uncompressed PFS, byte-identical to MkPFS pack folder --raw\n"
        "        --no-compress; takes --timestamp, --block-size, --case-sensitive, --no-ampr)\n"
        "        (.pkg: a stored debug FPKG, as fpkg-build; needs --keys DIR, takes --seed\n"
        "        and --package-passcode; for a PS4 game a PS4 package, needs --ps4-keys FILE)\n"
        "        (.exfat/.img, .ffpfsc, or a trailing '/' for a folder). Images are\n"
        "        copied unchanged unless --rebuild asks for a fresh exFAT layout.\n"
        "        Takes the ffpfsc and exfat options below, and:\n"
        "        --lz4                 pack game files into AMPR LZ4 asset packs on the way\n"
        "                              (any output; read on the console\n"
        "                              by ampr_emu's PackedStdio runtime)\n"
        "        --lz4-runtime FILE    that runtime (libSceAmpr.sprx), placed in fakelib/\n"
        "        --lz4-profile P       which files to pack: auto (default: a console-tested\n"
        "                              profile matched by title ID or engine, else scan),\n"
        "                              scan, unity, insomniac, astrobot, ufc6, or FILE.toml\n"
        "                              (an ampr_pack.py configuration)\n"
        "        --lz4-level N         1-4 LZ4 fast, 5-12 LZ4 HC (default 9; also replaces\n"
        "                              a profile's compression_level)\n"
        "        --lz4-block-size N    16384..1048576 (default 65536; scan profile only)\n"
        "        --lz4-timestamp UNIX  time stamp of the files the packer adds (default now)\n"
        "        --lz4-config-out DIR  also write the pack configuration (pack.toml) and the\n"
        "                              forced-loose list (excludes.txt) for ampr_pack.py\n"
        "        --lz4-unpack          turn LZ4-packed files back into ordinary files\n"
        "        --ampr-emu FILE       ampr_emu's libSceAmpr.sprx: added to fakelib/ when eboot.bin\n"
        "                              uses the console's APR and the game ships none (a game\n"
        "                              run from an image or folder needs it; not for .pkg)\n"
        "        --ampr-emu-always     add it even when the game does not seem to need it\n"
        "  ampr-emu <input> [--ampr-emu FILE] [--backports DIR] [--passcode P]\n"
        "        tell whether a game needs ampr_emu; with --ampr-emu, add it: into a game\n"
        "        folder directly, or for an image as a ShadowMount+ backport folder\n"
        "        DIR/<TITLE_ID>/ (on the console DIR is /data/homebrew/backports)\n"
        "  fpkg-build <input> <out.pkg> --keys DIR [--seed HEX32] [--package-passcode P] [--overwrite]\n"
        "        build a stored debug FPKG from a game folder or image; DIR holds ProsperoPkgTool's\n"
        "        passcode.bin and mount_image.bin; a fixed --seed makes it reproducible\n"
        "        A PS4 game (sce_sys/param.sfo) becomes a PS4 fake package, as LibOrbisPkg's\n"
        "        pkg_build makes it: --ps4-keys FILE (LibOrbisPkg's Util/Keys.cs, pinned by\n"
        "        SHA-256), --package-passcode, --timestamp UNIX, and --creation-date YYYYMMDD\n"
        "        [--creation-time HHMMSS] when param.sfo has no PUBTOOLINFO c_date\n"
        "  ls <input> [--passcode P]\n"
        "        list the files in a folder, exFAT, FFPKG, FFPFSC or debug FPKG\n"
        "  cat <input> <path> [<out>] [--passcode P]\n"
        "        write one file of any input to <out> (default: standard output)\n"
        "  lz4-info <input>\n"
        "        summarise the LZ4 pack set of a folder or image\n"
        "  same <a> <b> [--passcode P]\n"
        "        compare the files of two inputs byte for byte (LZ4-packed inputs are\n"
        "        unpacked first; ampr_emu.index is ignored)\n"
        "  lz4-verify <input>\n"
        "        decode every LZ4 pack chunk and check it against the CRC sidecar\n"
        "  exfat <dump-dir> <out.exfat> [--cluster-size N] [--no-ampr] [--ampr-index keep|rebuild] [--ampr-scheme S]\n"
        "        build an exFAT image from a dump folder (clusters: 64 KiB by default;\n"
        "        --cluster-size ps5pkgtool picks 32 or 64 KiB like PS5PKGTool)\n"
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
        "  game-info <input> [--passcode P]\n"
        "        title ID, eboot.bin's process parameters and which console the game is for\n"
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
        "--progress                print progress as log lines when not on a terminal\n"
        "--ampr-scheme ampr-emu    hash names the way ampr_emu reads them (default)\n"
        "--ampr-scheme ps5pkgtool  match PS5PKGTool byte for byte; its index is\n"
        "                          rejected by ampr_emu when a name is not ASCII\n",
        stderr);
}

// Set by --progress: one line per percent, for logs.
bool g_progress_lines = false;

class ProgressPrinter {
public:
    ProgressFn callback() {
        // Live percentages only on a terminal; logs and pipes stay clean
        // unless --progress asks for lines.
        if (g_progress_lines && !stderr_is_terminal()) {
            return [this](const ProgressEvent& event) {
                int percent = event.total == 0 ? 100 : static_cast<int>(event.done * 100 / event.total);
                if (percent != last_percent_ || event.stage != stage_) {
                    last_percent_ = percent;
                    stage_ = std::string(event.stage);
                    std::time_t now = std::time(nullptr);
                    char when[16];
                    std::strftime(when, sizeof when, "%H:%M:%S", std::localtime(&now));
                    std::fprintf(stderr, "%s %s: %d%%\n", when, stage_.c_str(), percent);
                }
                return true;
            };
        }
        if (!stderr_is_terminal()) return {};
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
        if (last_percent_ >= 0 && stderr_is_terminal()) std::fputc('\n', stderr);
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
        // Paths may come with '\\' on Windows; Alembic uses '/' inside.
        if (arg.rfind("--", 0) == 0) {
            bool is_flag = std::find(flags.begin(), flags.end(), arg) != flags.end();
            std::string value = (!is_flag && i + 1 < argc) ? portable_path(argv[++i]) : "";
            args.options.emplace_back(arg, value);
        } else {
            args.positional.push_back(portable_path(arg));
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
    if (auto size = args.get("--cluster-size"); size && *size == "ps5pkgtool") {
        options.ps5pkgtool_cluster_rule = true;
    } else if (size) {
        auto value = parse_number(size->c_str());
        if (!value || *value > UINT32_MAX) return std::fputs("error: bad --cluster-size\n", stderr), false;
        options.cluster_size = static_cast<std::uint32_t>(*value);
    }
    if (args.get("--no-ampr")) options.generate_ampr_index = false;
    if (auto mode = args.get("--ampr-index")) {
        if (*mode == "rebuild") options.rebuild_ampr_index = true;
        else if (*mode != "keep") return std::fputs("error: --ampr-index takes keep or rebuild\n", stderr), false;
    }
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

// --ps4-keys, --package-passcode, --creation-date and --creation-time, for a PS4 package.
bool parse_ps4_package_options(const Args& args, fpkg::ps4::PackageOptions& o) {
    o.keys_file = args.get("--ps4-keys").value_or("");
    if (auto p = args.get("--package-passcode")) o.passcode = *p;
    if (auto text = args.get("--timestamp")) {
        auto value = parse_number(text->c_str());
        if (!value || *value > INT64_MAX) return std::fputs("error: bad --timestamp\n", stderr), false;
        o.timestamp = static_cast<std::int64_t>(*value);
    }
    o.creation_date = args.get("--creation-date");
    o.creation_time = args.get("--creation-time");
    return true;
}

// --keys, --package-passcode and --seed, for FPKG output.
bool parse_package_options(const Args& args, fpkg::build::PackageOptions& o) {
    o.keys_folder = args.get("--keys").value_or("");
    if (o.keys_folder.empty() && !args.get("--ps4-keys"))
        return std::fputs("error: pass --keys DIR (passcode.bin, mount_image.bin), or --ps4-keys FILE for a PS4 game\n", stderr), false;
    if (auto p = args.get("--package-passcode")) o.passcode = *p;
    if (auto hex = args.get("--seed")) {
        if (hex->size() != 32) return std::fputs("error: --seed takes 32 hex digits\n", stderr), false;
        std::array<std::uint8_t, 16> seed{};
        for (int i = 0; i < 16; ++i) {
            auto v = parse_number(("0x" + hex->substr(i * 2, 2)).c_str());
            if (!v) return std::fputs("error: bad --seed\n", stderr), false;
            seed[i] = static_cast<std::uint8_t>(*v);
        }
        o.seed = seed;
    }
    return true;
}

int cmd_convert(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    convert::Options options;
    if (!parse_ffpfsc_options(args, options.ffpfsc) || !parse_exfat_options(args, options.exfat)) return 2;
    options.rebuild = args.get("--rebuild").has_value();
    if (auto passcode = args.get("--passcode")) options.passcode = *passcode;
    options.overwrite = args.get("--overwrite").has_value();
    options.lz4_unpack = args.get("--lz4-unpack").has_value();
    if (auto path = args.get("--ampr-emu")) {
        auto file = File::open(*path, File::Mode::read);
        if (!file.ok()) return fail(file.error());
        options.ampr_emulator = std::make_shared<File>(std::move(*file));
    }
    options.ampr_emulator_always = args.get("--ampr-emu-always").has_value();
    if (options.ampr_emulator_always && !options.ampr_emulator)
        return std::fputs("error: --ampr-emu-always needs --ampr-emu FILE\n", stderr), 2;
    // FFPKG (UFS2): fixed time stamp and process ID for reproducible images.
    if (auto text = args.get("--ufs2-timestamp")) {
        auto value = parse_number(text->c_str());
        if (!value || *value > INT64_MAX) return std::fputs("error: bad --ufs2-timestamp\n", stderr), 2;
        options.ufs2.timestamp = static_cast<std::int64_t>(*value);
    }
    if (auto text = args.get("--ufs2-pid")) {
        auto value = parse_number(text->c_str());
        if (!value || *value > INT32_MAX) return std::fputs("error: bad --ufs2-pid\n", stderr), 2;
        options.ufs2.process_id = static_cast<std::int32_t>(*value);
    }
    if (auto text = args.get("--volume-name")) options.ufs2.volume_name = *text;
    std::optional<std::string> config_out = args.get("--lz4-config-out");
    if (args.get("--lz4")) {
        lz4pack::ProfileRequest request;
        int& level = request.level;
        if (auto text = args.get("--lz4-profile")) request.profile = *text;
        if (auto text = args.get("--lz4-level")) {
            auto value = parse_number(text->c_str());
            if (!value || *value < 1 || *value > 12) return std::fputs("error: --lz4-level must be 1-12\n", stderr), 2;
            level = static_cast<int>(*value);
        }
        if (auto text = args.get("--lz4-block-size")) {
            auto value = parse_number(text->c_str());
            if (!value || *value < 16384 || *value > 1048576 || (*value & (*value - 1)))
                return std::fputs("error: --lz4-block-size must be a power of two from 16384 to 1048576\n", stderr), 2;
            std::uint32_t shift = 0;
            while ((1ull << shift) < *value) ++shift;
            request.block_shift = shift;
        }
        lz4pack::PackOptions pack;
        auto input = convert::open(args.positional[0], options.passcode);
        if (!input.ok()) return fail(input.error());
        auto profile = lz4pack::choose_profile(*input->tree, request);
        if (!profile.ok()) return fail(profile.error());
        std::printf("LZ4 profile: %s\n", profile->description.c_str());
        for (const std::string& note : profile->notes) std::fprintf(stderr, "warning: %s\n", note.c_str());
        pack.config = std::move(profile->config);
        pack.config.workers = options.ffpfsc.compression.threads;
        if (auto runtime = args.get("--lz4-runtime")) {
            auto file = File::open(*runtime, File::Mode::read);
            if (!file.ok()) return fail(file.error());
            pack.runtime_module = std::make_shared<File>(std::move(*file));
        }
        if (auto text = args.get("--lz4-timestamp")) {
            auto value = parse_number(text->c_str());
            if (!value || *value > INT64_MAX) return std::fputs("error: bad --lz4-timestamp\n", stderr), 2;
            pack.timestamp = static_cast<std::int64_t>(*value);
        }
        options.lz4 = std::move(pack);
    }
    std::optional<convert::Format> to;
    if (auto text = args.get("--to")) {
        to = convert::parse_format(*text);
        if (!to) return std::fputs("error: --to must be folder, exfat, ffpkg, ffpfs, ffpfsc or fpkg\n", stderr), 2;
    } else {
        to = convert::guess_output_format(args.positional[1]);
        if (!to) return std::fputs("error: cannot tell the output format from its name; pass --to\n", stderr), 2;
    }
    if (*to == convert::Format::fpkg &&
        (!parse_package_options(args, options.fpkg) || !parse_ps4_package_options(args, options.fpkg_ps4)))
        return 2;
    ProgressPrinter printer;
    auto report = convert::run(args.positional[0], args.positional[1], *to, options, printer.callback());
    printer.done();
    if (!report.ok()) return fail(report.error());
    std::printf("%s -> %s: %s\n", convert::name(report->from), convert::name(report->to), report->method.c_str());
    if (!report->detail.empty())
        std::printf("%s: %s\n", report->to == convert::Format::fpkg ? "content ID" : "inner name", report->detail.c_str());
    if (report->ampr_emulator_added) std::printf("AMPR emulator: added as %s\n", report->ampr_emulator_path.c_str());
    for (const std::string& w : report->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    if (report->lz4) {
        const lz4pack::PackStats& st = *report->lz4;
        auto gib = [](std::uint64_t v) { return static_cast<double>(v) / (1024.0 * 1024.0 * 1024.0); };
        // GiB with two decimals hides small trees; fall back to MiB.
        bool small = st.logical_bytes + st.loose_bytes < (1ull << 30);
        auto unit = [&](std::uint64_t v) { return small ? static_cast<double>(v) / (1024.0 * 1024.0) : gib(v); };
        const char* u = small ? "MiB" : "GiB";
        std::printf("LZ4 packs:  %llu of %llu files packed (%llu left loose by sampling), %u volumes\n",
                    static_cast<unsigned long long>(st.files_packed), static_cast<unsigned long long>(st.files_total),
                    static_cast<unsigned long long>(st.files_auto_loose), st.volumes);
        std::printf("            %.2f %s packed -> %.2f %s in volumes (%llu LZ4, %llu raw, %llu shared chunks)\n",
                    unit(st.logical_bytes), u, unit(st.volume_bytes), u, static_cast<unsigned long long>(st.chunks_lz4),
                    static_cast<unsigned long long>(st.chunks_raw), static_cast<unsigned long long>(st.chunks_shared));
        std::printf("            %.2f %s stays loose\n", unit(st.loose_bytes), u);
        for (const std::string& w : st.warnings) std::printf("  %s\n", w.c_str());
        if (config_out) {
            std::string toml = report->lz4_config_toml, excludes;
            for (const std::string& p : report->lz4_forced_loose) excludes += p + "\n";
            for (auto [name, text] : {std::pair<const char*, std::string*>{"pack.toml", &toml}, {"excludes.txt", &excludes}}) {
                auto out = AtomicOutput::create(*config_out + "/" + name, true);
                Status status = out.ok() ? out->file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(text->data()), text->size()))
                                         : Status(out.error());
                if (status.ok()) status = out->commit();
                if (!status.ok()) return fail(status.error());
            }
        }
    }
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

// Builds a debug FPKG from a game folder or image.
int cmd_fpkg_build(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    auto input = convert::open(args.positional[0], args.get("--passcode").value_or(std::string(32, '0')));
    if (!input.ok()) return fail(input.error());
    auto game_root = ffpfsc::resolve_game_root(input->tree->root());
    if (game_root.ok() && ffpfsc::is_ps4_game(*input->tree, **game_root)) {
        fpkg::ps4::PackageOptions o;
        if (!parse_ps4_package_options(args, o)) return 2;
        o.overwrite = args.get("--overwrite").has_value();
        auto r = fpkg::ps4::write_package(*input->tree, input->tree->root(), args.positional[1], o);
        if (!r.ok()) return fail(r.error());
        for (const std::string& w : r->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        if (r->generated_license) std::fprintf(stderr, "note: generated a debug licence (the game had none)\n");
        if (r->generated_keystone) std::fprintf(stderr, "note: generated sce_sys/keystone (the game had none)\n");
        std::printf("%s: PS4 package, %llu bytes, %zu inner files (content ID %s)\n", args.positional[1].c_str(),
                    static_cast<unsigned long long>(r->package_length), r->inner_files, r->content_id.c_str());
        return 0;
    }
    fpkg::build::PackageOptions o;
    if (!parse_package_options(args, o)) return 2;
    o.overwrite = args.get("--overwrite").has_value();
    auto r = fpkg::build::write_package(*input->tree, input->tree->root(), args.positional[1], o);
    if (!r.ok()) return fail(r.error());
    for (const std::string& w : r->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    std::printf("%s: %llu bytes, %zu inner files (content ID %s)\n", args.positional[1].c_str(),
                static_cast<unsigned long long>(r->package_length), r->inner_files, r->content_id.c_str());
    return 0;
}

// Copies `length` bytes of `source` to `sink` in 1 MiB pieces.
Status copy_all(const RandomAccess& source, std::uint64_t length, ByteSink& sink) {
    std::vector<std::uint8_t> buffer(1 << 20);
    for (std::uint64_t done = 0; done < length;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - done));
        ALEMBIC_TRY(source.read_at(done, MutableBytes(buffer.data(), n)));
        ALEMBIC_TRY(sink.write(Bytes(buffer.data(), n)));
        done += n;
    }
    return {};
}

class StdoutSink final : public ByteSink {
public:
    Status write(Bytes data) override {
        if (std::fwrite(data.data(), 1, data.size(), stdout) != data.size())
            return make_error(Errc::io, "cannot write to standard output");
        return {};
    }
};

int cmd_cat(const Args& args) {
    if (args.positional.size() != 2 && args.positional.size() != 3) return usage(), 2;
    auto input = convert::open(args.positional[0], args.get("--passcode").value_or(std::string(32, '0')));
    if (!input.ok()) return fail(input.error());
    const tree::Node* node = tree::find(input->tree->root(), args.positional[1]);
    if (!node || node->is_directory)
        return std::fprintf(stderr, "error: no file '%s'\n", args.positional[1].c_str()), 1;
    auto file = input->tree->open(*node);
    if (!file.ok()) return fail(file.error());
    if (args.positional.size() == 2) {
        StdoutSink sink;
        Status status = copy_all(**file, node->size, sink);
        std::fflush(stdout);
        return status.ok() ? 0 : fail(status.error());
    }
    auto out = AtomicOutput::create(args.positional[2]);
    if (!out.ok()) return fail(out.error());
    FileSink sink(out->file(), 0);
    Status status = copy_all(**file, node->size, sink);
    if (status.ok()) status = out->commit();
    return status.ok() ? 0 : fail(status.error());
}

int cmd_lz4_verify(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    auto input = convert::open(args.positional[0], args.get("--passcode").value_or(std::string(32, '0')));
    if (!input.ok()) return fail(input.error());
    ProgressPrinter printer;
    auto result = lz4pack::verify(*input->tree, printer.callback());
    printer.done();
    if (!result.ok()) return fail(result.error());
    std::printf("files:           %llu packed files verified\n", static_cast<unsigned long long>(result->files));
    std::printf("chunks:          %llu unique, %llu stored bytes -> %llu bytes\n",
                static_cast<unsigned long long>(result->physical_chunks),
                static_cast<unsigned long long>(result->stored_bytes), static_cast<unsigned long long>(result->raw_bytes));
    return 0;
}

int cmd_same(const Args& args) {
    if (args.positional.size() != 2) return usage(), 2;
    ProgressPrinter printer;
    auto result = convert::compare(args.positional[0], args.positional[1],
                                   args.get("--passcode").value_or(std::string(32, '0')), printer.callback());
    printer.done();
    if (!result.ok()) return fail(result.error());
    for (const std::string& p : result->only_in_a) std::printf("only in a: %s\n", p.c_str());
    for (const std::string& p : result->only_in_b) std::printf("only in b: %s\n", p.c_str());
    for (const std::string& p : result->different) std::printf("differs: %s\n", p.c_str());
    std::size_t differences = result->only_in_a.size() + result->only_in_b.size() + result->different.size();
    std::printf("%llu files compared, %zu differences\n", static_cast<unsigned long long>(result->files), differences);
    return differences == 0 ? 0 : 1;
}

int cmd_lz4_info(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    auto input = convert::open(args.positional[0], args.get("--passcode").value_or(std::string(32, '0')));
    if (!input.ok()) return fail(input.error());
    const tree::Node* node = tree::find(input->tree->root(), lz4pack::kIndexName);
    if (!node) return std::fputs("error: no ampr_assets.index in this input\n", stderr), 1;
    auto file = input->tree->open(*node);
    if (!file.ok()) return fail(file.error());
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node->size));
    Status status = (*file)->read_at(0, data);
    if (!status.ok()) return fail(status.error());
    auto m = lz4pack::parse_manifest(data);
    if (!m.ok()) return fail(m.error());
    std::uint64_t packed = 0, logical = 0;
    for (const auto& f : m->files)
        if (f.flags & lz4pack::kFilePacked) ++packed, logical += f.logical_size;
    std::printf("build id:        ");
    for (std::uint8_t b : m->build_id) std::printf("%02x", b);
    std::printf("\nfiles:           %zu (%llu packed, %llu bytes)\nchunks:          %zu\n", m->files.size(),
                static_cast<unsigned long long>(packed), static_cast<unsigned long long>(logical), m->chunks.size());
    for (std::size_t i = 0; i < m->packs.size(); ++i)
        std::printf("  %-48s %14llu bytes, %u-byte pages%s\n", m->pack_name(i).c_str(),
                    static_cast<unsigned long long>(m->packs[i].file_size), m->packs[i].io_page_size,
                    (m->packs[i].flags & lz4pack::kPackStriped) ? ", striped" : "");
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
        std::printf("%s %s\n", p.ps4() ? "PFS head digest:" : "FIH digest:     ", digests->fih_digest_ok ? "ok" : "MISMATCH");
        std::printf("entry digests:   %s%s%s\n", digests->entry_digests_ok ? "ok" : "MISMATCH",
                    digests->detail.empty() ? "" : " - ", digests->detail.c_str());
    }
    if (p.kind != fpkg::Kind::finalized_debug && !p.ps4()) return 0;
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

// Which console a game is for, and why (ffpfsc::game_platform).
int cmd_game_info(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    auto input = convert::open(args.positional[0], args.get("--passcode").value_or(std::string(32, '0')));
    if (!input.ok()) return fail(input.error());
    auto root = ffpfsc::resolve_game_root(input->tree->root());
    if (!root.ok()) return fail(root.error());
    const tree::Node& game = **root;
    std::printf("title id:        %s\n", ffpfsc::read_title_id(*input->tree, game).value_or("-").c_str());
    ffpfsc::ProcessParams pp;
    if (const tree::Node* eboot = tree::find_child_icase(game, "eboot.bin"); eboot && !eboot->is_directory)
        if (auto file = input->tree->open(*eboot); file.ok()) pp = ffpfsc::executable_process_params(**file);
    auto sdk = [](std::uint32_t v) {
        char t[16];
        std::snprintf(t, sizeof t, "%x.%02x", v >> 24, (v >> 16) & 0xFF);
        return std::string(t);
    };
    if (!pp.version) std::printf("eboot.bin:       process parameters not readable\n");
    else std::printf("eboot.bin:       PS4 SDK %s, PS5 SDK %s (process parameters version %u)\n", sdk(pp.ps4_sdk).c_str(),
                     pp.ps5_sdk ? sdk(pp.ps5_sdk).c_str() : "-", pp.version);
    auto has = [&](const char* name) { return tree::find(game, std::string("sce_sys/") + name) != nullptr; };
    std::printf("metadata:        %s%s\n", has("param.sfo") ? "param.sfo " : "", has("param.json") ? "param.json" : "");
    ffpfsc::Platform p = ffpfsc::game_platform(*input->tree, game);
    std::printf("platform:        %s\n", p == ffpfsc::Platform::ps4 ? "PS4" : p == ffpfsc::Platform::ps5 ? "PS5" : "unknown");
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

int cmd_ampr_emu(const Args& args) {
    if (args.positional.size() != 1) return usage(), 2;
    const std::string& path = args.positional[0];
    auto input = convert::open(path, args.get("--passcode").value_or(std::string(32, '0')));
    if (!input.ok()) return fail(input.error());
    auto emulator_path = args.get("--ampr-emu");
    if (!emulator_path) {
        auto check = ampr_emu::check(*input->tree);
        if (!check.ok()) return fail(check.error());
        std::printf("imports libSceAmpr: %s\n", check->imports_ampr ? "yes" : "no");
        std::printf("fakelib folder:     %s%s\n", check->fakelib.c_str(), check->has_fakelib ? "" : " (none yet)");
        std::printf("ships ampr_emu:     %s\n", check->ships_emulator ? "yes" : "no");
        std::printf("needs the emulator: %s\n", check->needed() ? "yes" : "no");
        return 0;
    }
    auto file = File::open(*emulator_path, File::Mode::read);
    if (!file.ok()) return fail(file.error());
    Result<ampr_emu::AttachResult> r = make_error(Errc::invalid_argument, "");
    if (input->format == convert::Format::folder) {
        input->tree.reset();
        r = ampr_emu::attach_to_folder(path, *file);
    } else {
        auto backports = args.get("--backports");
        if (!backports)
            return std::fputs("error: an image cannot be changed in place; pass --backports DIR to write a "
                              "ShadowMount+ backport folder (or convert it again with --ampr-emu)\n", stderr), 2;
        r = ampr_emu::attach_backport(*input->tree, *backports, *file);
    }
    if (!r.ok()) return fail(r.error());
    for (const std::string& n : r->notes) std::printf("note: %s\n", n.c_str());
    if (r->already_there) return std::printf("nothing to do\n"), 0;
    std::printf("wrote to %s:\n", r->folder.c_str());
    for (const std::string& w : r->written) std::printf("  %s\n", w.c_str());
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
    // UTF-8 arguments and console output everywhere (Windows hands main()
    // the ANSI code page).
    use_utf8_console();
    std::vector<std::string> utf8 = program_arguments(argc, argv);
    std::vector<char*> pointers;
    for (std::string& a : utf8) pointers.push_back(a.data());
    pointers.push_back(nullptr);
    argc = static_cast<int>(utf8.size());
    argv = pointers.data();
    if (argc < 2) return usage(), 2;
    std::string command = argv[1];
    // --progress may appear anywhere; strip it before the command parses.
    int kept = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--progress") == 0) g_progress_lines = true;
        else argv[kept++] = argv[i];
    }
    argc = kept;
    if (argc < 2) return usage(), 2;
    if (command == "convert")
        return cmd_convert(
            parse(argc, argv, {"--no-ampr", "--case-sensitive", "--overwrite", "--rebuild", "--lz4", "--lz4-unpack",
                                    "--ampr-emu-always"}));
    if (command == "ampr-emu") return cmd_ampr_emu(parse(argc, argv, {}));
    if (command == "lz4-verify") return cmd_lz4_verify(parse(argc, argv, {}));
    if (command == "lz4-info") return cmd_lz4_info(parse(argc, argv, {}));
    if (command == "same") return cmd_same(parse(argc, argv, {}));
    if (command == "ls") return cmd_ls(parse(argc, argv, {}));
    if (command == "fpkg-build") return cmd_fpkg_build(parse(argc, argv, {"--overwrite"}));
    if (command == "cat") return cmd_cat(parse(argc, argv, {}));
    if (command == "exfat") return cmd_exfat(parse(argc, argv, {"--no-ampr"}));
    if (command == "ampr-index") return cmd_ampr_index(parse(argc, argv, {}));
    if (command == "ffpfsc") return cmd_ffpfsc(parse(argc, argv, {"--no-ampr", "--case-sensitive", "--overwrite"}));
    if (command == "ffpfsc-info") return cmd_ffpfsc_info(parse(argc, argv, {}));
    if (command == "ffpfsc-extract") return cmd_ffpfsc_extract(parse(argc, argv, {}));
    if (command == "ffpfsc-verify") return cmd_ffpfsc_verify(parse(argc, argv, {}));
    if (command == "fpkg-info") return cmd_fpkg_info(parse(argc, argv, {}));
    if (command == "game-info") return cmd_game_info(parse(argc, argv, {}));
    if (command == "fpkg-dump") return cmd_fpkg_dump(parse(argc, argv, {}));
    if (command == "pfsc-encode") return cmd_pfsc_encode(parse(argc, argv, {}));
    if (command == "pfsc-decode") return cmd_pfsc_decode(parse(argc, argv, {}));
    if (command == "pfsc-info") return cmd_pfsc_info(parse(argc, argv, {}));
    usage();
    return 2;
}

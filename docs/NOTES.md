# Alembic: research notes and plan

Chronological log below; **read "Where we are" and "Next session" first.**

## Where we are (session 2, 2026-10-18)

### What works (CLI: `build/alembic`)

| From \ To | folder | exFAT | FFPFSC | FFPKG (UFS2) | FPKG |
|---|---|---|---|---|---|
| folder | – | ✓ | ✓ | ✓ | – |
| exFAT | ✓ | `--rebuild` | ✓ (wrap) | ✓ | – |
| FFPFSC (exFAT, UFS2 or PFS payload) | ✓ | ✓ | ✓ | ✓ (unwrap UFS2) | – |
| FFPKG (UFS2) | ✓ | ✓ | ✓ (wrap) | `--rebuild` | – |
| FFPFS (PFS) | ✓ | ✓ | ✓ (wrap) | ✓ | – |
| FPKG (debug) | ✓ | ✓ | ✓ | ✓ | – |

Every row can also be written as FFPFS (`.ffpfs`, uncompressed PFS),
byte-identical to MkPFS (see the 2026-10-18 FFPFS entry).

- `alembic convert <in> <out>` (format from content / extension / `--to`),
  `alembic ls <in>`, plus `exfat`, `ffpfsc`, `ffpfsc-info|extract|verify`,
  `fpkg-info`, `fpkg-dump`, `ampr-index`, `pfsc-encode|decode|info`.
- **Hardware-tested:** an FFPFSC made by `alembic convert pkg out.ffpfsc` from
  sample package B mounted and ran on a PS5 with ShadowMountPlus.
- Output written by Alembic (exFAT, FFPFSC, AMPR index with
  `--ampr-scheme ps5pkgtool`, PFSC at `--level 9`) is **byte-identical to
  PS5PKGTool**; the default AMPR scheme matches drakmor's ampr_emu builder.
- **LZ4 asset packs:** `convert --lz4` (to folder, exFAT or FFPFSC) packs
  game files for ampr_emu's PackedStdio runtime, byte-identical to
  `ampr_pack.py`; `--lz4-unpack`, `lz4-info`, `lz4-verify`, `same`. A 149 GiB
  game became a 114.7 GiB exFAT in 9.6 min (see the 2026-10-18 entry).
- Performance (14-core M-series): FPKG → FFPFSC 12.4 s for a 2.7 GB image
  (compression on all cores, default zlib level 6), FPKG → exFAT 5 s,
  FFPFSC → exFAT 3 s, memory ~40 MB.

### How it is checked

```sh
cmake -S . -B build && cmake --build build && ./build/alembic_tests   # 51 unit tests
tools/oracle/compare.sh             # 69 byte-for-byte checks vs PS5PKGTool, MkPFS (.ffpfs) + ampr_emu builder
tools/pkg-oracle/compare.sh         # FPKG reading vs packages built by ProsperoPkgTool/LibProsperoPKG
tools/pkg-oracle/check_samples.sh   # owner's real files in .refs/samples/ (~5 min)
tools/lz4-oracle/compare.sh         # LZ4 packs vs ampr_pack.py (needs .refs/venv, Homebrew lz4)
```

### Environment (all git-ignored, recreate if missing)

- `.refs/` holds reference checkouts:
  `git clone` https://github.com/pearlxcore/PS5PKGTool,
  https://github.com/pearlxcore/ProsperoPkgTool,
  https://github.com/SvenGDK/LibProsperoPKG (as `LibProsperoPKG-sven`),
  https://github.com/drakmor/ampr_emu, https://github.com/drakmor/ShadowMountPlus,
  https://github.com/theghostonline/PS5-LZ4-Compression,
  https://github.com/rdmrocha/fpkg-cli (no license: read only).
- `.refs/MkPFS` (https://github.com/PSBrew/MkPFS, GPL-3.0, 1.1.0 at `1d5df56`)
  and `.refs/mkpfs-venv`: `python3 -m venv .refs/mkpfs-venv &&
  .refs/mkpfs-venv/bin/pip install ./.refs/MkPFS` (the oracle for `.ffpfs`;
  its own PyPI dependencies). Also cloned for reference: danthespal/MkPFS.CSharp
  (GPL-3.0, has an FPKG builder too), ps5-homebrew-ui, websrv,
  ps5-web-file-manager, ps5-payload-manager.
- `.refs/venv`: `python3 -m venv .refs/venv` (no packages; the LZ4 oracle uses
  Homebrew's liblz4 1.10.0). `.refs/runtime/ampr_emu-0.4.2.1/`: the PackedStdio
  `libSceAmpr.sprx` from the official release (SHA-256 in the 2026-10-18 entry).
  `.refs/out/`: conversion outputs.
- `tools/pkg-oracle/patch_libprosperopkg.py` patches LibProsperoPKG with a
  managed SHA3 (macOS .NET has none); `compare.sh` runs it automatically.
- `.refs/samples/`: the owner's real test files (three debug FPKGs A/B/C,
  an exFAT dump, an FFPKG, a 13 GB FFPFSC, and our FFPFSC of package B).
  **Never name them anywhere in the repo** (CLAUDE.md rule; history was
  scrubbed once already).
- Tools: Apple clang 21, CMake, .NET 10 SDK, Python 3. No Docker.

### Known limitations

- Writing FPKG: stored packages only (`convert --to fpkg`, `fpkg-build`, the engine); no Kraken, fake-signing, DDS generation or keystone generation. Not yet installed on a console.
- LZ4 packs: no TOML profile input yet (only the automatic profile); pass 1
  keeps ~200 bytes per chunk in RAM (456 MB for 2.2M chunks), which may need
  trimming for the PS5 payload; never tested on a console.
- PFS reading covers unsigned, unencrypted images with 32-bit inodes and
  contiguous files (what MkPFS and Alembic write); signed, encrypted, 64-bit
  inode and fragmented PFS images are refused.
- FPKG: retail and patch (LIH) packages are not opened; NAPS "shuffle"
  layouts (current LibProsperoPKG output) are refused.
- Reading (inflate, Kraken, SHA-256, AES) is single-threaded and uses no
  CPU crypto instructions.
- No PS5 build yet; the core uses only POSIX + std::thread, no exceptions/RTTI.

## Reference repositories

### pearlxcore/PS5PKGTool (GPL-3.0)

- A Windows WinForms app on .NET 10. It does not run on a PS5; we port its
  logic, not its binaries.
- Formats: dump folders, exFAT, FFPKG (UFS2), FFPFSC (a PFSC-compressed PFS
  wrapper without Sony signatures), debug FPKG.
- FPKG → image: `PS5PKGTool.Core/Services/SonyPackageImageConversion.cs`
  extracts the package to a temp folder, then builds the image from it. That
  needs about twice the disk space. **Alembic should stream instead**: read
  files from the FPKG's inner image and write them straight into the target.
  It skips the inner-PFS system tables `inode_flat_path_table`,
  `apr_flat_path_table` and `afid_to_ino_table`.
- Code worth porting:
  - `PS5PKGTool.Ffpfsc/`, about 4k lines: `ExfatImage` (streams exFAT from a
    folder, mirrors MkPFS's deterministic layout), `AmprIndex` (AMPRIDX3,
    added when the dump has `fakelib/libSceAmpr.sprx`), `PfscCodec` (64 KiB
    zlib/raw blocks), `FfpfscImage` (create, inspect, extract, verify).
  - `PS5PKGTool.Ufs2/`, about 9k lines of UFS2/FFPKG code, BSD-2, from
    SvenGDK/UFS2Tool. Keep its license text.
- FFPFSC output is byte-identical to MkPFS when the PFS build timestamp is
  fixed, which gives us a byte-for-byte test oracle.
- Avoid: LibProsperoPkg 1.2.0 and `libScePubTools.dll` (binaries only, GPL),
  and the bundled Oodle DLL (cannot legally be redistributed or reused).

### pearlxcore/ProsperoPkgTool (MIT): the FPKG engine

- Managed code with no native dependencies, including a clean-room **Kraken
  encoder and decoder** (`ProsperoPkgTool/Compression/`). No Oodle needed.
- Covers FIH/CNT parsing, key derivation from content ID + passcode, outer
  PFS (AES-128-XTS), inner image reader, protected CNT entries (AES-128-CBC,
  SHA3 profile then SHA-256 fallback), and the FPKG builder (NAPS, PlayGo,
  fake-SELF, right.sprx injection).
- About 25k lines including CLI and GUI. The library is mostly the large
  files in `Containers/` and `Compression/`.
- Only **debug** FPKGs with a known passcode can be read (the default is all
  zeros); retail packages cannot be decrypted.
- Its README says no FPKG it built has been installed or launched on a PS5.

### blackbearreloaded/ps5-homebrew-ui (GPL-3.0-or-later)

- An immediate-mode UI kit in C++20 / OpenGL 4.6. Builds with clang-18 on
  Linux/WSL against the PS5 payload SDK. Hardware-tested at 60 fps.
- Useful for us: File Browser design, lists, dialogs, progress indicators,
  toasts, and a host preview that renders the UI on a PC.
- Rules we inherit:
  - No exceptions, no RTTI (`try`, `throw`, `dynamic_cast`, `typeid`).
  - Exit with `sceSystemServiceLoadExec("exit", NULL)`; `exit()` ends in the
    crash reporter. Killing a title while it renders has lost consoles.
  - A static zlib is already part of its build.
- **Open problem: storage access.** A title's sandbox has no `/data`; only
  `/app0` (read-only) and `/download0` (writable) are available. A converter
  needs multi-GB access to `/data` or USB. Options, neither verified yet:
  - run the conversion in a separate payload (payload SDK), with the UI app
    as a front end;
  - check on the target firmware and loader which mounts a title can see.

### SvenGDK/LibProsperoPKG (GPL-3.0): full FPKG pipeline in source

Inspected 2026-10-17. **Correction to the notes above:** LibProsperoPkg is
not binaries-only. SvenGDK/LibProsperoPKG is about 33k lines of C# under
GPL-3.0-or-later (compatible with ours), with no native imports and no Oodle
or `libScePubTools` references. drakmor/LibProsperoPKG is only a fork of it,
3 commits behind and 0 ahead (fork at `be79990`, 2026-07-17; upstream at
`748eabf`, 2026-09-12). The "Drakmor 0.6.x" binary releases that fpkg-cli
targets seem to be a separate, closed lineage. We could not find their source.

- Covers: inner PFS builder/reader, AES-XTS outer PFS, CNT writer/reader, FIH
  debug image, RSA-3072 metadata signing, SHA3 digest chain, NAPS layout,
  PlayGo (chunk.dat, hash-table, ficm, CRC-32C), keystone (chained
  HMAC-SHA256), fake-SELF generation, param.json model, UCP archives, DDS
  (BC7) encoding, package extractor and validator, disc-backup reassembly.
- Kraken encoder and decoder in managed code. The decoder is derived from
  powzix/ooz (GPL-3.0, C++), which we could port directly.
- Inner-image Kraken keeps a block only if `comp <= uncomp * 15 >> 4`
  (6.25 % saving). Do not confuse this with PS5PKGTool's zlib PFSC rule (5 %).
- `docs/implementation-status.md` and `docs/ps5-pkg-format.md` are the best
  written spec we have for FPKG building and the digest chain.
- **Do not copy:** `PlayGo/Data/right.sprx` (a SELF binary of unclear
  origin), `Keys/Data/*.bin` and `pkg_meta_rsa_key.pem` (key material; check
  where they come from before using any of it).
- Parts are derived from maxton/LibOrbisPkg (GPL-3.0): PFS model, CNT
  writer, crypto utilities.

### rdmrocha/fpkg-cli (no license)

- **No license file, so we may not copy its code.** We can only learn facts
  from its docs. It is a .NET CLI that drives a LibProsperoPkg DLL and binary-
  patches it with dnlib (container sources, Oodle, Windows-parity fixes).
- Same streaming idea as Alembic: it builds FPKGs straight from `.ffpfsc` /
  `.exfat` without extracting (`FpkgVirtualSource/`: a small exFAT reader).
- Facts from `NOTES.md` worth keeping for the FPKG builder:
  - Before building from a retail dump, set aside `sce_sys/license.dat`,
    `license.info`, `playgo-chunk.dat`, `playgo-hash-table.dat`,
    `playgo-ficm.dat`, `origin-param.json`, `target-param.json`. Otherwise
    stale PlayGo maps win over regenerated ones and the retail licence
    suppresses the debug one.
  - Matching the official Windows tools: language fillers each get their own
    stored copy, start on 64 KiB boundaries and go last in the image; licence
    and NP entries are stored unencrypted; no invented `about/right.sprx`;
    directories are walked case-insensitively.
  - PlayGo rules from the official binaries: scenario type `0x21`, first
    chunk ref is chunk 0, every scenario lists every chunk,
    `1 <= initial_chunk_count <= len`, limits 1000 chunks / 65535 mchunks /
    5 scenarios, language id N is bit `1 << (63 - N)`, and a language mask of
    all ones means "languages not supported". All files in chunk 0 with every
    chunk initial is the known-good layout.
  - Some dumps have corrupt `sce_sys/pic*.png`; fpkg-cli validates the PNG
    chunk chain and CRCs and rebuilds from the matching `.dds`.
  - "PFS v3" without shuffle analysis is byte-identical to v2; the shuffle
    variant produced images a console would not load.
  - LibProsperoPkg drops root `ampr_emu.index`, `fakelib/libSceAmpr.sprx` and
    `fakelib/libScePlayGo.sprx` from FPKG builds.

### drakmor/ampr_emu (GPL-3.0): the reader of ampr_emu.index

- Confirms the short FNV offset `1469598103934665603` is intended: the
  console-side reader uses it too.
- **PS5PKGTool bug, which our port reproduces:** the reader
  (`src/ampr_emu_index.cpp`, `compact_index_hash_key`) hashes the **UTF-8
  bytes** with **ASCII-only** lowercasing. PS5PKGTool hashes **Unicode code
  points** after full .NET lowercasing. They agree for ASCII paths only. On
  load the reader recomputes every slot's hash and marks the **whole index
  invalid** on any mismatch, so one non-ASCII file name breaks the index.
  Checked: `/app0/Data/été.dat` hashes differently in the two schemes.
  The reader also sorts and compares paths with ASCII-only folding.
- **Resolved 2026-10-17:** Alembic now defaults to ampr_emu's rules
  (`ampr::Scheme::ampr_emu`), following `tools/build_ampr_index.py`: fold
  UTF-8 bytes (A-Z only), sort by folded bytes, skip root `ampr_commands.bin`,
  `apr_emu.log` and `ampr_emu.index.bak`, flag every slot in a duplicate-hash
  group, and refuse paths with tabs or line breaks, paths over 1023 bytes or
  names over 255 bytes (the reader would reject the whole index).
  `--ampr-scheme ps5pkgtool` keeps PS5PKGTool parity for tests.
  `compare.sh` checks both: byte-identical to ampr_emu's builder on a
  fixture with non-ASCII names, and to PS5PKGTool with the compat scheme.
- Not yet looked at: how ampr_emu behaves when the index is rejected (it may
  rebuild its own at runtime, since it knows `ampr_emu.index.tmp`).

### theghostonline/PS5-LZ4-Compression (GPL-3.0): a working PS5 tool

Inspected 2026-10-17. The closest existing thing to what Alembic wants to be
on the console, and proof that the approach works on firmware 13.60.

- **Architecture:** a payload ELF (`lz4-compression.elf`, built with
  ps5-payload-sdk v0.43 `prospero.mk`, clang-18, C) does all the work and
  serves an HTTP API + web page on port 5920. A native app (title
  `PPSA99420`, built with half144/ps5-react: QuickJS + embedded React) is the
  front end and starts the payload through Payload Manager. Settings and logs
  in `/data/LZ4Compression`. **This answers our storage-access open problem:
  the conversion runs in a payload, which can read `/data` and USB; the app
  is only a UI.** ShadowMountPlus is used to mount images; Alembic reads
  image files directly, so it does not need mounts for conversion.
- On the console it does: FFPFSC compression (zlib level 7, multi-threaded
  block pipeline, up to 16 workers), FFPFSC → folder/exFAT, exFAT/PFS images
  ("choose PFS or exFAT output"), validate and repair of PFSC blocks,
  `ampr_emu.index` building, copy/move between internal and USB, rest-mode
  handling (renews the power timer during destructive phases), job history.
  Tested: 67 GB title → 19.5 GB FFPFSC on a PS5.
- **"LZ4 images" = AMPR seekable LZ4 asset packs**, not LZ4 inside PFSC.
  Game files are moved into `ampr_assets-NNN.pak` volumes (AMPRDAT3) with a
  manifest `ampr_assets.index` (AMPRPAK4) plus `.crc` (AMPRCRC1, offline
  verify only) and optional `.runtime` (AMPRCFG1) sidecars. The pack-capable
  ampr_emu runtime (`fakelib/libSceAmpr.sprx`, "PackedStdio" build 0.4.2.1)
  serves reads from the packs at runtime. Independent raw-LZ4 or RAW blocks,
  16 KiB to 1 MiB per file, LZ4 level 1-12 (1-4 fast, 5-12 HC, default 9).
  Executables, PRX/SPRX, `sce_sys`, `sce_module`, `fakelib`, saves, configs
  and already-compressed media stay loose. Packing needs `ampr_emu.index`
  (every file keeps its fileId) and a TOML profile (automatic scan or
  per-game, e.g. `profiles/*.toml`).
- References for the pack format: drakmor/ampr_emu `tools/ampr_pack.py` and
  `ampr_pack_format.py` (Python, ~3.6k lines) and `docs/ASSET_PACKS.md`;
  PS5-LZ4-Compression `src/ampr_pack*.c` (C port, ~6.4k lines, stated to be
  byte-identical to `ampr_pack.py` at HC 9/12 except the build ID). Both GPL.
  `ampr_pack.py verify/inspect/unpack` can be our oracle.
- Third party: LZ4 1.10.0 (BSD-2), zlib 1.3.1, miniz tinfl. Its PS5 Game
  Compressor ancestry has **no license** (juma-sayeh/PS5-Game-Compressor), so
  check which parts of `src/pfs_*.c` are original before porting any of it.
- The pinned runtime `third_party/ampr_emu/libSceAmpr.sprx` is a GPL binary
  built from drakmor/ampr_emu (SHA-256 `69e6c4d5…116d1a`). Decision: Alembic
  bundles no binaries; the app downloads them from the official release and
  checks a pinned SHA-256.
- Owner decision: no code from unlicensed upstreams (PS5 Game Compressor,
  fpkg-cli, and PS5-LZ4-Compression's `src/pfs_*` which descends from PS5
  Game Compressor). LZ4 packs get ported from drakmor's GPL `ampr_pack.py`.

How LZ4 packs fit Alembic: packing is a tree → tree transformation (some
files replaced by pack volumes + manifest), so it composes with every
writer: dump → LZ4-packed tree → exFAT/FFPFSC/FPKG. Unpacking is a
`tree::Source` that serves packed files from the volumes, so a packed image
converts back to a loose one without a temporary copy.

## Plan (status as of session 1)

1. ✓ **Core library + host CLI** in portable C++20, errors as values.
2. ✓ **Dump → FFPFSC** first: exFAT writer, AMPR index, PFSC codec, FFPFSC
   wrapper. Test byte-for-byte against PS5PKGTool/MkPFS with a fixed
   timestamp.
3. ✓ **FPKG reading** (debug packages): crypto (AES-XTS/CBC, SHA-256, SHA3, HMAC from mbedTLS or
   small standalone code), Kraken decoder, package reader, then
   FPKG → exFAT/FFPKG/FFPFSC by streaming.
4. **FFPKG (UFS2)**: ✓ read; write to do (ShadowMountPlus's recommended format).
5. **Dump/image → FPKG** builder (Kraken encoder; offer "Stored" because
   encoding on the PS5 CPU will be slow).
6. **PS5 front end.** The end goal is a PS5 homebrew; the CLI is the test
   bed. Following PS5-LZ4-Compression: the engine runs as a payload ELF
   (full `/data` and USB access) and a ps5-homebrew-ui app is the front end.
   Keep the core portable (POSIX only, no exceptions/RTTI) and add a
   ps5-payload-sdk cross-build early to catch problems.
7. ✓ **LZ4 asset packs** (AMPRPAK4/AMPRDAT3): pack, verify, unpack, as a tree
   transformation usable with every output format.
8. ✓ **Multi-threaded PFSC compression** (output identical for any thread count).
9. **PFS images (.ffpfs)**, standalone and as FFPFSC payload: read and write.

## Progress

### 2026-10-17: skeleton, PFSC codec, exFAT writer, AMPR index

- CMake project: `alembic_core` (static library), `alembic` (CLI),
  `alembic_tests` (own tiny harness in `tests/test.hpp`, no dependencies).
  Built with `-fno-exceptions -fno-rtti`; only external dependency is zlib.
  Build: `cmake -S . -B build && cmake --build build && ./build/alembic_tests`.
- Errors are `Status` / `Result<T>` (`result.hpp`) with `ALEMBIC_TRY` and
  `ALEMBIC_ASSIGN`. File I/O is plain POSIX (`io.hpp`), so it should carry
  over to the PS5's FreeBSD libc. No `std::filesystem`.
- Ported: `PfscCodec` → `pfsc.hpp`, `ExfatImage` → `exfat.hpp`,
  `AmprIndex` → `ampr.hpp`. The exFAT writer pushes bytes into a `ByteSink`,
  and `pfsc::Encoder` is a `ByteSink`, so dump → exFAT → PFSC can stream with
  no temporary image.
- CLI: `alembic exfat`, `ampr-index`, `pfsc-encode`, `pfsc-decode`,
  `pfsc-info`.
- **Oracle check passes byte for byte.** `tools/oracle/` is a .NET app that
  calls PS5PKGTool's own code; `tools/oracle/compare.sh [PS5PKGTool dir]`
  (default `.refs/PS5PKGTool`, git-ignored) builds 5 fixture folders and
  compares exFAT images at 3 cluster sizes plus PFSC both ways. All identical,
  including the PFSC container bytes on macOS with system zlib.

Findings worth remembering:

- PS5PKGTool's AMPR hash uses FNV offset basis `1469598103934665603`, the
  standard `14695981039346656037` with a digit missing. We keep it so our
  indexes match theirs (and whatever reads them on the console).
- File names are sorted by .NET `ToLowerInvariant()` then UTF-16 ordinal. We
  ship .NET's exact mapping in `src/core/data/lowercase_table.inc`
  (regenerate with `dotnet run --project tools/oracle -- lower`). Note .NET
  leaves `İ` (U+0130) unchanged.
- PS5PKGTool skips symlinks; Alembic also rejects FIFOs, sockets and devices
  instead of treating them as empty files.

### 2026-10-17: FFPFSC (dump folder → FFPFSC works end to end)

- Ported `FfpfscImage` → `ffpfsc.hpp`: create from a dump folder (exFAT
  streamed straight into the PFSC encoder, no temporary image) or from an
  image file; inspect; extract the payload image; verify (decode every block,
  SHA-256, optional compare with the original image).
- CLI: `alembic ffpfsc <dump|image> <out>` (`--timestamp`, `--inner-name`,
  `--block-size`, `--case-sensitive`, `--overwrite`, compression and exFAT
  options), `ffpfsc-info`, `ffpfsc-extract`, `ffpfsc-verify`.
- New helpers: strict JSON reader (`json.hpp`, enough for param.json),
  SHA-256 (`sha256.hpp`), `AtomicOutput` overwrite + parent folder creation.
- Oracle: 15 FFPFSC containers byte-identical to PS5PKGTool with a fixed
  timestamp, including param.json edge cases and nested game-root search.
  38 comparisons in total, all identical.
- 574 MB dump: 3.8 s, 6 MB peak memory; PS5PKGTool 3.9 s, 63 MB. Both are
  bound by single-threaded zlib. PFSC blocks are independent, so compressing
  on several threads is an easy win later (output stays identical).

FFPFSC naming rules (PS5PKGTool behaviour we match):

- Payload name is `<TITLEID>.exfat` when a top-level `titleId` key (any ASCII
  case) holds a string matching `[A-Za-z]{4}[0-9]{5}`; the first such key
  wins, upper-cased. Otherwise the game folder's name with anything outside
  printable ASCII or `"*/:<>?\|` replaced by `_` (per UTF-16 unit), trimmed
  of spaces and dots, plus `.exfat`; `PS5_GAME.exfat` if that is empty.
- param.json must be strict JSON. **A UTF-8 byte order mark makes it invalid**
  (as in .NET's `JsonDocument.Parse(bytes)`), so it falls back to the folder
  name. Checked against the oracle.
- One deliberate difference: .NET's regex `$` also accepts a trailing `\n`,
  which would put a newline into the payload name. We treat it as no match.
- Game root: the folder itself if it has `sce_sys/param.json`, else the one
  sub-folder (any depth) that has it; more than one is an error.

### 2026-10-17: conversions between folder, exFAT and FFPFSC

Priority set by the owner: **conversion between exFAT, FFPFSC and FPKG**
matters most; dump folders are a dated format, kept as a side feature.

- `RandomAccess` (io.hpp): anything readable at an offset. `File`, `Slice`,
  and `pfsc::Reader` (decodes 64 KiB blocks on demand, 16-block LRU cache).
- `tree::Source` (tree.hpp): a file tree plus `open(file)`. Implemented for
  folders (`tree::open_folder`) and exFAT images (`exfat::open_image`, port
  of `ExfatVolume.cs`, files stored as cluster runs). An FFPFSC is
  `ffpfsc::open_payload` + `exfat::open_image`. FPKG will be one more Source.
- The exFAT writer and FFPFSC builder take any tree
  (`ImageBuilder::from_source`, `ffpfsc::create_from_tree`); game-root search
  and payload naming run on the tree. Folder builds go through the same path
  and are still byte-identical to PS5PKGTool.
- `alembic convert <in> <out> [--to F] [--rebuild]` detects the input and
  picks the cheapest path: images are copied/wrapped unchanged unless
  `--rebuild` asks for a fresh deterministic layout. `alembic ls <in>`.
- Oracle: 47 comparisons, all identical. New: PS5PKGTool's FFPFSC files
  extract back to the original trees, and `--rebuild` of PS5PKGTool's exFAT
  images reproduces them byte for byte.
- 574 MB dump: FFPFSC → folder 0.2 s, FFPFSC → exFAT 0.3 s, compressing
  ~4 s (zlib), all under 7 MB of memory.

Notes:

- Rebuilt images write exFAT's fixed 2024-01-01 timestamps, so files
  extracted from our images carry that time, and a rebuilt
  `ampr_emu.index` records it. PS5PKGTool images behave the same.
- FFPFSC payloads that are not exFAT (UFS2, nested PFS) are refused for now.

### 2026-10-17: FPKG reading, first findings

Read so far: ProsperoPkgTool `ProsperoPackageReader`, `OuterPfsReader`,
`ProsperoKeys`, `AesXts`, `ProsperoPs5InnerImageReader`.

- Package: `\x7FFIH` (finalized; signed byte 0x00 debug / 0x80 retail,
  format version 3, PFS at 0x10000, CNT offset at 0x58), `\x7FLIH` (patch,
  wraps an FIH) or bare `\x7FCNT`. CNT header and 0x20-byte entry records are
  **big-endian**; entry 0x200 holds entry names. After the CNT comes the SI
  segment (a ZIP).
- Keys (debug): `ComputeKeys(cid, passcode, i) = SHA3(SHA3(BE32(i)) ||
  SHA3(cid padded to 48) || passcode)`; EKPFS = index 1. XTS keys:
  `inner = HMAC-SHA256(EKPFS, seed)`, `k = HMAC-SHA256(inner, LE32(1) ||
  seed)`, tweak = k[0:16], data = k[16:32]. Seed: superblock + 0x370.
  Default passcode: 32 ASCII '0'.
- Outer PFS: 64 KiB blocks, AES-128-XTS per block; sector = block index for
  `pfs_image.dat` data, `0x800000000000 | index` for metadata and the NAPS
  file; the superblock (found by magic 20130315 at +8) is plaintext. Seed
  `PPRPLAIN-NOAUTH!` = unencrypted outer PFS. S32 inodes (0x2C8 bytes), 12
  direct + 5 indirect block signatures (32-byte hash + index).
- Inner image: `pfs_image.dat` is a raw stream of stored or header-stripped
  Kraken blocks (up to 256 KiB, as two 128 KiB sub-chunks); geometry only in
  `naps_pkg_layout.dat` (9-byte bit-packed CblockInfo records, RUN/STD
  entries, dedup aliases, zero holes). The reconstructed mount is a PFS image
  holding the file tree. `sce_sys/param.json` and media live in **CNT
  entries**, not in the inner image; the builder adds `sce_sys/keystone`,
  `pfs-version.dat` and `about/right.sprx` to the inner image.

**Interoperability test** (`tools/pkg-oracle/`: a LibProsperoPKG harness;
`patch_libprosperopkg.py` adds a managed SHA3-256, because .NET on macOS
has none):

| package built by | ProsperoPkgTool reads it | LibProsperoPKG reads it |
|---|---|---|
| ProsperoPkgTool, Kraken | yes, byte-exact | no |
| ProsperoPkgTool, stored | yes, byte-exact | no |
| LibProsperoPKG (Kraken) | no ("no supported NAPS interpretation") | no (its own message: Kraken layouts "not yet fully supported") |

ProsperoPkgTool's reader is the most complete: it recognises several NAPS
"dialects" (a Sony debug fixture, LibProsperoPkg 1.2.0 binary output, its own).
Neither has been checked against packages made by Sony's tools on our side,
and **we have no real FPKG samples yet.** That is the main risk for FPKG
reading.

### 2026-10-17: FPKG reading, layer 1 (envelope, keys, outer PFS)

- Own crypto, written from the standards and checked against official test
  vectors (`crypto.hpp`): AES-128 (table-driven) with XTS and CBC, SHA3-256,
  HMAC-SHA256. No crypto library dependency, so it carries over to the PS5.
  AES-NI can be added later for speed (the PS5's Zen 2 has it).
- `fpkg.hpp`: reads FIH/LIH/CNT packages, CNT entries (names, protected-entry
  AES-CBC decryption), checks the key-free digests (FIH superblock digest,
  CNT entry digests), derives keys, decrypts the outer PFS and exposes its
  files and the `pfs_image.dat` data region as `RandomAccess`.
- CLI: `alembic fpkg-info <pkg>`, `alembic fpkg-dump <pkg> inner-image|<outer
  file>|entry:<name> <out>`. A wrong passcode is reported as such.
- `tools/pkg-oracle/compare.sh`: builds packages with ProsperoPkgTool
  (stored, Kraken) and LibProsperoPKG, then checks digests + ICV, the
  decrypted inner image (byte-identical to ProsperoPkgTool `native-decrypt`)
  and the `param.json` entry. 9/9 pass, **including LibProsperoPKG's
  package**, so the disagreement between the tools is confined to the NAPS
  layer above this one.
- Not done yet: protected-entry decryption is implemented but untested (no
  test package has protected entries); patch (LIH) packages are parsed but
  not decrypted.

### 2026-10-17: FPKG reading works on real packages

The owner keeps test files in `.refs/samples/` (git-ignored; never name them
in the repo, see CLAUDE.md): three debug FPKGs (A, B, C below), an exFAT dump,
an FFPKG (UFS2) and a large FFPFSC.

- Layer 2 ported from ProsperoPkgTool: NAPS parser and the four dialects
  (`compatible-coffset-v1`, `native-span-v1`, `indexed-coffset-v1`,
  `sparse-fidx-v1`, with the same file-system probe to break ties), the
  Kraken decoder (`kraken.hpp`; every input-derived index bounds-checked; C#
  shift semantics preserved), the on-demand inner mount, the inner PFS reader,
  and `fpkg::open_tree`: the inner `uroot` plus `sce_sys` files restored from
  CNT entries (protected ones decrypted). FPKG is now a `convert` input.
- Package A (350 MB, 1.8 GB decoded) uses `sparse-fidx-v1` (Sony's writer,
  with an unencrypted outer PFS, so most likely built by Drakmor's closed
  LibProsperoPkg). Packages B (1.4 GB) and C (124 MB) use `native-span-v1`.
- `tools/pkg-oracle/check_samples.sh` on all of them: every file and the
  **whole decoded inner image** byte-identical to ProsperoPkgTool; 456 Unity
  serialized files report their own size correctly (an independent check);
  FPKG → exFAT → folder round-trips. The exFAT sample (35,260 files) extracts
  identically to PS5PKGTool's reader and survives exFAT → FFPFSC → exFAT
  byte for byte.
- Protected-entry decryption verified: `nptitle.dat` (NPTD), `npbind.dat`
  (D294A018), `license.dat` (RIF).
- Speed (Apple Silicon), package A: → folder 1.1 s / 46 MB RAM
  (ProsperoPkgTool: 2.5 s / 5.3 GB); → exFAT 1 s; → FFPFSC 69 s, all of it
  single-threaded zlib level 9.
- LibProsperoPKG's current output uses NAPS shuffle; refused explicitly
  ("shuffled NAPS blocks are not supported"), as no reader decodes it.
- FIH 0xB0 is not SHA3(decoded mount) in package A, despite LibProsperoPKG's
  docs; its meaning is unknown.
- A 12.9 GB FFPFSC (29.7 GB exFAT payload, 452,736 PFSC blocks): the decoded
  payload's SHA-256 equals PS5PKGTool's. Ours took 105 s vs 46 s: most of
  it is our scalar SHA-256 (.NET uses the CPU's SHA instructions). Use
  hardware SHA/AES where available in the performance pass.
- The FFPKG sample is a UFS2 image (superblock magic 0x19540119 at
  0x1055C) of a PS5 app (`param.json`, a homebrew-range title ID), not a
  PS4 dump.

### drakmor/ShadowMountPlus (GPL-3.0): the formats the console loader mounts

| Extension | File system | SMP status | Alembic |
|---|---|---|---|
| `.ffpkg` | UFS2 | **recommended** default | read ✓ (write: to do) |
| `.exfat` | exFAT | for titles needing external-drive behaviour; use 64 KiB clusters | read + write ✓ |
| `.ffpfs` | PFS (uncompressed, MkPFS `--version PS5 --inode-bits 32`) | experimental | **missing** |
| `.ffpfsc` | PFSC container with a nested image (exFAT, or PFS `.ffpfs` / `pfs_image.dat`) | experimental; console decompression only ~150-250 MB/s | exFAT and UFS2 payloads ✓; PFS payload **missing** |

Images must hold the game at their root (`/sce_sys/param.json`), no extra
folder. SMP picks the format by file extension.

Alembic's exFAT images use 64 KiB clusters by default (since 2026-10-18), which
SMP's exFAT fast path (LVD/BFS) needs. `--cluster-size ps5pkgtool` keeps
PS5PKGTool/MkPFS's rule (32 KiB unless the average file is >= 1 MiB); the
parity scripts pass it. SMP's "use a cluster size of no more than 64 KB" toast
is about the USB drive's own format (shown on connect), not about the image.

### 2026-10-17: UFS2 (.ffpkg) reading

- `ufs2.hpp`: UFS2 reader written from FreeBSD's layout, with UFS2Tool's
  (BSD-2) offsets and checks. Regular files and folders; symlinks and device
  nodes skipped; real modification times; direct + single/double/triple
  indirect blocks; holes read as zeros. UFS1 refused.
- FFPKG is a `convert` input; FFPFSC payloads may be UFS2; an FFPFSC with a
  UFS2 payload unwraps to `.ffpkg` unchanged. Writing UFS2 is still to do.
- The exFAT writer now refuses names that differ only by case (UFS2 is
  case-sensitive, exFAT is not) instead of writing a broken image.
- Sample check: the FFPKG extracts identically to PS5PKGTool's UFS2 reader,
  survives FFPKG → FFPFSC → FFPKG byte for byte, and FFPKG → exFAT → folder.
- The CLI prints live progress only on a terminal.

### 2026-10-17: multi-threaded PFSC compression

- Owner tested an Alembic FFPFSC (from package B) on a PS5 with
  ShadowMountPlus: **it mounts and runs.**
- `pfsc::Encoder` now compresses on worker threads (`CompressionOptions::threads`,
  CLI `--threads N`, default all cores): a ring of 64 KiB block slots, each
  worker with its own zlib stream, results written strictly in order, so the
  output is byte-identical for any thread count (checked; PS5PKGTool parity
  47/47 still).
- Package B (2.7 GB image) on a 14-core M-series Mac: compression 114 s →
  11.4 s; FPKG → FFPFSC end to end 121 s → 12.4 s, 40 MB RAM.

### 2026-10-18: LZ4 asset packs (AMPRPAK4 / AMPRDAT3)

- `lz4pack.hpp` + `src/core/lz4pack_{format,build,read}.cpp`: port of
  drakmor/ampr_emu's `tools/ampr_pack.py` / `ampr_pack_format.py` (format,
  rules, auto-loose sampling, lane assignment, page-safe placement, lane/group
  de-duplication, striping, volume rollover, build ID, CRC and runtime
  sidecars) and of PS5-LZ4-Compression's automatic profile (its
  `src/ampr_pack_profile.c`: safety list, hot audio rules, four balanced lanes
  of up to 16 GiB, runtime cache budget, keep-on-disk list). LZ4 1.10.0 is
  vendored from the official tag in `third_party/lz4/` (with its xxHash).
- **CLI:** `alembic convert <in> <out> --lz4 --lz4-runtime libSceAmpr.sprx`
  (folder, exFAT or FFPFSC output; `--lz4-level` 1-12, default 9 like
  PS5-LZ4-Compression; `--lz4-block-size`; `--lz4-config-out DIR` writes the
  `pack.toml` + `excludes.txt` that make `ampr_pack.py` produce the same set);
  `--lz4-unpack` turns a packed tree back into loose files; `lz4-info`,
  `lz4-verify`. `--progress` prints progress lines for logs. `alembic cat
  <in> <path> [out]` pulls one file out of any input.
- **Streaming, two passes, no temporary copy:** pass 1 compresses every block
  to fix sizes, offsets and the build ID (keeps ~80 bytes per chunk); pass 2
  compresses again while the writer reads each volume (read-ahead on all
  cores) and checks every block against pass 1. Source reads stay on one
  thread (the FPKG/UFS2 readers cache and are not thread-safe).
- **Output tree:** the original tree minus the packed files, plus
  `ampr_emu.index` (built from the original tree plus the runtime, so file IDs
  match the pack records), `ampr_assets.index` + `.crc` + `.runtime`, the
  volumes and `fakelib/libSceAmpr.sprx`. The exFAT writer now keeps the
  `ampr_emu.index` of a tree that has `ampr_assets.index` (rebuilding it would
  renumber the files and break the packs). Folders emptied by packing stay.
- **Checked:** `tools/lz4-oracle/compare.sh` (synthetic game, three
  level/block-size settings, `--big` adds auto-loose files): manifest, `.crc`,
  `.runtime` and every `.pak` byte-identical to `ampr_pack.py pack`, build ID
  included; `ampr_pack.py verify` accepts Alembic's output; packed exFAT and
  FFPFSC extract to the same files as the folder output and `--lz4-unpack`
  restores the original tree. Oracle setup: `python3 -m venv .refs/venv`;
  the Python `lz4` wheel bundles LZ4 1.9.4, so the script instead lets
  `ampr_pack_format.py` load Homebrew's liblz4 1.10.0 through ctypes
  (`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`, no `lz4` module installed).
- **Runtime binary:** ampr_emu 0.4.2.1's `libSceAmpr.sprx` (PackedStdio
  build) from the official release
  (`gh release download 0.4.2.1 -R drakmor/ampr_emu -p libSceAmpr.sprx`),
  SHA-256 `69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a`,
  the same file PS5-LZ4-Compression and Lazy_AMPR pin. Kept in
  `.refs/runtime/ampr_emu-0.4.2.1/` (git-ignored), never committed.
- The packer refuses a tree without `fakelib/libSceAmpr.sprx` unless
  `--lz4-runtime` is given: without the PackedStdio runtime the game cannot
  read its packed files.
- Fidelity notes: like `ampr_pack.py`, duplicate blocks are shared only
  within one lane by default; PS5-LZ4-Compression's video rule
  (`**/movies/**`) does not match a top-level `movies/` folder (ported as is).

- **Package D** (owner sample, an 87 GB debug FPKG of a 149 GiB game, almost
  all in 74 large archive files): `alembic convert D.pkg D.exfat --lz4
  --lz4-runtime .refs/runtime/ampr_emu-0.4.2.1/libSceAmpr.sprx` took 9.6 min
  on the 14-core Mac (pass 1 4.7 min, pass 2 + exFAT 4.9 min), 456 MB peak
  RAM. 227 of 284 files packed, 22 left loose by sampling (per-language
  archives, 0-4% LZ4 gain); 136.5 GiB packed into 102.4 GiB of volumes
  (8 volumes, 4 lanes, 16 GiB max), 12.2 GiB loose; exFAT image 114.7 GiB
  (149 GiB unpacked). `lz4-verify`: every chunk decodes and matches its CRC
  (116 s). `alembic same D.pkg D.exfat`: all 283 files identical after
  unpacking (the only extra file is the runtime). **Not run on a console
  yet**: whether this engine reads its archives through paths ampr_emu
  intercepts (not mmap) is only known on hardware. The profile also packs a
  1 MiB `playgo-languages/*.bin`, as PS5-LZ4-Compression would; first suspect
  if PlayGo complains.
- `alembic same <a> <b>` compares two inputs file by file (unpacking LZ4 on
  either side).

### 2026-10-18: first console test of an LZ4 pack, 64 KiB clusters

- Owner tested package D's LZ4 exFAT on the PS5 with ShadowMountPlus: it
  mounts, but the game reports that its pack files are missing. The message
  is the game's own (not in ampr_emu or SMP): it cannot see the archives that
  were moved into the volumes. The runtime in the image is the right one
  (same SHA-256 as the release's `-test-pack` build, which intercepts ordinary
  reads, `stat` and directory listing). Suspects, in order:
  1. SMP's `update_emulators=1` (default) replaces the game's
     `fakelib/libSceAmpr.sprx` with any same-named file in
     `/data/shadowmount/emus/` (an older or no-pack build turns packing off);
     check that folder and `/data/shadowmount/debug.log`.
  2. The game maps its archives with `mmap`, which ampr_emu cannot serve;
     those files would have to stay loose.
  The debug runtime (`libSceAmpr.sprx-0.4.2.1-test-debug-pack`) logs to
  `/app0/ampr_emu.log`, so it needs a writable folder install, not an image.
- Second test image: package B (a Unity title whose Alembic FFPFSC already
  runs on the console) as an LZ4 exFAT, written beside the sample. 434 of 497
  files packed, 2.22 GiB -> 1.61 GiB in 4 volumes, 0.35 GiB loose; image
  2.0 GiB, built in 11 s; `lz4-verify` and `same` pass. Package B ships its
  own `fakelib/` (other libraries); Alembic adds only `libSceAmpr.sprx`.
- exFAT default cluster size is now 64 KiB (see the SMP section).
- **Console result for package B (scan profile): crash at start**,
  `0xa0020318 SYSTEM_ABNORMAL_TERMINATION_REQUEST` in eboot.bin 3 s in. Cause:
  the scan profile packed Unity's own data files (`level*`, `*.resS`,
  `globalgamemanagers`, `sharedassets*`, `boot.config`). PS5-LZ4-Compression's
  console-tested `unity.toml` says exactly this makes Unity abort at start and
  packs only `StreamingAssets/` (`global-metadata.dat` must stay loose: IL2CPP
  mmaps it). Every tested profile starts from `default_action = "loose"` and
  packs only the engine's known data; their notes say packing everything
  crashed the game. Package D (another engine: large archive files plus small
  index files next to them) has no tested profile; the scan profile packed
  the index files too (UFC 6's notes: packing its `.toc` files crashed it).
  SMP log also showed the USB drive uses 128 KiB blocks (the toast).
- **LZ4 profiles** (`lz4pack_profile.cpp`, `toml.cpp`): `--lz4-profile
  auto|scan|<id>|FILE.toml`. `config_from_toml` reads ampr_pack.py TOML like
  its `load_config` (unknown keys ignored, `required_packed` refused for now);
  `--lz4-level` replaces `[pack] compression_level` as PS5-LZ4-Compression
  does; the safety list is always forced loose. Bundled profiles (astrobot,
  ufc6, insomniac, unity) are embedded from `profiles/lz4/` at build time and
  picked by title ID, then by engine layout. Unity detection also accepts the
  PS5 `Media/` layout (`Media/globalgamemanagers`,
  `Media/Metadata/global-metadata.dat`), which PS5-LZ4-Compression's check
  (`Data/globalgamemanagers` only) misses, package B included. No match: the
  scan profile with a warning. Name matching (param.json `titleName`) is not
  ported. LZ4 oracle: unity and insomniac profiles byte-identical to
  `ampr_pack.py pack --config <same toml>`, at the profile's level and with
  the override.
- Package B rebuilt with `unity.toml`: 93 files packed, 0.53 GiB, but almost
  no saving (Unity's bundles are already compressed); 2.79 GB image. It tests
  that runtime + packs run, not the size win. For Unity games FFPFSC (zlib,
  1.51 GB for package B) is the better size option.
- **Console result (owner): package B with `unity.toml` runs from internal
  storage.** From USB (exFAT drive with 128 KiB clusters) it hung on a black
  screen. SMP mounts USB images with its legacy-1.6 profile (the optimized
  one is only for `/data/...` and `/user/...`); untested whether a 64 KiB
  cluster drive fixes it. Until then: LZ4-packed images belong on internal
  storage.

### 2026-10-18: PS5 build and the conversion engine

- **Toolchain on macOS:** `brew install llvm@18` and ps5-payload-dev/sdk v0.44
  (release zip `ps5-payload-sdk.zip`, SHA-256
  `4d06eab777bb135705b5a6816688c9132efb246093fe606ac488ab6cf65ca5b0`) unpacked
  to `.refs/ps5-payload-sdk` (git-ignored). `tools/ps5/build.sh` sets
  `PS5_PAYLOAD_SDK` / `LLVM_CONFIG` and builds into `build-ps5/` with the SDK's
  CMake toolchain. The SDK ships libc++ and pthreads but no zlib, so zlib
  1.3.1 sources are vendored in `third_party/zlib` (used on the PS5 by
  default). The whole core, the CLI and the unit tests build for the PS5
  with no warnings; `int64_t` is `long` there (`long long` on macOS).
- **Engine** (`src/engine/`, `alembic.elf` on the PS5, `alembic-engine` on a
  PC): one worker thread runs queued conversions in order; the queue is saved
  to `<state>/jobs.json` (PS5: `/data/alembic`) after each change; a job that
  was running when the engine died comes back as `interrupted` and its
  `AtomicOutput` temp file is deleted. HTTP JSON API on port 5940 (all
  interfaces, no authentication, like PS5-LZ4-Compression) plus a built-in
  web page (`src/engine/web/index.html`): queue with progress, speed and time
  left, cancel / retry / remove, a file browser limited to the allowed roots
  (PS5: `/data`, `/mnt/usb0-7`, `/mnt/ext0-1`), inspect (format, title ID,
  size, matching LZ4 profile) and the new-job form. Notifications when a job
  finishes; `sceSystemServicePowerTick` every 20 s while one runs. LZ4 jobs
  use `<state>/runtime/libSceAmpr.sprx` when the game has no runtime; `/api/info`
  reports whether it is the pinned build.
- Checked on the Mac (engine on 127.0.0.1): queueing, two jobs in a row
  (package B → FFPFSC in 6 s through the queue), cancel while queued and while
  running (temp file removed), output conflicts (409), paths outside the roots
  (403), kill -9 mid-job and restart (job interrupted, temp removed, next job
  runs), `/api/quit`.
- **Owner: the engine works on the PS5** (loaded with the ELF loader, web
  page on port 5940).
- **File manager + delete source** (same day): web page rewritten with Files
  and Queue tabs, drive free space, dialogs (works without `<dialog>` support
  too). Jobs have a `kind`: `convert`, `copy`, `move` (copy/move run in the
  queue with progress; files go through `<dest>.alembic-partial`; a failed
  copy removes what it created; move is a rename on the same file system,
  else copy + delete). Instant: `/api/fs/mkdir`, `rename`, `delete`, `space`.
  Roots, the engine's state folder and anything a queued or running job uses
  cannot be renamed or deleted. `delete_source` on a conversion deletes the
  input only after success and, with `verify` (default on), after
  `convert::compare` (moved from the CLI's `same` into the core) finds every
  file identical (the LZ4 runtime the packer adds is allowed); a source that
  another queued job still reads is kept. Package B: convert + check +
  delete 14 s (6 s without the check).
- Owner feedback round: elapsed time comes from the engine (`elapsed` in
  `/api/jobs`; the page no longer compares the console's clock with its own);
  `/mnt/*` roots count only when a drive is mounted exactly there
  (`statfs().f_mntonname`), for listing and for every path check; a new
  icon (`assets/icon/`: SVG for the page and favicon, served at `/icon.svg`;
  a full-bleed square and its 512×512 `icon0.png` for the PS5 app tile).
- **Output elsewhere:** jobs take `output_dir` / `output_name` (default: the
  input's folder and name), so a USB source can go straight to internal
  storage. `/api/locations` suggests `/data/homebrew` (ShadowMount+ scans it),
  `/data` and, per mounted drive, `<drive>/homebrew` and `<drive>`; the page's
  "Save to" lists them plus recent folders (browser storage) and a folder
  picker; Copy/Move get the same shortcuts.

### 2026-10-18: package install, and the FPKG writer plan

- **Install** (`kind: "install"`, Files → ⋯ → Install on `.pkg` files):
  `sceAppInstUtilInstallByPackage` with the local path, as in
  ps5-payload-dev/shsrv `bundles/pkg_install` (GPL-3.0); the system installs
  in the background (Downloads). The engine links `libSceAppInstUtil`.
  Not tried on a console yet.
- **FPKG output is the owner's priority** (image → installable PKG and back).
  Key material: both reference builders embed Sony publishing-tool blobs
  (ProsperoPkgTool `Keys/passcode.bin`, `mount_image.bin`,
  `metadata_modulus.bin`: public RSA-3072 moduli, the header "signature" is
  an RSA encryption under the metadata modulus, so no private key;
  LibProsperoPKG also ships `pkg_meta_rsa_key.pem`). Decision: Alembic ships
  none of them; the user supplies the files, checked by SHA-256.
- **First, find out which builder's output installs.** Neither reference has
  ever been installed on a PS5. Test packages of the 124 MB sample package, built from its extracted files minus the licence and PlayGo files (as
  fpkg-cli advises), are next to it in `.refs/samples/`: `-ppt-stored.pkg`
  and `-ppt-kraken.pkg` (ProsperoPkgTool 0.3 `img_create`); both read back
  with every file identical (plus regenerated PlayGo files). LibProsperoPKG's
  output uses the shuffled NAPS layout, which Alembic refuses and fpkg-cli
  reports a console does not load, so it was not offered. Owner to install
  the original, then each test package, and launch them.
- Then port the builder that works (ProsperoPkgTool is MIT, clean-room:
  inner PFS, Kraken encoder or stored, outer PFS AES-XTS, CNT, PlayGo, debug
  licence), streaming from any `tree::Source`, checked byte for byte against
  it on the samples.

- **Components downloader** (owner: fetch dependencies in the app instead of
  bundling them). `src/engine/deps.cpp` lists each file with a fixed URL,
  size and SHA-256; `POST /api/deps/ID/install` downloads all of a
  component's files, checks every hash, and only then writes them under the
  state folder (`runtime/libSceAmpr.sprx`: ampr_emu 0.4.2.1 release asset;
  `keys/*.bin`: ProsperoPkgTool's public moduli at commit `f2c91cd7a5d8`).
  HTTPS on the PS5 uses `libSceHttp`/`libSceSsl` with the four public CA
  roots from ShadowMountPlus (`src/engine/github_ca.inc`: Sectigo E46 /
  USERTrust ECC for github.com, ISRG Root YR / X1 for the
  githubusercontent.com hosts, checked 2026-10-18); a PC uses curl. The
  page has a Components tab; the Convert dialog links to it when the
  runtime is missing. Tested on the Mac with real downloads; not yet on the
  console. The owner cannot install FPKGs on their console, so FPKG install
  tests wait.
- Components are detected per job: `missing_dependencies()` (runner.cpp)
  reports the runtime for an LZ4 job when the runtime file is missing and the
  game ships no `fakelib/libSceAmpr.sprx` (checked by opening the input,
  cached per path/size/mtime); `/api/plan` returns them as `needs`, and
  `POST /api/jobs` refuses such a job (409 with `needs`). The Convert dialog
  says what will be downloaded, asks on "Add to queue", downloads, then
  queues; declining keeps the dialog and its settings.
- **Game icons and titles in the file list:** `/api/meta` and `/api/icon`
  (`game_meta.cpp`) read `sce_sys/param.json` (title: en-US, else the
  default language; title ID; contentVersion) and `sce_sys/icon0.png` from
  any input (FPKG, exFAT, FFPFSC, FFPKG, game folder), at most two inputs
  open at a time, cached in memory and in `<state>/cache/meta/<hash>.json|png`
  (hash of path, size, mtime). Listings flag game folders (`game: true`).
  The page loads them for rows on screen (IntersectionObserver), two at a
  time. On the Mac every sample type gives title, version and icon; first
  read 0.02-0.3 s, cached 0.02 s. The icon cache is never pruned (about
  0.1-0.8 MB per game).

### 2026-10-18: FFPKG (UFS2) writer

- `src/core/ufs2_image.cpp`: port of UFS2Tool's `Ufs2ImageCreator` as
  PS5PKGTool's `Ufs2Operations.CreateFromDirectoryAsync` drives it (UFS2,
  sector 4096 normalised to 512 for the layout, 32 KiB blocks / 4 KiB
  fragments, 256 KiB per inode, minfree 0, optimisation "space", no soft
  updates, no .snap; volume name = title ID). Same sizing (24-byte
  estimate per directory entry, CG-overhead sizing, the inode check, and the
  25 % retries with the exact ffs_size_dir size), same allocation order
  (inodes pre-order; a directory's blocks after its children; files' blocks
  with their single/double/triple pointer blocks in between; tail fragments
  of files without indirect blocks free), same CG headers, bitmaps, cluster
  maps, summaries and superblock, di_gen from .NET's seeded `Random(cg + 1)`
  (ported Knuth subtractive generator). PS5PKGTool writes an empty FS and
  patches it; Alembic plans everything first (extents of file / directory /
  pointer blocks, ~20k for a 150 GB game) and streams the image in order, so
  any tree (FPKG, image, folder) becomes an FFPKG without a temporary copy.
- PS5PKGTool quirks kept: the image grows past fs_size when the last CG is
  smaller than its metadata (it still writes that CG's header and inode
  table; about 3 % of sizes); permissions 0555. Not kept: names are UTF-8
  (PS5PKGTool's Encoding.ASCII turns non-ASCII into '?' and then fails its own
  verify), folder order is sorted (PS5PKGTool takes the host's listing order:
  NTFS's case-insensitive order on Windows, which Alembic uses), and macOS
  junk files are skipped like everywhere else in Alembic.
- Checked: `tools/oracle/compare.sh` now runs a patched copy of PS5PKGTool's
  UFS2 code (`patch_ufs2.py`: clock and PID from environment variables,
  sorted listings, namespace `UFS2ToolPatched`; the checkout is untouched):
  7 fixtures (ASCII copies) and a small-last-CG size byte-identical (55/55
  checks). By hand: the extracted 1.4 GB, 124 MB and 350 MB sample packages
  byte-identical (files over 128 MB, double-indirect), FPKG -> FFPKG directly
  equals folder -> FFPKG, 60 000 long names in one folder (directory
  indirect block) identical. The retry path never triggered: the inode
  sizing leaves far more room than directory entries need. Package B: 1 s
  (PS5PKGTool 4 s). Engine and page offer FFPKG as a target.
- Not yet tried on the console (ShadowMount+ mounts `.ffpkg` by extension).

### 2026-10-18: FPKG writer, research (not started in code)

- Port target: ProsperoPkgTool 0.3 (MIT) `img_create` ->
  `ProsperoDebugPackageBuilder.Build` (Containers/): inner image
  (`ProsperoInnerImageAssembler`, stored or Kraken), NAPS
  (`ProsperoNapsPlanGenerator` + `ProsperoNapsLayoutBuilder`), outer PFS
  (`ProsperoOuterPfsBuilder`, AES-XTS with keys from EKPFS + outer seed), CNT
  (`ProsperoCntBuilder`, entries: standard keys, param.json, imagedigs,
  PlayGo chunk/hash-table/ficm, sce_sys media), FIH (`ProsperoFihBuilder`,
  built twice around the SI segment). ~10k lines incl. Kraken encoder.
- Determinism: output is random only through `OuterSeed` (16 bytes) and the
  entry keys; `DeterministicEntryKeys = true` + fixed `OuterSeed` + the
  default timestamps (0x6A31A5B9 s / 0x14DC9380 ns) make it reproducible ->
  byte-parity oracle possible (needs a small C# oracle setting those options;
  the CLI only exposes `--seed`). Two plain builds differ in ~80 M bytes.
- img_create input: `Directory.EnumerateFiles(src, "*", AllDirectories)`
  ordinal-sorted, skipping .gp4/.gp5/.pkg and `IsExcludedFromInner` paths;
  sce_sys outer entries via `CollectSceSysFiles` (read CliApplication.cs
  ~990-1100 next).
- Key material: the three public moduli (Components: fpkg_keys). Keystone:
  `Crypto/Keystone.cs` hardcodes HMAC keys; dumps from debug FPKGs already
  ship sce_sys/keystone, so v1 reuses it and refuses a dump without one.
  right.sprx: ProsperoPkgTool embeds the same 12,752-byte SELF as
  LibProsperoPKG (origin unclear) -> Alembic never injects it (dumps ship it).
- Plan: (1) stored-mode parity vs the oracle on the 124 MB sample, (2) Kraken
  encoder, (3) streaming for large games, fself of raw ELFs, DDS generation,
  keystone. Install testing waits for the owner's hardware.

### 2026-10-18: web page for the controller, Media-tab shortcut

- **Owner decision:** the native PS5 app (ps5-homebrew-ui) is shelved for
  now; the engine's web page is the front end, opened on the console itself
  and usable with a controller. The app may adopt the same design later.
- **Media-tab shortcut** (`platform::install_launcher`, PS5 only): at start
  the engine writes `/user/app/ALEM00001/sce_sys/{param.json,icon0.png}`
  (`applicationCategoryType` 65536 = Media, `deeplinkUri`
  `http://127.0.0.1:<port>/`) and registers it with
  `sceAppInstUtilAppInstallTitleDir` (by name from the SDK's stub, weak like
  the other system calls); skipped when the registered param.json is the
  same. Same technique as websrv, ps5-web-file-manager and Payload Manager
  (all GPL). Settings → "Shortcut in Media" removes it
  (`sceAppInstUtilAppUnInstall`, and `<state>/launcher.off` so it stays
  removed) or adds it back: `POST /api/launcher {enabled}`; `/api/info` has
  `launcher` and `port`. **Not tried on a console yet.** Payload Manager notes
  an older Games-tab tile must be removed before a Media one shows.
- First console try (owner): the page opened fine, but **no shortcut
  appeared in Media**. Compared with websrv / ps5-web-file-manager / Payload
  Manager: the symbol name does hash to their NID `Wudg3Xe3heE`, so linking by
  name is fine; all three call `sceUserServiceInitialize` first, which we
  didn't (now done in `appinst_init`, shared with package install). websrv
  also sets its auth ID to `0x4801000000000013` (`kernel_set_ucred_authid`);
  second try (still nothing): now raised to that ID only around the
  register/unregister calls and restored after (`AuthIdScope`), and the
  status reports the original auth ID and whether the system recorded the
  title in `/user/appmeta`.
- Third try: the console showed "engine ready on port", i.e. the weak import
  `sceAppInstUtilAppInstallTitleDir` was null although the ELF lists
  `libSceAppInstUtil.sprx` as NEEDED and the symbols as weak imports: the
  loader left them empty. Now every installer function is taken from the
  import if set, else from `kernel_dynlib_handle` + `dlsym` (NID
  `Wudg3Xe3heE` as fallback), loading `/system/common/lib/libSceAppInstUtil.sprx`
  with `sceKernelLoadStartModule` first when needed (PS5-LZ4-Compression's
  `resolve_module_handle`). The result goes into the Settings status and a
  second notification at start.
- Fourth try: the library was found but has neither the name nor the NID
  of `AppInstallTitleDir` on the owner's firmware. All four reference
  projects fall back to `sceAppInstUtilAppInstallAll(0)` (registers every
  folder in `/user/app`); Alembic now does too, and the status lists which
  installer functions are missing.
- Owner, first controller run on the console: D-pad navigation works; **✕ is
  a mouse click at the browser's cursor**, not Enter on the focused item; L1/R1
  switch sections the wrong way round and never reach Settings (cause unknown:
  maybe the browser's own back/forward, maybe a double press). Done: while the
  D-pad is in use (`pointer.mode`, until the cursor really moves > 4 px), a
  trusted click and its mousedown act on the focused item instead of where the
  cursor is. Added Settings → Controller test: logs every key (key, code,
  keyCode), gamepad button index and mapping, click and history event, and
  swallows them, so the owner can report what L1/R1/△/□/L2/R2/Options send.
- **PS5 browser controller map** (owner, Controller test on the console):

  | Button | Page receives | Alembic action |
  |---|---|---|
  | D-pad | ArrowUp/Down/Left/Right | move focus |
  | ✕ | mouse click at the browser's cursor | the focused item (`pointer.mode`) |
  | ○ | Escape | back (dialog, folder, section) |
  | △ | F1 | options for the focused item |
  | □ | F2 | (free) |
  | Options | F3 | (free) |
  | L1 / R1 | F5 / F6 | previous / next section |
  | L2 / R2 | F7 / F8 | page up / down in the list |
  | L3 / R3 | F9 / F10 | (free) |

  F5 is the browser's reload and F6 its address bar, which is why L1/R1
  misbehaved; every F1-F12 now has its default prevented. The Gamepad API
  mapping (standard indices) stays as a fallback and only acts when no key
  arrived for the same press.
- Owner: navigation works on the console. Removed the details panel (Files is
  rail + list; Install etc. are in △ options). Queue cards now show the game's
  icon, title, title ID and version (`jobMeta`: `/api/meta` of the input, or
  of the output for a finished job whose source was deleted).
- Owner: the list's scrollbar showed through dialogs on the console (the old
  WebKit can paint a scrolling area's bar above a fixed layer). Now every
  overlay sets `body.covered` (and marks the overlay below it `.under`), which
  turns the covered scroll areas to `overflow: hidden` (positions kept), the
  overlay gets its own layer (`translateZ(0)`), `-webkit-overflow-scrolling`
  is gone, and scrollbars are thin and dark. Holds for any future page or
  dialog that uses `openOverlay`. Settings now shows the last shortcut result
  (`/api/info` `launcher.status`) and has "Register again" (unregister, then
  register). Also fixed: `PkgMetadata` lacked `slot`/`is_playgo_enabled` and
  `PlaygoInfo` was 0x1B40 bytes instead of 0x2700 (layouts asserted as in
  ps5-web-file-manager), so a package install would have overflowed it.
- **Page rewrite** (`src/engine/web/index.html`), after ps5-homebrew-ui's
  Files screens: drives/places rail, list, details panel for the focused
  item (icon art, title, title ID, version, Convert/Install/More), a
  button-hint bar; sections Files, Queue, Components, Settings (new: shortcut
  switch, text size, engine facts, stop the engine).
  - Input: D-pad = arrow keys (spatial focus by position; up/down stay inside
    the rail/list/details columns; coming back into a column returns to its
    last item); ✕ = Enter/click; ○ = the browser's Back, so folders, sections
    and every dialog push a history entry (○ on the first page leaves the
    browser, as on the console); △ = options (key A); L1/R1 = sections (Q/E).
    The Gamepad API is used where the browser has it, for △/L1/R1, and for
    D-pad/✕/○ only when no key event arrived for the same press.
  - Dropdowns became ‹ value › rows changed with ←/→; checkboxes are switch
    rows; dialogs are sheets whose hints stay visible.
  - Compatibility: older firmwares have a Safari-12-era WebKit (Payload
    Manager's changelog), so no `?.`/`??`, flex `gap`, `inset`, `clamp()`,
    `:focus-visible`, `<dialog>`.
  - Checked in headless Chromium at 1920×1080 and 390 px wide by driving keys
    over DevTools (file browsing, back to the parent row, Convert dialog,
    queue with live jobs, settings, phone layout). **Not verified:** how the
    PS5 browser actually maps the controller (arrow keys, ○ as Back, Gamepad
    API), and the old WebKit.

### 2026-10-18: FFPFS (PFS) writer and reader

- Owner: focus on the writers still missing, FPKG and PFS. PFS first: it is
  smaller, and the FPKG's inner image is a PFS too.
- `.ffpfs` = PFS holding the game directly, as ShadowMount+ mounts it (MkPFS
  `--version PS5 --inode-bits 32`, uncompressed: MkPFS's README says the console
  misreads per-file-compressed raw PFS). `src/core/pfs_image.cpp` ports MkPFS
  1.1.0's `build_pfs` (unsigned, uncompressed): only files are scanned (empty
  folders are left out), paths sorted lower-cased; inodes: super root,
  flat_path_table, [collision_resolver], uroot, folders, files; blocks: header,
  inode table (whole 0xA8-byte D32 inodes per block), super root, path table,
  resolver or one empty block, then each folder and file as one contiguous run;
  flat_path_table = Java hash of `/path` (upper-cased when case-insensitive)
  with 0x20000000 for folders and 0x80000000|offset into the resolver on a
  collision. Planned first, then streamed in block order (any tree, no temp).
  ampr_emu.index is generated like the exFAT writer does (`--no-ampr`,
  `--ampr-scheme`).
- Reader (`pfs::open_image`): the tree under uroot, PFSC-compressed files
  decoded on read; detection: PFS header + a valid FFPFSC wrapper = FFPFSC,
  other PFS = FFPFS; FFPFSCs with a PFS payload open and unwrap to `.ffpfs`.
- Checked (`tools/oracle/compare.sh`, MkPFS with a pinned clock via
  `mkpfs_raw.py`): every ASCII fixture, block sizes 4 KiB-128 KiB,
  case-sensitive, a path-hash collision ("0_"/"1@"), the AMPR index (with
  `--ampr-scheme ps5pkgtool`: MkPFS follows PS5PKGTool's index rules, which
  list ampr_commands.bin; our default ampr_emu scheme leaves it out), MkPFS's
  compressed raw PFS read back, its two-pass FFPFSC unwrapped byte for byte.
  69/69 checks. Package B: folder -> .ffpfs byte-identical to MkPFS (2.79 GB,
  1.4 s vs 1.7 s); FPKG -> .ffpfs directly gives the same image (4.9 s); all
  496 files read back identical.
- Engine and page: FFPFS is a Convert target. Not tried on a console yet.
- MkPFS bug seen: it crashes when param.json is not a JSON object.
- Next: the FPKG writer (ProsperoPkgTool's builder, which PS5PKGTool wraps);
  its inner image is PFS, so this writer is the starting point.

### 2026-10-18: FPKG writer (stored), and a full round trip

- `alembic fpkg-build <input> <out.pkg> --keys DIR [--seed HEX32]
  [--package-passcode P]`: builds a debug FPKG from any input (folder, FPKG,
  exFAT, FFPFSC, FFPKG, FFPFS). Ported from ProsperoPkgTool 0.3 (MIT,
  `f2c91cd`), layer by layer, each checked against `img_create --compression
  stored --seed S --no_right_sprx` (byte-reproducible from its CLI):
  inner image (`fpkg_inner_build.cpp`), NAPS (`fpkg_naps_build.cpp`),
  outer PFS (`fpkg_outer_build.cpp`: streamed, data blocks hashed and
  AES-XTS encrypted on the way), RSA key wraps (`fpkg_keys.cpp`: Montgomery
  RSA-3072 e=65537, MT19937 deterministic PKCS#1 padding), PlayGo
  (`fpkg_playgo_build.cpp`), CNT (`fpkg_cnt_build.cpp`), and the FIH header,
  SI zip (naps_meta_18 XTS-encrypted with fixed keys, meta300 x4,
  playgo-chunk.dat/.crc) and orchestration (`fpkg_package_build.cpp`).
  The package is written in place: outer PFS streamed at 0x10000, then CNT,
  header, and one read-back pass for the SI's whole-image SHA3 and CRC32Cs.
- Byte-identical to ProsperoPkgTool: the test game (3.6 MB) and real
  package C (124 MB; DDS media, protected entries, trophies, its own PlayGo
  files): 1.6 s vs 5.8 s. ProsperoPkgTool crashes on macOS above 1 GiB
  (its file-backed SI path uses IncrementalHash SHA3, which macOS .NET lacks);
  a patch to a copy, like patch_libprosperopkg.py, is next.
- Not ported yet (refused with a clear message): Kraken compression, fake-
  signing raw ELF modules, generating DDS from PNG, generating a keystone.
  right.sprx is never injected (as `--no_right_sprx`). Keys come from the
  engine's fpkg_keys component files, checked by SHA-256.
- **Round trip** (`tools/pkg-oracle/roundtrip.sh <pkg>`): package -> P1
  (fixed seed) -> FFPFSC -> exFAT -> FFPKG -> FFPFS -> P2. The first run
  showed P2 != P1: the exFAT/FFPFS writers rebuilt the game's
  ampr_emu.index (the unpacked package also holds its licence and PlayGo
  files from CNT entries, so the new index listed more files). Every other
  byte matched. **Owner decision: keep a game's own index by default**;
  generate one only when missing; `--ampr-index rebuild` restores the
  PS5PKGTool/MkPFS behaviour (the parity scripts pass it). Now packages A,
  B and C all give P2 == P1 byte for byte, with every file identical at each
  step. (`playgo-chunk.dat` legitimately differs from the original package:
  each build regenerates it for its own layout.)
- Speed to improve: 2.6 GB package in ~30 s (single-threaded SHA3/AES-XTS
  plus the read-back).

### 2026-10-19: FPKG output in convert, the engine and the page

- `alembic convert <in> <out.pkg> --keys DIR [--seed HEX32]
  [--package-passcode P]` (or `--to fpkg`) runs the same writer as
  `fpkg-build`; FPKG to FPKG needs `--rebuild`. LZ4 packing and unpacking
  can target a package too (the writer does not look at `fakelib/`).
- Engine: `to: "fpkg"` uses the fpkg_keys component in `<state>/keys`;
  `/api/plan` lists it under `needs` until it is downloaded. The writer's
  warnings come back in the job result. "Check, then delete" skips
  `sce_sys/playgo-chunk.dat` for package outputs (each build writes its own).
- Page: FPKG is a Convert target ("Debug package to install,
  uncompressed"), hidden for packages as their own format.
- The round trip's last step now uses `convert` (P2 == P1 for two sample
  packages); an engine job exFAT -> FPKG with check-then-delete passed.

### 2026-10-19: page and engine fixes

- Queue cards keep their icon element across progress updates (a new one
  reloaded the picture and flashed the format label).
- Media shortcut: on the owner's firmware it shows in Media while
  `/user/appmeta/ALEM00001/param.json` never appears. The Settings switch now
  follows Alembic's own files in /user/app; the status says what
  /user/appmeta/ALEM00001 holds, to learn where the record is (pending the
  owner's report). Without that file, the shortcut is registered at each start.
- Game info cache (`<state>/cache/meta`, a .json and icon .png per path,
  size and mtime): a .json's time stamp is its last use; at start, entries
  unused for 30 days go, then the least recently used until under 100 MB.
  Settings shows its size and has "Clear cache" (`POST /api/cache/clear`).

### 2026-10-19: checkpoints, pause, resume and queue order

- **Resumable writes** (`alembic/resume.hpp`): FFPFSC (from a tree or an
  image), exFAT and image copies write into `<output>.alembic-partial` and,
  every 1 GiB of input (and when stopped by a pause or an error), sync it
  and save a small State. PFSC writes the offset-table entries of finished
  blocks into their final place before each checkpoint, so a State is just
  {blocks, written, compressed, timestamp, lengths}; on resume the table is
  read back and checked and the file is cut at the checkpoint. The exFAT
  builder can start at any offset and does not read files wholly before it.
  Unit tests stop a run, append junk to the partial file, continue from an
  older checkpoint and require the same bytes as an uninterrupted run.
  Bump `resume::kLayoutVersion` when a resumable writer's bytes change.
- Also resumable since: FFPKG (UFS2; the checkpoint keeps the time stamp and
  process ID that go into the image) and FFPFS (PFS; the time stamp), both
  planned first and streamed, so a continued write drops bytes before the
  checkpoint and does not read files wholly before it; and folder outputs:
  at each checkpoint everything is flushed (`sync()`, not an fsync per file)
  and {files finished in tree order, bytes of the current file} is saved; a
  continued run keeps those files when their size matches, continues the
  current one from its saved length, and rewrites anything after it. Still
  starting over: FPKG output and LZ4 packing.
- **Engine:** the checkpoint lives in `<state>/resume/<hash of output>.json`
  with a fingerprint (job options, input size and mtime, layout version);
  anything else is discarded and the job starts over, as it does when the
  partial file does not match. A job that was running when the payload died
  is queued again at the next start (at most 3 times automatically); one
  stopped with the engine continues at the next start. Cancel, removing a
  failed job and "Clear" delete its partial output (unless another waiting
  job writes the same output).
- **Queue:** paused state; `POST /api/jobs/ID/pause | /resume`, `/move
  {direction}`. The page offers Pause (running), Hold (queued), Resume, Move
  up/down, and shows where a waiting job continues from.
- Checked on the PC engine with a 2.8 GB package -> FFPFSC: pause, two other
  jobs (one moved up) run, resume; and `kill -9` mid-run, restart, the job
  continued by itself. Both outputs have the same payload hash and match the
  package file for file.

### 2026-10-19: languages and the confirm button

- The page speaks English, Spanish, French, German, Japanese and Turkish
  (`src/engine/web/i18n.js`, served at /i18n.js). Texts are keyed by their
  English wording through `t("...", {vars})`; anything missing falls back to
  English. The language follows the browser, and Settings → Language
  overrides it (kept in localStorage). Texts the engine sends (stages, how a
  job was done, place and component names) are translated on the page by
  exact match; error messages stay English. `tools/i18n_keys.py --check`
  lists missing or unused keys and placeholder mismatches; run it after
  changing page texts. Translations are the model's; the owner checks Turkish.
- Confirm button: the page follows the console's own setting. The engine
  reads it (`sceSystemServiceParamGetInt(1000)`: 0 = ○, 1 = ✕, as in the
  public PS4 headers) and reports `enter_button` in /api/info; the browser
  already sends its keys to match, so the page only swaps the ✕/○ hint
  glyphs and its reading of raw Gamepad API buttons. (A Settings switch that
  swapped the actions against the console was tried first and removed: with
  it the owner could not close dialogs.)
- Hold Back (○, or ✕ on ○-confirm consoles) to leave: after 0.7 s every
  dialog closes and the page returns to its first history entry, where one
  more press leaves the browser (a toast says so; window.close() is tried
  too). On the PS5 a held ○ arrives as a stream of Escape keydowns with
  e.repeat false (owner's Controller test), so presses under 250 ms apart
  count as one hold and the third one leaves; the stream is swallowed so it
  no longer steps back one level per repeat.
- The browser keeps △, Options, L3 and R3 for its own actions (owner), so
  the page's second action (Options in Files, Add to queue, Copy/Move here)
  moved from △ to □. The page uses ✕ ○ □, the D-pad, L1/R1 and L2/R2 only.
  Their keys (F1, F3, F9, F10) are no longer stopped either, not even in the
  Controller test; only the function keys the page uses are (F2, F5-F8).
- Engine texts. i18n.js is now `var I18N = <JSON>` with the five language
  tables and a "messages" list: the engine's messages as patterns
  ("cannot write '{path}': {reason}"), longest first. The page's tMsg()
  translates errors, warnings and statuses by exact key or pattern, and the
  matched parts too (system reasons such as "No space left on device"). About
  90 user-facing messages are in it; damaged-file diagnostics and the engine
  log stay English. The engine reads the same file (l10n.cpp) for its console
  notifications, in the page's language (POST /api/lang, saved in
  <state>/lang) or else the console's (`sceSystemServiceParamGetInt(1)`:
  0 ja, 2 fr, 3/20 es, 4 de, 19 tr, as in the public PS4 headers). A new
  engine message that users can see needs a pattern and its translations in
  i18n.js; `tools/i18n_keys.py --check` also covers the notification texts.

### 2026-10-19: the AMPR emulator for games that use APR

- **Why:** games whose eboot.bin imports libSceAmpr read through the
  console's APR, which fails when they run from a mounted image or folder
  (package D: `APR_FATAL_ERROR_ASYNC`; package E: an abort in eboot.bin).
  ampr_emu's `libSceAmpr.sprx` in fakelib/ replaces it; ShadowMount+ never
  adds it (its `emulators_path` only replaces a copy the game ships). Owner:
  E runs as an LZ4 exFAT (the only path that added it) and not as a plain one.
- **Core** (`ampr_emu.hpp`): `check` scans eboot.bin for `libSceAmpr` (a
  plain string in the dynamic string table of a fake-signed SELF or ELF;
  found in the first MiBs on the samples, 0.5 s on the 95 MB eboot of D) and
  picks the fakelib ShadowMount+ mounts (fakelib2 first, used alone, else
  fakelib). Samples: A, B, D and E import it and ship none; C ships one.
  `add` lays the emulator over any tree, plus `ampr_emu.index` when the game
  has none: ampr_emu otherwise builds one at start and saves it to /app0,
  which fails on a read-only image. The index equals what the exFAT/PFS
  writers generate (they only add one when fakelib/libSceAmpr.sprx exists,
  PS5PKGTool's rule), and now `.ffpkg` gets one too (the UFS2 writer never
  made one).
- **Convert:** `convert::Options::ampr_emulator` (+ `_always`): added for
  every output but FPKG (installed packages have the real APR) and not when
  LZ4 packing (the packer adds its runtime). Images are then rebuilt, not
  copied. Without the file the report warns that the game will likely not
  start. CLI: `convert ... --ampr-emu FILE [--ampr-emu-always]`.
- **Existing games** (`attach_to_folder`, `attach_backport`; CLI
  `alembic ampr-emu <in> [--ampr-emu FILE] [--backports DIR]`, without the
  file it only reports): a game folder gets fakelib/libSceAmpr.sprx (and an
  index) in place. An image is left alone; a ShadowMount+ backport folder
  `<root>/<TITLE_ID>/` gets the game's fakelib files (minus libkernel.sprx,
  which SMP drops) plus the emulator, and an index when the image has none.
  SMP lays a backport over an image game with unionfs, and a backport's
  fakelib replaces the game's (`resolve_game_fakelib_source_for_path`:
  backport fakelib2, backport fakelib, game fakelib2, game fakelib). An
  existing backport folder only gets the emulator in its own fakelib. Note:
  for an installed PPR package of the same title SMP deletes libSceAmpr.sprx
  from /data/homebrew/backports/<TITLE_ID>/fakelib before launch.
- **Engine:** `JobSpec::ampr_emu` = auto (default) | always | off, uses the
  runtime component (`<state>/runtime/libSceAmpr.sprx`, the pinned 0.4.2.1
  PackedStdio build, which also serves loose files); `/api/plan` returns
  `ampr_needed` and lists the component under `needs` when the game needs it
  and it is missing. New job kind `ampr_emu` (input: a game folder or image):
  backports go to `/data/homebrew/backports` (SMP's fallback, always
  checked), or to `<scan path>/backports` when a backport for the title is
  already there (the image's folder or the one above). "Check, then delete"
  accepts the added emulator. The component is now "AMPR emulator
  (ampr_emu)".
- **Page:** Convert has "Add the AMPR emulator when the game needs it" (on;
  hidden for FPKG and LZ4) and says when the game needs it; Files → ⋯ →
  "Add the AMPR emulator…" on images and game folders explains what will be
  written and downloads the component first if needed. Texts in all six
  languages (`i18n_keys.py --check`: 0 problems). The page was not opened in
  a browser after these changes (its script parses with node).
- Checked: 65 unit tests (5 new), the PS5PKGTool/MkPFS oracle (69 same),
  PS5 build without warnings, the engine on the Mac (plan without and with
  the runtime, an attach job writing a backport, a conversion adding it,
  check-then-delete of a conversion with it). Package E converted with the
  emulator: every original file unchanged, plus the emulator (pinned
  SHA-256) and the index. **Not tried on the console yet.**

## Next session

**Open (owner, 2026-10-19), in this order:**
1. Games converted PKG -> FFPFSC do not start. The conversion finishes and
   ShadowMount+ finds and installs the image, but launching it errors out
   (a small package and the ~90 GB one; CE-108264-1 for one, CE-108255-1
   for the other: the game process crashing). An earlier Alembic FFPFSC made
   from a dump folder did run. Get the console's error code (CE-...) and
   ShadowMount+'s log. Suspects: what an FPKG source adds or changes
   compared with a dump: the CNT entries Alembic exposes as files
   (licence, PlayGo, param.json), sce_sys contents, the kept ampr_emu.index.
   Compare `alembic ls` of the FFPFSC with a working dump of the same game.
   **Owner's codes (2026-10-19):** the ~90 GB one (package D) gives
   `APR_FATAL_ERROR_ASYNC` (…2c502); the 350 MB one (package E, a Unity
   title from the same packager as B) gives
   `SYSTEM_ABNORMAL_TERMINATION_REQUEST` (…20318) in eboot.bin, the code
   B gave when Unity's data files were LZ4-packed.
   **Findings:**
   - Package D: its eboot imports libSceAmpr, and neither the package nor
     its fakelib/ has ampr_emu's `libSceAmpr.sprx`. ShadowMount+ never adds
     one: `emulators_path` only replaces a same-named file already in the
     game's fakelib (`copy_emulator_files_to_cache` skips it when the
     source file is missing), so a mounted image runs the console's own
     APR, which fails. D's earlier LZ4 exFAT (runtime added by Alembic)
     got past start-up to the game's own "pack missing" message. Likely
     fix: an option to put ampr_emu's runtime in fakelib/ for image
     outputs of games that import libSceAmpr and ship none.
   - Package E: built on the Mac today, its FFPFSC holds the same 321
     files as the package, every block decodes, and its layout matches B's
     (same fakelib files and param.json flags; B imports libSceAmpr too).
     Package B rebuilt with today's code has the same files as the FFPFSC
     that ran, so the writer has not regressed. The owner's failing image
     was made by the engine on the console (in `.refs/samples/` with a
     `-borked` suffix): structure ok, every block decodes, and its exFAT
     payload has the same SHA-256 as the Mac build; only 45 header bytes
     (time stamps, offset table) and the zlib output differ. E's and B's
     eboots import the same 70 libraries. So the conversion is not the
     cause; next: does E run as .ffpkg or .exfat, with ampr_emu in a
     backport fakelib, and what does SMP's debug.log say.
   - Owner: an earlier Alembic FFPFSC of E did run. B's image that ran
     (17 Oct) and B rebuilt today have the same payload SHA-256 and differ
     only in time stamps, so the writer's output for these packages has
     not changed. The console image uses zlib level 9 (`78da`, as
     PS5PKGTool), the Mac default level 6 (`789c`); both decode cleanly,
     no trailing bytes. Suspect the console side instead: SMP version or
     config, its per-title fakelib cache, save data from earlier runs,
     internal vs USB storage.
   - **Owner: E runs as an LZ4 exFAT, crashes as a plain one.** LZ4 mode
     is the only path that adds ampr_emu (`fakelib/libSceAmpr.sprx`) and
     `ampr_emu.index`, so both D and E point at the same cause: without
     the emulator, a mounted image falls back to the console's own APR.
     **Done (2026-10-19, see the entry above):** conversions add the
     emulator, and an existing game can get it. Owner to test D and E.
   - Side finding: `detect_engine` needs a `StreamingAssets/` folder, so
     E (Unity, no StreamingAssets) gets the scan profile, which packs its
     `level*` / `sharedassets*` files; it still ran on the console.
   - Quick test without rebuilding: `/data/homebrew/backports/<TITLE_ID>/
     fakelib/` with the game's own fakelib files plus `libSceAmpr.sprx`
     (a backport fakelib replaces the game's whole fakelib, it is not
     merged).
2. Holding Back is "not working properly" yet; ask what exactly (leaves
   only after release, steps back first, never triggers, or false
   triggers) and the Controller test's timing of the repeated Escapes.
3. Then: checkpoints for FPKG output and LZ4 packing; FPKG speed.

**First (owner, 2026-10-18): the web page on the console.** Load the
payload, check the Media tab shows "Alembic" and opens the page, then try the
controller: D-pad focus, ✕, ○ closing dialogs and going up folders, △, L1/R1.
Fix what the real browser does differently, then continue below.

Agreed order (owner, end of session 1). Each item: port, test against the
references and the samples, commit.

1. **Faster reading** (matters most on the PS5's slower cores):
   - parallel read-ahead decoding: PFSC blocks in `pfsc::Reader`/`decode`
     and Kraken blocks in `fpkg::InnerMount` (decode upcoming blocks on worker
     threads; same ring idea as `pfsc::Encoder`);
   - hardware SHA-256 (ARMv8 SHA2 / x86 SHA-NI) and AES (ARMv8 AES / AES-NI)
     with the current code as fallback; verify of a 30 GB FFPFSC took 105 s,
     mostly SHA-256.
2. **PS5 cross-build of the core** with ps5-payload-sdk (`prospero.mk`,
   clang-18); check std::thread, file I/O and zlib there; then a minimal
   payload that runs a conversion. Ask the owner before running anything on
   the console.
3. ✓ **LZ4 asset packs** done (2026-10-18). Left: owner's console test of
   package D's LZ4 exFAT; TOML profiles (PS5-LZ4-Compression's per-game
   profiles are GPL); smaller pass-1 memory for the PS5.
4. **FPKG builder** (dump/image → debug FPKG): Kraken encoder or "stored";
   ProsperoPkgTool and LibProsperoPKG builders as references; must install on
   a console (owner tests).
5. **UFS2 writer** (.ffpkg), then **PFS images** (.ffpfs).

Smaller ideas noted along the way: an app-side downloader for third-party binaries
(`libSceAmpr.sprx`) pinned by SHA-256, never bundled.

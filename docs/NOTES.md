# Alembic: research notes and plan

Chronological log below; **read "Where we are" and "Next session" first.**

## Where we are (2026-10-22, release 0.1.1)

### What works

| From \ To | folder | exFAT | FFPFSC | FFPKG (UFS2) | FFPFS (PFS) | FPKG |
|---|---|---|---|---|---|---|
| folder | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | `--rebuild` | ✓ (wrap) | ✓ | ✓ | ✓ |
| FFPFSC (exFAT, UFS2 or PFS payload) | ✓ | ✓ | ✓ | ✓ (unwrap UFS2) | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ (wrap) | `--rebuild` | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ (wrap) | ✓ | – | ✓ |
| FPKG (debug) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

- **Byte-identical to the references:** exFAT, FFPFSC and FFPKG to
  PS5PKGTool (AMPR index with `--ampr-scheme ps5pkgtool`, PFSC at
  `--level 9`); FFPFS to MkPFS; FPKG (stored) to ProsperoPkgTool
  `img_create --seed S --no_right_sprx`; LZ4 packs to `ampr_pack.py`.
- **CLI** (`build/alembic`, run with no arguments for the list): `convert`
  (any row to any column, `--lz4`, `--ampr-emu`), `ampr-emu`, `fpkg-build`,
  `ls`, `cat`, `same`, `lz4-info|verify`, `ffpfsc-info|extract|verify`,
  `fpkg-info|dump`, `exfat`, `ampr-index`, `pfsc-encode|decode|info`.
- **PS5 engine** (`alembic.elf`, built by `tools/ps5/build.sh` and in CI):
  a queue of conversions, copies, moves and installs with checkpoints
  (pause, resume, survives a restart); a web page on port 5940 made for
  the controller, opened from a Media-tab shortcut; a Components page that
  downloads third-party files pinned by SHA-256. Inputs can be files shared
  from a PC browser tab or FTP servers; outputs can go to FTP (formats
  written front to back only).
- **Console-tested (owner):** FFPFSC images from dumps and packages run via
  ShadowMount+; games that use APR start once the AMPR emulator is added;
  some LZ4 packs run (Unity with `unity.toml`). FPKG → uncompressed exFAT
  of a ~90 GB package: about 30 min on the console.
- Mac speeds (14-core M-series): package B (2.8 GB) → exFAT 1.7 s, →
  FFPFSC 5.5 s, → FPKG 9.5 s.

### How it is checked

```sh
cmake -S . -B build && cmake --build build && ./build/alembic_tests   # 72 unit tests
tools/oracle/compare.sh             # byte-for-byte checks vs PS5PKGTool, MkPFS (.ffpfs) + ampr_emu builder
tools/pkg-oracle/compare.sh         # FPKG reading/writing vs ProsperoPkgTool/LibProsperoPKG
tools/pkg-oracle/check_samples.sh   # owner's real files in .refs/samples/ (~4 min; skips files over
                                    # ALEMBIC_SAMPLES_MAX_GB GiB, default 20; cleans up after each step)
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

- Writing FPKG: stored packages only (`convert --to fpkg`, `fpkg-build`, the
  engine); no Kraken, fake-signing, DDS generation or keystone generation.
  Not yet installed on a console.
- LZ4 packs: hit and miss on the console (owner, 2026-10-22: "works when it
  works", left as it is). Pass 1 keeps ~200 bytes per chunk in RAM (456 MB
  for 2.2M chunks).
- PFS reading covers unsigned, unencrypted images with 32-bit inodes and
  contiguous files (what MkPFS and Alembic write); signed, encrypted, 64-bit
  inode and fragmented PFS images are refused.
- FPKG: retail and patch (LIH) packages are not opened; NAPS "shuffle"
  layouts (current LibProsperoPKG output) are refused.
- SHA-256 is single-threaded (with SHA-NI / ARMv8 SHA2 where the CPU has
  them); inflate and Kraken decode read ahead on worker threads; AES and
  CRC32C use the CPU's instructions on x86-64 and ARMv8.
- FTP outputs cannot take FFPFSC or PKG (they seek back), and uploads do
  not resume.

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
  current one from its saved length, and rewrites anything after it. FPKG
  output and LZ4 packing followed later the same day (see below).
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
  SHA-256) and the index.
- **Console (owner, 2026-10-19): both games now start.** Package D's
  existing FFPFSC with the emulator added as a backport folder, and package
  E converted to a plain exFAT (no LZ4) with the emulator.

### 2026-10-19: ShadowMount+ picks up new images without uninstalling

- **Owner:** to make ShadowMount+ run a new image of a game, the game had to
  be deleted from the home screen; and an unconverted source stays in use.
- **How SMP chooses** (`sm_scan.c` `handle_existing_directory_candidate`,
  `sm_filesystem.c` `cleanup_mount_links_entry`): one source per title, in
  `/user/app/<TITLE_ID>/mount.lnk`. While that source exists and the title is
  installed and mounted, any other source of the title is set aside ("duplicate
  ignored"). When it is gone, the pre-scan cleanup drops the link and
  unmounts the old stack (the game stays installed) and the scan remounts the
  title from what is left (`mount_and_install(..., is_remount)`). A title
  whose mounts failed `MAX_FAILED_MOUNT_ATTEMPTS` times is skipped until the
  counters are reset. Its scanner skips every name starting with '.', finds
  images by extension only (`.ffpkg .exfat .ffpfs .ffpfsc`) and never scans
  `.pkg` files (so for a package source, the image SMP keeps is an earlier
  conversion).
- **SMP's HTTP API** (`127.0.0.1:10101`, `api_enabled`/`api_bind_address`/
  `api_port` in `/data/shadowmount/config.ini`; POST JSON, `status` 0 = ok):
  `src/engine/smp.cpp` uses `/api/v1/games` (each game's physical `path`)
  and `/api/v1/scan {"reset_attempts": true}`.
- **Engine:** after a conversion to anything but FPKG it asks SMP which source
  the title uses (`smp_source` in the result when that is another existing
  file), and requests a rescan with the counters reset (`smp_rescan`).
  `JobSpec::hide_source` (when the source is kept) renames the source to
  `.name` (`source_hidden`); skipped with a warning when another queued job
  uses it or the name is taken. `POST /api/smp/scan`; `--shadowmount
  HOST:PORT` points a PC engine at a test server. On the console the
  endpoint comes from SMP's config.ini.
- **Page:** Convert → "Hide the source from ShadowMount+" (remembered; not for
  packages, nor with "Delete the source"); Files → ⋯ → "Hide from
  ShadowMount+" / "Show to ShadowMount+ again" on images and game folders
  (rename + rescan); a finished job shows "ShadowMount+ still uses <path>"
  with ⋯ → "Hide <name> from ShadowMount+", and "Show the source again".
- Checked on the Mac with a stand-in SMP server (Python): the source became
  `.src.ffpfsc`, the older image SMP used was reported, the rescan went out
  with `reset_attempts: true`; without SMP, `/api/smp/scan` answers 503.
  66 unit tests, PS5 build clean, i18n 0 problems.
- **Console (owner, 2026-10-19): works**: the new image is picked up without
  deleting the game from the home screen.

### 2026-10-19: FPKG speed, checkpoints for packages and LZ4 packing

- **Crypto primitives** (`crypto.cpp`): SHA3-256 absorbs whole blocks with
  64-bit loads and an unrollable round (333 -> 900 MB/s on the Mac); AES-128
  uses AES-NI (x86-64, the PS5 target enables it: `__AES__`, `__SSE4_2__`,
  `__SHA__`, AVX2) or ARMv8 AES, eight blocks in flight; XTS whitens 32 blocks
  per call; CBC decrypt runs in parallel (415 MB/s -> 12-13 GB/s for XTS);
  `crypto::crc32c` uses SSE4.2 / ARM CRC32. The instruction paths run only
  after a self-test at first use (FIPS-197 C.1 over nine blocks both ways;
  the CRC32C check value), else the table code; a test compares both on
  random data. The x86 paths are in the PS5 build (231 AES/CRC instructions)
  but have not run yet (no Rosetta on the Mac): the self-test guards them.
- **Package writer:** outer blocks are SHA3-hashed and XTS-encrypted in
  batches of 64 on worker threads (`OuterPfsWriter`, three batches in
  flight, written in order); the per-file inner SHA3 and the read-back
  SHA3 run on a helper thread (`crypto::BackgroundSha3`). The thread pool
  moved to `alembic/thread_pool.hpp` (shared with LZ4). Package B (2.8 GB):
  30.8 s -> 9.5 s from its package (Kraken decode is now the main cost),
  7.5 s from its FFPFSC, 6.5 s from an exFAT; byte-identical every time
  (fixed seed). What is left is the read-back SHA3, which cannot be split:
  the header comes first in the package and depends on every data block.
  A test builds an outer PFS with 1 and 4 threads and compares (it caught a
  missing final flush during the work).
- **FPKG checkpoints** (`kFpkgFormat`): every interval of game data and when
  stopped, at a chunk boundary: outer batches flushed, file synced, then
  `<partial>.digests` (outer block digests, appended) and `<partial>.state`
  (inner progress, finished files' SHA3, the current file's SHA3 state, the
  open outer block; CRC in the State), then the State {written, blocks,
  seed, state_size, state_crc}. A run given it replays the layout without
  reading, restores the hashes and the outer writer (`resume`), cuts the
  partial file and continues with the same seed. Test: stopped, damaged,
  continued (older checkpoint refused, its side file replaced), 1 and 3
  threads, byte-identical. Engine: kill -9 at 1.7 GB of package B, restart:
  continued from 1 GiB, same size, `same` passes (only playgo-chunk.dat
  differs, as between any two builds).
- **LZ4 checkpoints** (`kLz4Format`): pass 1 depends only on each chunk's
  compression result {stored size, codec, raw and stored CRC, two dedupe
  hashes}, so those 32 bytes per chunk are appended to `<partial>.lz4`; a
  run replays them through the same code (placement, dedupe, stats) without
  reading or compressing. convert keeps lz4_timestamp/lz4_chunks/lz4_done in
  the State beside the output writer's own; during the output phase the
  writer's checkpoint is used as before (volumes are produced from any
  offset). Test: stopped in pass 1 and in the exFAT phase, continued,
  byte-identical. Engine: package B, scan profile, kill -9 in pass 1 and
  in the exFAT phase: both continued; `lz4-verify` and `same` pass. The
  engine's fingerprint now includes the LZ4 options and the runtime file's
  size and mtime; a checkpoint with only the .lz4 side file counts.
- **Package reading:** `InnerMount` decodes Kraken blocks ahead on worker
  threads once reads move forward block by block (three per worker; the
  compressed bytes are still read on the calling thread, since the
  decrypting reader under it is not thread-safe; a jump drops the window).
  Package B -> exFAT 5.1 s -> 1.7 s (identical image); -> FFPFSC 7.1 s ->
  5.5 s (now bound by zlib on all cores; same payload hash).
- **Unity detection:** Unity's markers (Data/ or Media/ globalgamemanagers,
  global-metadata.dat, data.unity3d) select the Unity profile without
  StreamingAssets too; such a game then packs nothing (with a note), rather
  than the scan profile packing Unity's own data files (what crashed B).

### 2026-10-19: converting a file on a PC without copying it to the console

- **Owner:** the slow part of the ~90 GB package was the USB drive (about
  30 MB/s while converting), and copying a package to USB first is a step of
  its own. The engine should do the work, nothing should run on the PC.
- **Design:** the page opened in a PC browser shares a file it picked; the
  console engine reads it through the page (`src/engine/remote.cpp`). The
  page registers {name, size, last_modified} and gets `/remote/<id>/<name>`
  (id = hash of those, so picking the same file again gives the same path;
  a stopped job continues from its checkpoint). Engine reads ask for 1 MiB
  pieces (the HTTP body limit); the page long-polls `want` (8 s) on three
  loops and posts each piece as a raw body (`file.slice`). A 128 MiB cache;
  read-ahead grows 2/4/8/16 pieces on forward reads and is dropped on a
  jump (writers read file by file, which jumps inside a package). No want
  for 45 s fails the read: "the computer sharing X stopped answering".
- **Core:** `convert::set_file_provider` lets detect/open/run read such
  paths like files on disk (packages and images; not folders).
- **Engine:** shared inputs are allowed as job inputs, inspect and meta; only
  convert jobs, never delete_source/hide_source; the output needs a folder
  on the console. Fingerprints and the meta cache key use the path.
- **Page:** "From this computer…" in Files (not in the console's browser);
  the Convert dialog picks a console location; a bar shows what is shared
  and how much was sent; leaving the page with a job reading it asks first;
  a queue card says when a job reads a file this tab does not share.
- **Checked on the Mac:** the page's own relay code run in node against the
  engine (package B -> exFAT: identical to a direct conversion; 1472 MiB
  sent for 1318 MiB: the package header and eboot.bin are read again by the
  plan's AMPR check, the job's check and the conversion, a fixed cost);
  the page killed mid-job: the job failed after 45 s with the message,
  kept its partial file, and after sharing again Retry continued from the
  checkpoint (identical result). Not tried in a real browser or over Wi-Fi.

### 2026-10-19: FTP servers as inputs and outputs

- **Why:** the same goal as a shared file, without keeping a browser tab
  open on the PC: the engine talks to an FTP server on the PC (or a NAS)
  itself.
- **Engine (`src/engine/ftp.cpp`):** plain FTP (no TLS), passive data
  connections (EPSV, else PASV with the control connection's address),
  binary type, MLSD (else LIST). Servers are kept in `<state>/ftp.json`
  (password included, as text) and added only after a login works. Paths
  are `/ftp/<id>/<path on the server>`; they show up in browse ("/" lists
  each server) and in locations, so the page's Places and the folder picker
  show them with no page change.
- **Inputs:** through `convert::set_file_provider`, like shared files: one
  download thread per open file, REST + RETR from where the reader is,
  16 MiB read ahead, 64 MiB kept; a jump stops the download and starts
  another. Stopping a download drops the data connection and reads the
  transfer's one final reply (426 or 226), sending nothing else first:
  servers answer ABOR or NOOP during a transfer, in either order with that
  reply (first ABOR, then NOOP failed on the console: "passive mode
  refused: Entering extended passive mode", every reply one command
  behind). A connection whose stop or end reply fails is dropped.
- **No traffic without reads:** `set_file_provider` takes a `handles`
  predicate, so `is_provided` (fingerprints, meta keys, plan) recognises
  an FTP path by its name; it used to open the file, and a failed job's
  checkpoint check on the page's 1 s job poll made a new FTP session and a
  1 MiB download every second. An opened file downloads nothing until it
  is read. Server ids include the port unless it is 21. Caveat: an FTP
  input's fingerprint and meta key are the path only (no size or time).
- **Outputs:** `convert::set_output_provider` (core): one STOR stream to
  `<name>.alembic-part`, renamed on commit, deleted when dropped (cancel,
  failure). Missing parent folders are made first. Only formats written
  front to back can go there: folders (file by file), exFAT, FFPKG, FFPFS,
  and unchanged image copies. FFPFSC and PKG seek back, so plan and run
  refuse them ("save it on the console, then copy it there"). Uploads are
  not resumable: a stopped job starts over.
- **Page:** Settings > FTP servers lists them (Remove) and adds one (name,
  address, port, user, password; the engine logs in first). Servers show
  under Drives in the rail and the top-level list (server icon, host and
  port), not under Places; breadcrumbs start at the server's name; the
  folder list, picker and Convert dialog say "On an FTP server" instead
  of the free space. Owner asked for Drives (2026-10-19).
- **Editing a server** (owner, 2026-10-19): Settings > Edit opens the add
  dialog filled in; `PUT /api/ftp/ID` logs in first and keeps the id (so
  queued jobs, remembered folders and the meta cache still point at it);
  an empty password field keeps the saved password. Adding a server whose
  id another (edited) server already holds gives it "-2", "-3", ... Closing
  a dialog whose opener was re-rendered focuses the element with the same
  data-key.
- **Checked on the Mac** against pyftpdlib: FTP exFAT -> local FFPFSC; FTP ->
  FTP exFAT (rebuild), FFPKG, FFPFS and folder, nested folders made; output
  equal to local CLI builds except time stamps (two local builds differ in
  the same bytes); an 800 MB FTP -> FTP FFPKG; cancel mid-upload
  (server throttled to 20 MB/s) leaves no `.alembic-part`; a wrong
  password is refused when adding. A ~1.4 GB package over FTP -> exFAT,
  straight and paused/resumed on a throttled server: identical to a
  direct conversion. Console: the first try failed with the reply bug
  above; not tried against another FTP server.

### 2026-10-19: games crashing with the engine idle

The owner saw games crash some time after starting them with the engine
loaded and idle (page never opened, empty queue). The engine re-registered the
Media shortcut at every start: it took the shortcut as unregistered while
`/user/appmeta/ALEM00001` was missing, and that firmware never writes it. On
that firmware AppInstallTitleDir is missing too, so the fallback,
`sceAppInstUtilAppInstallAll`, re-registered every app in `/user/app` with
ShellCore's auth ID, here while a game was starting. Now the engine leaves the
installer alone at start when `/user/app/ALEM00001/sce_sys` already holds the
same param.json and icon0.png (`launcher_current`), and a failed registration
removes param.json so the next start tries again. Not yet confirmed on the
console; the owner's test is the same session again (payload, then a game).
Also: `read_small_file` stopped near 64 KB, below the 160 KB icon.

### 2026-10-22: FFPFSC read-ahead

- `pfsc::Reader` decodes ahead once two blocks in a row are read: runs of
  8 blocks (512 KiB), two runs per worker, on one pool shared by every
  Reader (a PFS image opens a Reader per compressed file, so no threads per
  Reader). Stored bytes are read on the calling thread, one read per run.
  A jump drops what was queued; a failed read or decode ahead is not an
  error until that block is wanted (same message as without read-ahead).
  `pfsc::decode` (`ffpfsc-extract`, verify) now goes through a Reader.
  Memory: at most 2 MiB per worker (decoded and stored bytes).
- The 13 GB FFPFSC sample on the Mac: → exFAT 37.4 s → 7.8 s, extract
  39.4 s → 7.8 s, both byte-identical to the previous build.
- **Owner on the console:** an 8.5 GB FFPFSC → exFAT (12.1 GB) in about
  1.5 min (~150 MB/s written); → FFPKG similar (~160 MB/s); a paused and
  resumed job's check (`same`) passed in about 1.5 min.
- **Elapsed time across pauses** (owner: the timer restarted at every
  resume): jobs keep `ran_before`, the seconds of earlier runs they continue
  from; a run that pauses or stops with a checkpoint adds its time there
  (one that starts over drops it), and a waiting job reports only that, not
  the time spent paused. Engine on the Mac: 6 s, paused 13 s (stays 6),
  +5 s (11), paused, +4 s (15).
- ThreadSanitizer run of the unit tests: clean, after fixing a race in the
  LZ4 packer's volume reader (it read a block's `done` flag without the
  compressor's lock).

### 2026-10-22: Windows build

- **Toolchain (owner's choice):** llvm-mingw 20261006 (LLVM 23.1.3, UCRT),
  the same clang family as the Mac and PS5 builds. macOS:
  `llvm-mingw-20261006-ucrt-macos-universal.tar.xz` (SHA-256
  `a7a19b11048e89372637d26279cdb7c430ed3cf8c1e224310421312bf986ebf6`) in
  `.refs/`; CI: the `ubuntu-22.04-x86_64` archive (SHA-256
  `5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`).
  `cmake/mingw-w64-x86_64.cmake`; bundled zlib; `-static`, so `alembic.exe`
  and `alembic-engine.exe` import only KERNEL32, SHELL32, WS2_32 and the
  UCRT (part of Windows 10 and later).
- **Files and folders** go through `alembic/io.hpp` on every platform now:
  `stat_path`, `list_directory`, `make_directory`, `remove_file`,
  `remove_directory`, `rename_path` (replaces a file; Errc::unsupported
  across drives, where move falls back to copy), `set_file_times`,
  `disk_space`, `open_stdio`, `safe_file_name`, `join_path`, ...
  `io.cpp` has the POSIX code, `io_win32.cpp` the Win32 one: UTF-8 ↔ UTF-16,
  '/' or '\\', absolute paths (relative ones are made absolute) get `\\?\`
  (no 260-character limit; "." and ".." folded first), files opened with
  every share mode (rename/delete while open, as on POSIX), positional
  ReadFile/WriteFile, read-only files deletable. MinGW's own `open`/`stat`/
  `fopen` take the ANSI code page and `open` defaults to text mode, so
  none of them is used. Program arguments come from `GetCommandLineW`; the
  console is switched to UTF-8.
- **Names from images** are refused on Windows when they hold \\ : * ? " < > |
  or control characters (':' would write an alternate data stream).
- **Sockets:** `src/engine/net.hpp` (Winsock: WSAStartup, closesocket,
  WSAGetLastError, time-outs in milliseconds, SO_EXCLUSIVEADDRUSE instead
  of SO_REUSEADDR, refused connects seen through POLLERR).
- **Engine on Windows:** state in `%LOCALAPPDATA%/Alembic`, default root
  `%USERPROFILE%`, paths `C:/...` (the page's paths work unchanged; a drive
  root keeps its '/'), roots compared without case; downloads through
  Windows' own `curl.exe`. Same defaults as on the Mac otherwise: listens on
  0.0.0.0:5940 without authentication.
- **Checks:** all three builds (macOS, Windows, PS5) without warnings; the
  oracles, samples and LZ4 checks unchanged; `tools/windows/smoke.sh`
  (unit tests; a folder with non-ASCII names and a 300+ character path to
  exFAT, FFPFSC, back to a folder, `same`; the engine: browse, a 403
  outside the root, a queued job) passes on the Mac with stand-ins, and
  runs in CI on `windows-latest`. Not yet run on a Windows PC.
- **Left:** `flush_file_system` does nothing on Windows (checkpoints of
  folder outputs trust the cache); the x86-64 PC builds (Windows, Linux) use
  the table AES/CRC32C code, as the instruction paths are compiled only when
  the target enables them (the PS5 does): runtime CPU detection with
  `target` attributes would speed up packages on PCs.

### 2026-10-22: the engine says where it runs

- Owner asked what the engine shows when it starts. It printed only the
  listen address ("0.0.0.0:5940"). Now, after "listening on", it logs the
  system ("macOS 27.0 (arm64)", "Windows 11 (build 26100, x64)", the Linux
  distribution's PRETTY_NAME; "PS5"), the computer's name, the page's URL
  on this computer and from other devices (each IPv4 address that is not
  loopback or link-local: getifaddrs, GetAdaptersAddresses), the shared
  folders, the state folder, a note that anyone on the network can use it
  (no password; `--listen 127.0.0.1` keeps it local) and how to stop it.
- `/api/info` adds `os`, `host_name`, `local_url`, `network_urls`,
  `open_to_network`; Settings > Engine shows the system, computer, the
  addresses for other devices and the shared folders (texts in all five
  languages).
- Windows: an engine started by double-click that cannot start (a port in
  use) waits for Enter before its window closes.
- The PS5 reports "PS5"; its addresses come from getifaddrs too, untried
  there (the page shows "–" if it finds none).
- Owner: a PC browser still showed "L2 R2 Leave". Leaving is for the
  console's browser, so the hint and the chord are PS5-only now; on a PC
  L2/R2 page at once (no wait for the other trigger).

### 2026-10-22: engines find each other (step 1 of working across engines)

- **Plan (agreed with the owner):** engines see each other on the network;
  then pairing (a code shown on one, typed on the other; a shared key after
  that) before anything reads or writes files; then another engine's files
  as inputs and its folders as outputs (`/peer/<id>/...`, ranged reads and
  writes at any offset, so every format and resuming work); then queuing a
  job on another engine. A job has three parts that can each be any
  engine: the source, the engine that converts, the target (owner).
- **Discovery** (`src/engine/peers.cpp`): with Settings > Other engines >
  "Find engines on this network" on (off by default, kept in
  `<state>/discovery.on`), an engine broadcasts a JSON beacon on UDP 5940
  every 5 s to each interface's broadcast address and 255.255.255.255:
  `{"alembic":"engine","v":1,"id","name","os","platform","version","port"}`;
  the first after switching on carries `"query":true` and the others answer
  at once (a broadcast too: engines sharing the port on one computer each get
  only some unicast datagrams). Heard engines drop off after 20 s of
  silence; the address is where the beacon came from, not what it says.
  Text from beacons is cleaned (no control characters, at most 80 bytes,
  whole UTF-8). An id per engine in `<state>/engine_id`; `/api/info` has
  it.
- **By address:** Settings > "Add an engine by address…" asks the engine's
  `/api/info` (works with discovery off and across networks that drop
  broadcasts); kept in `<state>/peers.json` and asked again every 15 s
  ("Not reachable: ..." when it does not answer).
- **API:** `GET /api/peers`, `POST /api/peers/discovery {enabled}`,
  `POST /api/peers {address, port}`, `DELETE /api/peers/ID`. `--discovery-port`
  for tests. Discovery shares nothing /api/info does not already show.
- **Checked on the Mac:** two engines (ports 6101/6102, one discovery port)
  find each other within a second of switching on; off by default; switching
  off empties the list; added by address; "that is this engine"; an engine
  that is not there; removed. Unit test for beacons. Windows smoke test
  switches it on (UDP port opens). **Not yet tried:** across two computers,
  and on the PS5 (whether a payload may broadcast).

### 2026-10-22: pairing engines (step 2)

- **Flow (owner: "Pair" on one, code on the other):** on engine B the person
  presses Pair next to engine A; A shows a six-digit code; they type it on
  B. A shows the code only where someone at A sees it: a PS5 notification,
  the engine's window, and its page when the browser runs on that computer
  (`/api/pair/incoming` answers 127.0.0.1 only). The page has no password, so
  a code readable over the network would let anyone there pair.
- **Protocol** (`src/engine/pairing.cpp`, written up at the top of
  `pairing.hpp`): X25519 keys and 16-byte nonces; B commits to SHA-256(pubB
  || nB) before A answers, A shows its code before B reveals, so a
  man-in-the-middle cannot search for values that give both sides the same
  code (1 in a million per attempt); code = HMAC(shared, "code" | transcript)
  mod 10^6; K = HMAC(shared, "key" | transcript); each side proves K before
  keeping it. Three wrong codes end the attempt; a session lasts 2 min; one
  pairing at a time and six attempts a minute at A.
- **Signed requests** for what comes next: X-Alembic-Peer/Time/Nonce/Sign,
  HMAC(K, method, path, sorted query, time, nonce, SHA-256(body)), within 2
  minutes of the receiver's clock, each nonce once. `/api/pair/check` (a
  signed ping) and `/api/pair/forget` use them now.
- **Keys** in `<state>/paired.json` (secret); a paired engine is remembered
  like one added by address. Randomness: `platform::secure_random`
  (sceRandomGetRandomNumber on the PS5, weak import, else the module loaded
  at run time, else arc4random_buf; arc4random_buf on macOS, getentropy on
  Linux, BCryptGenRandom on Windows). X25519 after TweetNaCl (public domain),
  checked against RFC 7748; HMAC-SHA256 against RFC 4231.
- **Page:** Pair / Enter the code / Check / Unpair per engine; the code
  dialog; the engine that shows the code opens a dialog with it (local
  browser only) and closes it when done.
- **Checked on the Mac:** two engines: pair (wrong code refused, right one
  accepted), signed checks both ways, kept across a restart, unsigned
  request refused (401), "unpair it first" before removing, unpairing on one
  side unpairs the other. Unit tests: the protocol at A with the test as B
  (a broken commitment, a second engine while one pairs, matching codes and
  proofs), signatures (tampered body, other path, replay). Not yet on the
  PS5 (sceRandom, notifications) or between two computers.
- **Known gap:** the page itself still has no password; pairing protects
  engine-to-engine requests only.

### 2026-10-22: CPU instructions on every x86-64 PC, SHA-256 in hardware

- **Why:** the PC is to do the heavy package work, but PC builds target plain
  x86-64, so AES-NI and CRC32 were compiled only for the PS5 (whose target
  enables them) and Windows/Linux PCs ran the table code.
- **x86-64 now:** the AES, CRC32C and SHA-256 paths are compiled with
  `__attribute__((target(...)))` in every x86-64 build and used when CPUID
  says the CPU has them (`src/core/cpu_features.hpp`, `__get_cpuid`, no
  runtime library), then only after the existing self-tests pass. Builds
  that target them (the PS5) skip the CPUID check.
- **SHA-256:** SHA-NI (x86-64) and the ARMv8 SHA-256 instructions (Apple
  silicon), four rounds a step; self-tested against the portable code on
  five blocks before first use; a unit test compares every length to 200
  bytes and 1 MiB in odd pieces. `Sha256::portable()` is the reference.
- **Mac:** `ffpfsc-verify` of the 13 GB FFPFSC 71 s -> 13.4 s.
- **Shown:** `crypto::accelerations()` ("AES, CRC32C, SHA-256" or "none") in
  the engine's start-up log, `/api/info` (`cpu_instructions`) and Settings >
  Engine; the unit tests print it (CI shows each runner's).
- **Not yet run:** the x86 paths on a real x86-64 machine other than the CI
  runners (the Mac has no Rosetta); the self-tests guard them.

### 2026-10-22: source, converting engine and target on any engine (steps 3 and 4)

- **io mounts** (`io.hpp`): paths under a mounted prefix go to a
  `FileSystem` (File::open, stat, listings with details in one call,
  mkdir, remove, rename, times, free space); `File` can hold a handle to a
  file elsewhere. `tests/test_mounts.cpp` mounts an in-memory one: exFAT,
  FFPFSC, FFPKG and FFPFS written into it equal the same conversions on
  disk; an image and a folder in it work as inputs. So every writer and
  reader already goes through io.hpp.
- **Paired engines at `/peer/<id>/<its path>`** (`peer_fs.cpp`; a Windows
  engine's `C:/x` is `/peer/<id>/C:/x`): each call is a signed request to
  that engine's `/api/peer/fs/{stat,list,read,open,write,truncate,sync,
  mkdir,remove,rmdir,rename,times,space,roots}`, served inside its shared
  folders, never its state folder (pairing keys), never another engine's
  files through it; a shared folder itself cannot be changed. Reads move a
  4 MiB window; writes next to each other go out together (4 MiB requests;
  the HTTP body limit is 5 MiB now). Folders above a shared one answer
  "a folder" to stat only (making an output's folders walks down to it).
  The serving engine keeps up to 16 files open between requests.
- **Jobs** (step 4): `JobSpec.origin` is the engine that sent it. The page's
  engine rewrites paths for the converting engine (its own files become
  `/peer/<its id>/...`, the converter's become plain paths; browser-shared
  and FTP files stay with their engine) and sends the job signed to
  `/api/peer/jobs`; `GET /api/peers/jobs` gathers the jobs this engine sent
  elsewhere; `/api/peers/<id>/jobs/<n>/<action>` pauses, resumes, cancels,
  retries and removes them there. Components (ampr_emu) are the converting
  engine's own.
- **Page:** paired engines' drives under "Other engines" in the rail, at
  the top level and in Save to (`/api/peer-drives`, asked again after 8 s);
  the Convert dialog's "Convert on" (this engine or a paired one,
  remembered); the queue shows jobs on other engines ("on <engine>"), their
  paths as this engine names them, and its buttons act there.
- **Checked on the Mac** with two engines: A's FFPFSC to a folder on B;
  B's folder to FFPFSC and exFAT on A (exFAT equal to a local build, FFPFSC
  verifies, no temporary files left); A's folder as an input; browsing A
  from B; A's state folder out of reach (browse and copy refused); a job
  the page on B sends to A, reading and writing B's files, verifies;
  listed and removed from B. A 1.5 GB FFPFSC read from A: 1.0 s against
  0.6 s locally (loopback). **Not yet:** the page in a browser, two
  computers, the PS5.

### 2026-10-22: faster across engines; packages checked where they are; sending files

- **Overlap** (owner asked whether images are built first, then sent: no,
  they stream): `PeerFile` keeps the next two 4 MiB read windows on their way
  and sends collected writes in the background, two at a time (a write over
  one in flight waits for it; reads, size, truncate, sync and close wait for
  all). With `ALEMBIC_TEST_PEER_DELAY_MS=20` on the serving engine (a test
  hook: a pause before each read and write request): a 1.5 GB FFPFSC read
  11.5 s -> 6.0 s, a 2.8 GB FFPKG written there 22.7 s -> 11.1 s.
- **PKG output on another engine** read the whole package back over the
  network for its digest and CRCs. Now `FileHandle::checksums` (io.hpp) lets
  the engine with the file work them out: `POST /api/peer/fs/checksum`
  starts a task there (SHA3 on a helper thread), the converting engine asks
  how far it is every 0.5 s (job stage "Checking the package (on the other
  engine)"), fetches the CRCs (`checksum_crc`, 4 MiB pieces) and drops the
  task; results nobody fetched go after 10 min. A package written to
  another engine equals one built locally with the same seed (`seed`: 32
  hex digits, a new optional job field for this).
- **Sending without converting** (owner): copy and move jobs already work
  between engines (a move from another engine copies, then deletes there);
  the file menu has "Send to another engine…", the copy picker opened at
  that engine's usual destination. Not resumable yet: a stopped copy starts
  over.

### 2026-10-22: checking outputs on other engines by digests

- Owner asked whether the check before deleting a source pulls files back:
  it did (`convert::compare` reads both sides byte by byte, so an output or
  source on another engine came over the network again). Only that check
  reads outputs back; the writers do not (FPKG's read-back is handled on
  the engine with the package since the entry above).
- Now (`runner.cpp` `check_output`): both sides on the converting engine:
  bytes, as before (hashing would read the same and add work). Either side
  elsewhere: `convert::digest_tree` (every file's size and SHA-256, any
  format, LZ4 packs unpacked, `ampr_emu.index` left out, as compare does)
  for each side where its files are, both at once (a `digest` task on the
  other engine, the background tasks now general: `task`, `task_blob`,
  `task_drop`), then `compare_digests`. Only the lists cross the network
  (text, one line a file; '%', CR and LF in names escaped). The job's
  result says `verify.by`: "bytes" or "digests".
- Checked: a unit test (digests of a folder and its exFAT agree with
  compare; a changed and a missing file; names with newlines); two
  engines with "delete the source": B's game to an FFPFSC on A, A's game to
  an exFAT on B, both checked by digests, sources then deleted.

### 2026-10-19: open items from that day (all solved)

1. ✓ Games converted PKG -> FFPFSC do not start. The conversion finishes and
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
     **Solved (2026-10-19, see the entry above):** conversions add the
     emulator, and an existing game can get it; both games start on the
     console.
   - Side finding: `detect_engine` needs a `StreamingAssets/` folder, so
     E (Unity, no StreamingAssets) gets the scan profile, which packs its
     `level*` / `sharedassets*` files; it still ran on the console.
   - Quick test without rebuilding: `/data/homebrew/backports/<TITLE_ID>/
     fakelib/` with the game's own fakelib files plus `libSceAmpr.sprx`
     (a backport fakelib replaces the game's whole fakelib, it is not
     merged).
2. ✓ Leaving moved off ○ (owner, 2026-10-19): **L2 + R2 together** close
   everything and return to the first screen (one more ○ then leaves the
   browser; a page cannot close it). L2 or R2 alone still pages; a press
   waits 120 ms for the other trigger so a chord never pages first, held
   triggers keep paging every ~120 ms, and while both keep coming the chord
   fires once (checked in node with simulated presses). A held ○ is one
   step back, however long (its stream of Escapes is swallowed). The hint
   bar shows "L2 R2 Leave". Console (owner): works; it lands on the first
   screen and one more ○ leaves, acceptable though not convenient.
3. ✓ Checkpoints for FPKG output and LZ4 packing; FPKG speed; Kraken
   read-ahead for package inputs (2026-10-19 entry). Left: zlib read-ahead
   for FFPFSC inputs, SHA-NI/ARM SHA-256. Owner: the ~90 GB package took
   1.5 h to become an image on the console before this; time it again.

## Next session

**Settled (owner, 2026-10-22):**
- Games crashing with the engine idle: not only caused by the payload;
  closed.
- The 1.5 h for the ~90 GB package is not worth chasing: FPKG → exFAT
  without compression takes about 30 min on the console, fine for now.
- LZ4 packs stay as they are.

**Open:**
1. Owner's console test: install an Alembic-made FPKG.
2. ✓ zlib read-ahead for FFPFSC inputs, hardware SHA-256, CPU instructions
   on every x86-64 PC (2026-10-22).
3. ✓ Windows build (2026-10-22). Owner: try the .exe files on a PC.
4. **Working across engines** (owner): source, converting engine and target
   each any engine on the network. Steps 1-4 (discovery, pairing, peer files
   as inputs and outputs, choosing the converting engine) done 2026-10-22;
   owner's test on the console and a PC next.
5. Smaller: retail/patch packages, NAPS shuffle layouts, Kraken-compressed
   FPKG output, less pass-1 memory for LZ4.

Older plan items (faster reading, PS5 build, LZ4 packs, FPKG, UFS2 and PFS
writers, the downloader for third-party files) are done; see the log above.

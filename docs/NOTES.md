# Alembic: research notes and plan

Written 2026-10-17 after the first planning session. Update it as decisions
change.

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

## Plan

1. **Core library + host CLI** in portable C++20, errors as values.
2. **Dump → FFPFSC** first: exFAT writer, AMPR index, PFSC codec, FFPFSC
   wrapper. Test byte-for-byte against PS5PKGTool/MkPFS with a fixed
   timestamp.
3. **FPKG reading**: crypto (AES-XTS/CBC, SHA-256, SHA3, HMAC from mbedTLS or
   small standalone code), Kraken decoder, package reader, then
   FPKG → exFAT/FFPKG/FFPFSC by streaming.
4. **FFPKG (UFS2)** read and write.
5. **Dump/image → FPKG** builder (Kraken encoder; offer "Stored" because
   encoding on the PS5 CPU will be slow).
6. **PS5 front end** on ps5-homebrew-ui: conversion on a worker thread with
   progress and cancel, after solving storage access.

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

## Next step

Read side: port `ExfatVolume.cs` (exFAT reader) and the tree part of
`FfpfscVolume.cs`, so `alembic` can list and extract files from an FFPFSC or
exFAT image (FFPFSC → dump folder, exFAT → dump folder). Check against the
oracle by round trip: dump → FFPFSC → folder must equal the original tree.
UFS2/FFPKG and nested PFS payloads come after that.

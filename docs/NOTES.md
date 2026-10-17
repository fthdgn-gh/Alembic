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
- CLI: `alembic exfat`, `pfsc-encode`, `pfsc-decode`, `pfsc-info`.
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

## Next step

Port `FfpfscImage.cs` (the PFS wrapper around the PFSC payload): build an
FFPFSC straight from a dump folder by streaming `exfat::ImageBuilder` into
`pfsc::Encoder`, then add inspect/extract/verify. Extend the oracle with
`FfpfscImage` using a fixed `BuildTimestampUnixSeconds` and compare byte for
byte. After that, `ExfatVolume` (reading exFAT) for extract/verify.

# Alembic: research notes and plan

Chronological log below; **read "Where we are" and "Next session" first.**

## Where we are (2026-11-11, release 0.6.4)

### What works

| From \ To | folder | exFAT | FFPFSC | FFPKG (UFS2) | FFPFS (PFS) | FPKG |
|---|---|---|---|---|---|---|
| folder | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | `--rebuild` | ✓ (wrap) | ✓ | ✓ | ✓ |
| FFPFSC (exFAT, UFS2 or PFS payload) | ✓ | ✓ | ✓ | ✓ (unwrap UFS2) | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ (wrap) | `--rebuild` | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ (wrap) | ✓ | – | ✓ |
| FPKG (debug) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| PS4 FPKG (fake) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (PS4) |

- **Byte-identical to the references:** exFAT, FFPFSC and FFPKG to
  PS5PKGTool (AMPR index with `--ampr-scheme ps5pkgtool`, PFSC at
  `--level 9`); FFPFS to MkPFS; FPKG (stored and Kraken) to
  ProsperoPkgTool `img_create --seed S --no_right_sprx [--compression
  kraken]`; LZ4 packs to `ampr_pack.py`. zstd packs to ps5-dump-forge
  (`tools/zstd-oracle`, same archive ID; 2026-11-02).
- **CLI** (`build/alembic`, run with no arguments for the list): `convert`
  (any row to any column, `--lz4`, `--zstd`, `--ampr-emu`), `ampr-emu`,
  `fpkg-build`, `ls`, `cat`, `same`, `lz4-info|verify`, `zstd-info|verify`, `ffpfsc-info|extract|verify`,
  `fpkg-info|dump`, `exfat`, `ampr-index`, `pfsc-encode|decode|info`.
- **PS5 engine** (`alembic.elf`, built by `tools/ps5/build.sh` and in CI):
  a queue of conversions, copies, moves and installs with checkpoints
  (pause, resume, survives a restart); a web page on port 5940 made for
  the controller, opened from a Media-tab shortcut; a Components page that
  downloads third-party files checked by SHA-256 (the AMPR emulators
  follow their projects' latest releases, the key files are pinned). Inputs can be files shared
  from a PC browser tab or network servers (SMB, NFS, WebDAV, FTP); outputs
  can go to them (formats written front to back only).
- **In 0.4.0 (2026-10-31):** Kraken FPKG output (byte-identical to
  ProsperoPkgTool, ~2x faster on the console after the encoder stopped
  allocating per block); PS5 packages get a generated debug licence (no
  lock); Install... on images and folders (convert, install over HTTP,
  delete; tried on the console); SMB 3 encryption when required; FTPS and
  HTTPS WebDAV on mbedTLS with certificates pinned on first use.
- **In 0.4.1 (2026-11-01):** Rename suggests names from a game's title, title
  ID, version and format, made by formats the person edits in Settings →
  Rename formats (`src/engine/name_formats.hpp`: fields, `[titleid]`,
  modifiers, `<…>` optional parts, spaces and letters per format; kept in
  `<state>/name_formats.json`; `/api/names...`). Names too long for
  ShadowMount+ are flagged and get a shortened twin (title cut at a word);
  a typed one asks first; hidden names stay hidden. The length warning
  counts bytes; `shorten_name` no longer cuts inside a UTF-8 character.
  Published 2026-11-01: the first v0.4.1 tag failed CI's Windows
  cross-build (`std::atoi` without `<cstdlib>`; macOS and Linux get it
  indirectly, llvm-mingw does not) before anything was published, so the
  tag was moved to the fix (owner's OK). Build `build-win` before tagging
  when new engine files are added. Catalog PR darkxex/ps5-homebrew-catalog#1026.
- **In 0.4.2 (2026-11-01):** selecting several entries in Files (□ >
  Select…; ✕ ticks, □ acts, ○ ends; `state/selection.ts`, `bulk.tsx`, app
  `bulk.cpp`): Convert (a plan and a job per game), Install, AMPR,
  Hide/Show, Rename with a format (`POST /api/names/apply`), Send, Copy,
  Move, Delete. The page keeps a history entry while selecting (○ is the
  browser's Back); stale ones are stepped over. Not yet tried on the
  console's browser. Published 2026-11-01 (every CI job green); catalog PR
  darkxex/ps5-homebrew-catalog#1028 (#1026, for 0.4.1, merged).
  Console limits measured: payloads run on 5 of 16 logical CPUs (mask
  0xEA00) at priority 700.
- **In 0.5.0 (2026-11-03):** zstd asset packs for quer3q's ampr-emu-ng
  (engine, page, app; levels 1/5/19; pass 2 copies pass 1's frames),
  ampr-emu-ng as a second AMPR emulator, the emulators following their
  latest releases, switching a game's emulator, switching a packed game
  between LZ4 and zstd in Convert, LZ4/ZSTD tags in Files, Hide/Unhide for
  any file. Console: a 160 GB package packed at level 5 by the Mac engine
  (pass 1 ~1.5 GB/s, 96.4 GB exFAT written to the console at 41 MB/s, 41
  min) starts through ShadowMount+ with "update emulators" off (owner).
  Release: the Windows test had failed on main since the Replace option
  (`attach_backport` replaced the emulator while its old file was still
  open; Windows can't rename over an open file), unseen because those
  commits were not pushed until the tag. Fixed (db40e2c); the tag moved
  there with the owner's OK (the owner force-pushed it: the agent's
  auto mode refuses). Published 2026-11-03, every job green. Lesson: push
  main and wait for CI before tagging. Catalog PR from the fork branch
  update-alembic-0.5.0: darkxex/ps5-homebrew-catalog#1084 (opened by the
  owner: auto mode refuses it).
- **In 0.6.4 (2026-11-11):** Update AMPR emulators asks the engine for
  one plan per emulator, not per image (each plan opens the game and
  hashes the emulator files: many images kept the engine busy, the app
  sent them all at once); a run of AMPR emulator jobs gets one
  notification (count, failures apart) once none is queued. Not tried on
  the console (the owner had already updated every game's emulator; to be
  seen at the next emulator release). Main's CI green before the tag;
  published 2026-11-11, every job green; notes cumulative. Catalog PR from
  the fork branch update-alembic-0.6.4: darkxex/ps5-homebrew-catalog#1261
  (opened by the owner: auto mode refuses it).
- **In 0.6.3 (2026-11-10):** Convert step by step (Format with the asset
  packs, Options, Where, Afterwards, Review; ✕ Next, ○ back, □ Review),
  one Asset packs choice instead of four switches, FFPKG the recommended
  default; the README guide (formats, how-tos, tips) in every language.
  See the 2026-11-10 entry "Convert, step by step". Tried on the console
  by the owner before the tag (a conversion through the new dialog). Main's
  CI green before the tag; published 2026-11-10, every job green; notes
  cumulative (0.6.3, 0.6.2, 0.6.1, then 0.6.0's). Catalog PR from the fork
  branch update-alembic-0.6.3: darkxex/ps5-homebrew-catalog#1236 (opened
  by Claude).
- **In 0.6.2 (2026-11-08):** the AMPR emulator only for games that use
  the APR: never added to one that does not (engine refuses; Files and
  Images do not offer it), Files does not offer it on Alembic's own app,
  and the APR check reads eboot.bin's import table instead of searching it
  for the name (same answer for every sample game; Alembic's app no
  longer matches). See "Alembic's own app and the AMPR emulator". Main's
  CI green before the tag; published 2026-11-08, every job green; notes
  cumulative (0.6.2, 0.6.1, then 0.6.0's). Catalog PR from the fork branch
  update-alembic-0.6.2: darkxex/ps5-homebrew-catalog#1209 (opened by the
  owner; #1208 for 0.6.1 merged).
- **In 0.6.1 (2026-11-08):** Alembic's own app is no longer listed in
  Images nor taken for an APR user (its texts name libSceAmpr); adding the
  AMPR emulator to it is refused, and a PS5 engine removes at start what
  0.6.0 added to it (the app then did not start). See "Alembic's own app
  and the AMPR emulator". Main's CI green before the tag; published
  2026-11-08, every job green; notes carry 0.6.0's too (owner). Catalog PR
  from the fork branch update-alembic-0.6.1:
  darkxex/ps5-homebrew-catalog#1208 (opened by the owner; #1207 for 0.6.0
  merged).
- **In 0.6.0 (2026-11-08):** the Images tab, first and the PS5's home:
  every image, game folder and package in ShadowMount+'s folders, the
  packages in the downloads folder (`/data/downloads`; on a PC the system's
  Downloads), and paired engines' images, a section per engine, as a grid
  (default) or a list, with Files' actions except moving and copying to
  folders, plus Show in Files; on a PS5, each game's AMPR emulator with
  updates that keep its kind (every official build named by GitHub's
  SHA-256). Files gets the list/grid switch too. See the 2026-11-08 entry
  "the Images tab". Main's CI green before the tag; published 2026-11-08,
  every job green. Catalog PR from the fork branch update-alembic-0.6.0:
  darkxex/ps5-homebrew-catalog#1207 (opened by the owner: auto mode
  refuses it).
- **In 0.5.4 (2026-11-08):** Simplified Chinese (简体中文) on the page, in
  the app and in notifications; the README in every page language; the
  app imports libSceNet/Ssl/Http, so an app without an engine inside can
  download one (by path, as before, it could not in a game process: found
  with the Probe Bench, a development title never released; the engine's
  `GET /api/probe` diagnostic came with it). Main's CI green before the
  tag; published 2026-11-08, every job green. Catalog PR from the fork
  branch update-alembic-0.5.4: darkxex/ps5-homebrew-catalog#1200 (opened
  by the owner: auto mode refuses it).
- **In 0.5.3 (2026-11-07):** Hungarian (Magyar, by isty1974 from
  issue 4) on the page, in the app and in the engine's notifications; the
  app's queue badges upper-case accented letters. Main's CI green before
  the tag; published 2026-11-07, every job green. Catalog PR from the fork
  branch update-alembic-0.5.3: darkxex/ps5-homebrew-catalog#1174 (opened
  by the owner: auto mode refuses it).
- **In 0.5.2 (2026-11-06):** fake-signing. Unsigned PS5 executables
  (plain ELF) become fake-signed SELFs in every conversion: images and
  folders as ampr-emu-ng's make_fself.py makes them, packages as
  ProsperoPkgTool's img_create does (FPKG no longer refuses them); Convert
  says so beforehand. zstd packs follow ps5-dump-forge v0.5.4 (no
  store-only files; everything but system files packed). Not tried on a
  console: the owner can't, users' reports decide. Main's CI green before
  the tag; published 2026-11-06, every job green. Catalog PR from the fork
  branch update-alembic-0.5.2: darkxex/ps5-homebrew-catalog#1157 (opened by
  the owner: auto mode refuses it).
- **In 0.5.1 (2026-11-03):** languages. The console app translates at
  all (it never had: i18n.js's comment broke its parser), translations
  are one plain JSON file per language (`i18n.<code>.json`, fetched as
  needed), Japanese in the app (IBM Plex Sans JP, line breaks between
  Japanese characters), Russian (three plural forms), English and Türkçe
  first in the list, extended-storage places translated. Owner tried the
  app on the console with the new engine before the release. Main's CI
  green before the tag (the macOS runner queued ~10 min); published
  2026-11-03, every job green. Catalog PR from the fork branch
  update-alembic-0.5.1: darkxex/ps5-homebrew-catalog#1096 (opened by the
  owner: auto mode refuses it).
- **Console app** (`app/`, title `PPSA99594`, work for 0.3.0; see the
  2026-10-25 entry): the engine's page drawn natively with
  ps5-homebrew-ui, every section and dialog, talking to the engine on
  127.0.0.1:5940 and starting the payload it carries when none answers.
  Builds on a Mac and in CI; **not run on a console yet.**
- **Console-tested (owner):** FFPFSC images from dumps and packages run via
  ShadowMount+; games that use APR start once the AMPR emulator is added;
  some LZ4 packs run (Unity with `unity.toml`); the console and a PC pair,
  and convert for each other (2026-10-24). FPKG → uncompressed exFAT
  of a ~90 GB package: about 30 min on the console. Install hands a
  debug package to the system installer; a conversion paused, the engine
  updated, and resumed from its checkpoint (2026-10-24). PS4: an image
  converted from a fake package (package P) starts through ShadowMount+
  1.7beta8, with its "Trophy registration failed"
  (ShadowMount+ #97: it copies only PS5 trophy metadata) (2026-10-26).
  A package converted from an SMB share (Samba on the owner's Mac,
  signing required) with the 0.3.3 build (2026-10-30).
- Mac speeds (14-core M-series): package B (2.8 GB) → exFAT 1.7 s, →
  FFPFSC 5.5 s, → FPKG 9.5 s.

### How it is checked

```sh
cmake -S . -B build && cmake --build build && ./build/alembic_tests   # 88 unit tests
tools/oracle/compare.sh             # byte-for-byte checks vs PS5PKGTool, MkPFS (.ffpfs) + ampr_emu builder
tools/pkg-oracle/compare.sh         # FPKG reading/writing vs ProsperoPkgTool/LibProsperoPKG
tools/pkg-oracle/check_samples.sh   # owner's real files in .refs/samples/ (~4 min; skips files over
                                    # ALEMBIC_SAMPLES_MAX_GB GiB, default 20; cleans up after each step)
tools/lz4-oracle/compare.sh         # LZ4 packs vs ampr_pack.py (needs .refs/venv, Homebrew lz4)
tools/fself-oracle/compare.sh       # fake-signed SELFs vs ampr-emu-ng's make_fself.py (passes)
tools/zstd-oracle/compare.sh        # zstd packs vs ps5-dump-forge (builds it with cargo; passes;
                                    # --no-cargo: Alembic + check_pack.py only, needs Homebrew zstd)
tools/pkg-oracle/write_samples.sh   # fpkg-build vs img_create on every real PS5 package (stored, Kraken)
tools/netfs/samba_check.sh          # the SMB client against Samba: encryption required/not, 3.0-3.1.1
tools/netfs/tls_check.sh            # FTPS and HTTPS WebDAV against local servers (TLS 1.2/1.3, RSA/ECDSA)
app/build.sh                        # the console app: build-app/dist/PPSA99594 (+ .zip)
app/preview.sh [dir] [script]       # PNGs of the app drawn on this computer (Mesa)
```

### Environment (all git-ignored, recreate if missing)

- `.refs/` holds reference checkouts:
  `git clone` https://github.com/pearlxcore/PS5PKGTool,
  https://github.com/pearlxcore/ProsperoPkgTool,
  https://github.com/SvenGDK/LibProsperoPKG (as `LibProsperoPKG-sven`),
  https://github.com/drakmor/ampr_emu, https://github.com/drakmor/ShadowMountPlus,
  https://github.com/theghostonline/PS5-LZ4-Compression,
  https://github.com/rdmrocha/fpkg-cli (no license: read only),
  https://github.com/maxton/LibOrbisPkg (LGPL-3.0, PS4 packages, at
  `6434772`; `tools/ps4-oracle/compare.sh` builds its PkgTool.Core for
  .NET 10 with `-p:TargetFramework=net10.0`; its Util/Keys.cs is what the
  ps4_keys component downloads).
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

- Writing FPKG: stored or Kraken (`--compression kraken`, the page's
  Compression choice). The Kraken encoder is ProsperoPkgTool's (greedy/lazy
  LZ parse, raw or Huffman arrays): a 1.3 GB package came out at 1.6 GB
  (2.7 GB stored), so Sony's encoder still does better. No DDS generation
  or keystone generation. Not yet installed on a console,
  stored or Kraken.
- zstd packs: not tried on a console; not resumable (no pass-1
  checkpoints: a stopped zstd conversion starts over).
- LZ4 packs: hit and miss on the console (owner, 2026-10-22: "works when it
  works", left as it is). Pass 1 keeps ~200 bytes per chunk in RAM (456 MB
  for 2.2M chunks).
- PFS reading covers unsigned, unencrypted images with 32-bit inodes and
  contiguous files (what MkPFS and Alembic write); signed, encrypted, 64-bit
  inode and fragmented PFS images are refused.
- FPKG: retail and patch (LIH) packages are not opened; NAPS "shuffle"
  layouts (current LibProsperoPKG output) are refused.
- PS4 packages: read with the passcode (no fake-RSA unwrap of the image
  key, so a package with an unknown passcode does not open); outer PFS with
  32-bit inodes. Written as LibOrbisPkg writes them: game applications
  (CATEGORY gd) only, stored (no zlib in the PFSC), not resumable; patches
  and DLC neither read-tested nor written. Byte-identical to LibOrbisPkg
  (`tools/ps4-oracle/compare.sh`, 2026-10-26).
- SHA-256 is single-threaded (with SHA-NI / ARMv8 SHA2 where the CPU has
  them); inflate and Kraken decode read ahead on worker threads; AES and
  CRC32C use the CPU's instructions on x86-64 and ARMv8.
- Network server outputs (FTP, SMB, WebDAV) cannot take FFPFSC or PKG
  (they seek back), and uploads do not resume. SMB 3 encryption only when
  the server or share requires it (no "always" option yet); FTPS and HTTPS
  WebDAV with certificates pinned on first use (no CA checking; implicit
  FTPS on port 990 untried); no SFTP; no Kerberos; NFS v3 with AUTH_SYS
  only (no v4, no Kerberos).

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
  Works on a console since 0.2.2 (see 2026-10-24: the installer's path).
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
  (Russian from 2026-11-03)
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

### 2026-10-23: jobs show on the engines whose files they use

- Owner: a job started on the PC's page, converting on the PC, writing to
  the PS5, did not show on the PS5. Engines saw only the jobs they sent.
  Now `/api/peer/jobs` (and its actions) cover every job an engine has a
  part in: it sent it, or its input, output or output folder is under
  `/peer/<that engine>/`. So the source's and the target's queues show it
  ("on <engine>") and can pause, resume or cancel it there.

### 2026-10-22: Places of your own, shared with paired engines

- Owner: add any folder to Places, and paired engines share their Places so
  they can be picked on every engine.
- **Engine** (`src/engine/places.cpp`): `<state>/places.json` (at most 64;
  inside the shared folders, not the state folder, a folder that exists).
  `POST /api/places {path, label, remove}`; a `/peer/<id>/...` folder goes
  to that engine's Places (signed `POST /api/peer/places`), so it shows
  everywhere, that engine too. `/api/locations` lists this engine's Places
  first (`custom`), and each engine's `/api/peer/fs/roots` shares its own
  (never ones on other engines), so paired engines show them as
  "<engine> · <label>" (`peer`, `engine`, `place`, `custom`). The drive
  cache is asked again at once after a change made from here.
- **Page:** a folder's Options: Add to Places / Remove from Places; Places
  in the rail (this engine's) and under Other engines (theirs); Settings >
  Places lists them all with Remove; Save to and the copy picker take them.
- Also: the engine's state-file helpers are one `text_file.cpp` now
  (jobs, peers, pairing, places).
- Checked with two engines (API): A's place shows on B; B adds a folder on
  A (it lands in A's places.json) and removes another; outside the shared
  folders, the state folder and unsigned requests refused.

### 2026-10-23: the web page in Preact and TypeScript

- Owner: clean up the page with a framework. Chosen (owner): Preact +
  TypeScript, built by CMake (so building the engine needs Node 20+ and npm;
  CI sets up Node 24).
- **Build** (`src/engine/web/build.mjs`): esbuild bundles `src/main.tsx` and
  `src/style.css` for `safari12` (destructuring marked as supported: esbuild
  counts it missing before Safari 14.1 over an edge case, and the page has
  used it on the console all along), minified, inlined into `index.html` at
  its `<!-- alembic:... -->` markers; CMake embeds the result as before. It
  runs `npm ci` itself when `package-lock.json` changed since the last
  install (`node_modules/.alembic-lock`). Pinned: preact 10.29.8 (not 11,
  out a month and of unknown old-WebKit support), @preact/signals 2.11.3,
  esbuild 0.28.2, typescript 6.0.3 (type checks only: `npx tsc --noEmit`,
  in CI on Linux). `node build.mjs --serve [--engine URL]`: rebuilds on each
  change, `/api` from a running engine. `i18n.js` is unchanged (the engine
  reads it too); `tools/i18n_keys.py` scans `index.html` and `src/`.
- **Code** (`src/`, map at the top of `main.tsx`): `api/` (requests,
  types, the game-info cache), `state/` (signals: the engine's data, the
  view, path naming, jobs and their speeds), `lib/` (i18n, formats,
  storage, env, kinds), `input/` (focus movement, controller keys and
  gamepad, history: a straight port, DOM-based), `components/` (dialog
  stack and sheet, `ask`/`choose` returning promises, icons and glyphs,
  form rows, crumbs, toasts), `hooks/`, `dialogs/` (Convert, folder picker,
  component downloads), `pages/<section>/` (files, queue, deps, settings),
  `app/` (App, header, hints), `styles/` (the CSS in cascade order). Same
  markup and class names as before, so the CSS moved unchanged.
  `window.__alembic` (`debug.ts`) is for the screenshot tool. tsconfig has
  `noUnusedLocals`/`noUnusedParameters` on.
- **Checked:** two commits. First the old script moved as it was into the
  build: 19 states pixel-identical. Then the rewrite, against the old page
  on the mock (`tools/screenshots/mock.mjs` with `MOCK_FROZEN=1`, and
  `MOCK_PAGE` for another build): 44 states walked with keys (files, menus,
  convert and its options, queue and job menus, components, settings and
  every dialog, Spanish, larger text, phone width) land the focus on the
  same element at every step; the screenshots are pixel-identical except
  a caret position, a scroll position after changing the text size and
  sub-pixel anti-aliasing. Against a real engine: convert, rename, new
  folder, add to Places, delete, discovery, language kept after a reload.
  The bundle has no `?.`, `??` or regex lookbehind; 120 KB (145 KB before).
- **Console (owner, 2026-10-23):** the Preact page works in the PS5
  browser: an image converted, and Check on an already paired engine
  passed. The payload now shows as `alembic.elf` in the process list
  (`platform::set_process_name`; the ELF loader names every payload
  `payload.elf`). Not tried yet: copy/move, pause/resume, package installs,
  pairing from scratch (and its incoming-code dialog), older firmwares.
- **Leaving the browser (owner: L2 + R2 then ○ missed more than it hit):**
  the page jumped back with `history.go(-n)`, which on the console went back
  one step only; it now steps back one entry at a time to the first one
  (owner: reliable now; ○ after the message leaves every time).
  Closing the browser from the engine is not possible safely: with the page
  open, `sceLncUtilGetAppIdOfRunningBigApp` reports no app and the focused
  app (`sceLncUtilGetAppFocusedAppStatus`) is NPXS40087, ShellUI itself
  (kstuff-lite's `shellui_patch.c` and others look for that ID). Do not
  kill it. The PS button still goes home at once (the browser stays open
  in the background).

### 2026-10-24: engine protocol number

- Owner: what if a v0.2.1 engine pairs with a v0.2.0 one? Today they work
  (nothing between engines changed since v0.2.0), but nothing checked: a
  later change could fail with "not found", or worse, an older engine could
  ignore a new job option and report success.
- `peers::kPeerProtocol` (1) numbers how engines talk (pairing, files,
  jobs, places), apart from the app version: same-version-only would break
  pairing over a page fix, and pairing goes both ways. It is in beacons,
  `/api/info` (`protocol`), identities (a missing one is 1: v0.2.0), and an
  `X-Alembic-Protocol` header on every request to another engine (signed
  ones and the pairing handshake). An engine refuses `/api/pair/...` and
  `/api/peer/...` with another number (409, "update Alembic on the older
  one"); the sender checks first and names the engine ("{name} has an
  older Alembic ... update it"); Settings > Other engines shows "Older/Newer
  Alembic" and no Pair button. Rule in CLAUDE.md: raise it with any
  incompatible change to those requests.
- Checked: unit test (beacons with and without it, nonsense, messages, a
  header-less request counts as 1); a running engine refuses protocol 2 on
  peer and pairing paths, lets a header-less request through to the
  signature check, and leaves the page's own pairing poll alone; the page
  with a protocol-2 engine on the mock.

### 2026-10-24: notifications with Alembic's icon

- Notifications showed the console's download arrow (`Predefined` icon).
  As ShadowMount+ does (`src/sm_log.c`: `"icon": {"type": "Url",
  "parameters": {"url": "/user/data/shadowmount/smp_icon.png"}}`), the
  engine writes `assets/icon/notify_icon.png` (`alembic.svg`, the page's
  logo, at 512 × 512 with transparent rounded corners: notifications show
  the PNG as it is, while the home screen rounds the Media tile itself) to
  `<state>/notify_icon.png` at start (only when it differs) and names it as
  `/user/data/alembic/notify_icon.png`, the path the notification service
  sees. If writing fails, the download arrow stays. Owner: the icon shows
  (2026-10-25; first with the square `icon0.png`, then rounded on request).
- The console caches notification icons by path: after the icon changed,
  the first notification still showed the old picture. The file is now
  `notify_icon-<first 8 hex of its SHA-256>.png`, so a new icon has a new
  path; other `notify_icon*.png` files in the state folder are removed.

### 2026-10-24: the engine updates itself (for 0.2.3)

- **What it does** (`src/engine/update.{hpp,cpp}`, Settings → Updates):
  at start the engine asks `api.github.com/repos/fthdgn-gh/Alembic/releases/latest`
  (same Sectigo/USERTrust chain as github.com, already in `github_ca.inc`).
  When newer: download `alembic-vX.Y.Z.elf` and `SHA256SUMS`, check the
  hash and the version marker inside the ELF, save it in
  `<state>/update/`, replace the chosen payload files, send the ELF to the
  ELF loader (`127.0.0.1:9021`), then quit. Owner decisions: pause/resume a
  running job automatically (quitting keeps a job with a checkpoint queued,
  so the new engine continues it), check once at start, signing releases
  later.
- **Version marker:** `ALEMBIC_PAYLOAD_VERSION:X.Y.Z` (main.cpp, read back for
  `/api/info` so the linker keeps it). Releases 0.1.0–0.2.2 have none; they
  are recognised by SHA-256 (table in update.cpp), and builds from source by
  "Alembic engine " + "Alembic is ready". The bare prefix is in every
  engine too (the code that looks for it): only a prefix followed by a digit
  counts.
- **Where the payload lives:** a running payload has no file (the loader
  gets the bytes over a socket). Searched: Payload Manager
  (`/data/pldmgr`, `/mnt/usbN/pldmgr`: `payloads/{local,repo}/<slot>/<file>`
  with a `meta.json`; it checks a checksum only when downloading from a
  repository, never at launch; its `autoload.txt` names slots, not files),
  the autoloaders' `ps5_autoloader` folders (`autoload.txt` names files),
  `/data/payloads`; "Search the drives" walks every root (6 levels, `.elf`
  only: games are full of `.bin`). Files already at the new version are not
  listed.
- **Renaming (owner):** where the folder's `meta.json` names the file, the
  file gets the new version in its name and every occurrence of the old
  name in `meta.json` is replaced as plain text (its schema is Payload
  Manager's and may change). Elsewhere only the contents change.
- **Taking over (main.cpp `take_over_port`):** before starting its queue, a
  new engine asks `/api/info` on its port; an older or equal Alembic gets
  `/api/quit` and is waited for (up to 90 s); a newer one is left running and
  this one exits. Always on the console, `--take-over` on a PC. So sending a
  newer ELF by hand also works. Like Payload Manager, which kills an older
  `pldmgr.elf` by process name at start and hands self-updates to the loader
  (DEVELOPMENT.md "Payload Manager self-updates"; ideas only, no code taken).
- **API:** `GET /api/update`, `POST /api/update/check`, `/find {everywhere}`,
  `/install {paths}` (only paths the last search found). `/api/info` has
  `update: {state, latest, newer}`; the page says once per version that one
  is out. Page-only: the engine protocol number stays.
- **Test hooks:** `--loader HOST:PORT`, `--update-url URL` (the PC downloader
  accepts `http://127.0.0.1:`/`localhost:` for this), `--no-update-check`,
  `-DALEMBIC_ENGINE_VERSION=X` (an engine that says it is newer).
- **Checked on the Mac** (fake release server, a fake loader that starts a
  Mac engine built as 0.2.3, folders laid out like a Payload Manager slot
  with the real 0.2.2 release ELF and an autoloader folder): the slot's file
  renamed and replaced and its `meta.json` fixed, the autoloader's replaced
  under its name, an unrelated ELF untouched; a running conversion paused at
  its checkpoint and continued in the new engine; the page reloaded on
  0.2.3 about 4 s after Update. Also: an older engine started next to a
  newer one stops; a second engine of the same version takes over; with no
  loader the engine reports it and keeps running. 82 unit tests.
- **Console (owner, 2026-10-24):** works. A 0.2.3 build that said it was
  0.2.2 (`-DALEMBIC_ENGINE_VERSION=0.2.2`), uploaded to Payload Manager as
  `alembic-v0.2.2.elf`, found the published 0.2.3, replaced and renamed its
  slot's file and handed over through the ELF loader. The first release that
  can update itself is 0.2.3: going from 0.2.2 to 0.2.3 is still by hand.

### 2026-10-24: the installer's path, and a dialog that waits visibly (0.2.2)

- **Install** failed on the console: the system installer refused a
  package in `/data/packages` with 0x80b2150f. It runs in another process
  that sees the internal drive as `/user/data`; ps5-web-file-manager's
  `src/pkg_installer.c` passes `/user` + path, and the package's content ID
  and title. Doing the same, the package installs (owner, 2026-10-24).
- **Convert dialog on a slow engine:** the output name and Add to queue
  both waited on `/api/plan` with nothing on screen. On the console the
  first `/api/inspect` of a 68 GB package took about 3 minutes, later
  3.3 s every time (cause not found: not a full scan of the file, which
  stays fast with the cache churned). The footer now counts the wait
  (from 2 s; a warning from 60 s), the button waits for the plan of the
  current choices, and Add to queue reuses it.
- **Convert on** listed an offline paired engine; it now lists reachable
  ones only.
- **Owner:** paused a conversion, updated the engine, resumed: it carried
  on from its checkpoint (`ran_before` kept).
- **Verifying** an output runs only with "Delete the source"; a separate
  "check against the source" action is an idea, not planned.

### 2026-10-23: README screenshots, redone

- The README was from before engines could pair. It now covers working
  across engines (discovery, pairing, Convert on, jobs on every engine
  involved), Places, CPU instructions and `alembic-engine --root`, with four
  screenshots in `docs/screenshots/` (Files, Convert, Queue, Settings >
  Other engines) instead of the one `docs/screenshot.png`.
- `tools/screenshots/shoot.sh [name]` remakes them: it asks a throwaway
  engine for `/api/info` and `/api/deps`, serves the real page from
  `tools/screenshots/mock.mjs` (made-up games ALEM00011-19 with SVG icons,
  a paired "Living room PC", an FTP "NAS", jobs that move with the clock),
  and drives a headless Chromium browser over DevTools with a PlayStation
  user agent at 1600x900, 2x (`shoot.mjs`). Chrome by default,
  `BROWSER=` for another (the Mac here has Brave). When the page's API
  changes, the mock may need the same change.

### 2026-10-25: the console app (for 0.3.0)

- **Owner decision:** the native app comes back (it was shelved on
  2026-10-18): the same UI/UX as the page, every section and dialog in
  0.3.0 ("Everything"), Latin languages first (Japanese shows English until
  the console's fonts are used), and the Media-tab shortcut stays.
- **ps5-homebrew-ui is gone from GitHub** (the whole blackbearreloaded
  account, with ps5-opengl and ps5-native-app-boilerplate). The kit is
  GPL, so it is vendored in `third_party/ps5-homebrew-ui` from our
  `.refs` copy at `bbe1905`, the last upstream commit (mirrors with history
  end there too). The ps5-opengl SDK 1.0.2 comes from two copies of the
  release (JMUtechnologies, darkxex) that serve exactly the SHA-256 the kit
  pins (`30d8e51d…`); drakmor still hosts ps5-native-app-boilerplate.
  Local changes to the kit's scripts are listed in its `UPSTREAM.md`.
- **Build on a Mac** (`app/build.sh`): Homebrew `llvm@18`, `ninja`, `bash`,
  `coreutils`, `findutils`; clang's x86-64 Linux builtins come from Ubuntu
  24.04's `libclang-rt-18-dev_18.1.3-1_amd64.deb` (SHA-256 in
  `app/tools/host_env.sh`, the noble index's), since Homebrew's LLVM has
  none. The kit's clean-room `libc.prx` rebuilds byte-identical to its pin
  (`e6ff45d1…`). The payload SDK for the app is v0.42 (the kit's validated
  one), the engine keeps v0.44. Output: `build-app/dist/PPSA99594/` and a
  ZIP whose entries are 0777 (the console refuses other modes:
  CE-107750-0). `ALEMBIC_ENGINE_ELF` (default `build-ps5/alembic.elf`) goes
  into `assets/alembic.elf`. CI job "PS5 app" builds it with the payload
  of the PS5 job; releases get `alembic-app-<version>.zip` (an `.ffpkg`
  from 2026-10-25, see "restarting the engine").
- **Preview on a Mac** (`app/preview.sh`): Homebrew's `mesa` has
  surfaceless EGL with llvmpipe (OpenGL 4.6), so the kit's PC renderer
  works here too. The preview runs the app against the README's stand-in
  engine (`tools/screenshots/mock.mjs`, frozen jobs) and writes PNGs from a
  script of button presses; the pictures match the page's screenshots.
  The stand-in engine's icons are SVG, which the app cannot draw, so
  previews show format labels where the console shows game icons.
- **How the app is built** (`app/src`): `ui/ui.*` is an immediate-mode
  layer over the kit's draw list: items register with their rectangles,
  the D-pad moves the focus as the page's `focus.ts` does (columns,
  remembered items, scroll areas that follow the focus), each dialog is a
  layer of its own. `ui/draw.*` draws the page's CSS pieces at the page's
  sizes (one rem is 19.2 canvas units: the page is laid out for the PS5
  browser's 1600 px). `app/*` follows the page file by file (state,
  Files, Queue, Components, Settings, Convert, the folder picker, the
  menus, forms whose fields open the kit's on-screen keyboard). Requests
  run on two worker threads (`engine/client.*`, quick calls and polling
  apart) with a minimal HTTP client using only socket/connect/poll/send/
  recv (`engine/http.*`); icons are decoded and shrunk to 128 px on the
  worker (stb_image). Texts come from the engine's `i18n.js`; the language
  is the engine's (`/api/lang`), shared with the page and notifications.
- The app's imports resolve to the same modules as the kit's demo app (no
  new system library). A native title has no `localtime_r`: dates show in
  UTC for now.
- **On the console (owner, 2026-10-25):** the app starts, talks to the
  engine and ran a conversion on a paired PC engine with the output on
  the console. Not yet confirmed: sending the payload to the ELF loader
  on 9021, controller feel, frame rate, sounds.

### 2026-10-25: restarting the engine, and the app's version

- **Owner (console):** the app stopped the engine; reopened, it started it
  again through the ELF loader. Asked for a restart button, the app as an
  `.ffpkg`, and app updates; decided on one version for engine and app.
- **`POST /api/restart`** (`Updater::restart`, PS5 only): the console
  started the engine from a socket, so it looks for a copy of its own
  version (its own update download in `<state>/update/` first, then the
  Payload Manager and autoloader folders the updater searches; checked by
  the `ALEMBIC_PAYLOAD_VERSION:` marker), hands it to the ELF loader and
  stops; the new engine takes over the port. 409 with "no copy of this
  engine's payload ..." when none is found. `/api/info` gains `can_restart`
  and `started` (ms): the new engine can take the port between two polls,
  so the page and the app watch `started` change rather than wait for a gap.
- **Page and app:** Settings → Engine → "Restart the engine" (question
  with the running-job notes the update uses). The app falls back, when the
  engine can't restart itself (none found, or an engine before 0.3.0), to
  stopping it and sending its own payload, but only when the app's version
  is not older than the engine's. Once the engine is gone, the app's engine
  card shows "Start the engine" (its own payload) instead.
- **One version:** `app/build.sh` takes Alembic's version from
  CMakeLists.txt (or `ALEMBIC_APP_VERSION`, for update tests), writes it into
  the copied `param.json` (`contentVersion` `NN.NNN.NNN`) and into
  `app_version.h`; Settings shows it as "App version".
- Checked on the Mac: a PC engine with `--loader` pointing at a stand-in
  loader that starts a new engine; the app preview restarts it ("Restarting
  the engine…", then "Engine restarted"). Not checked: the page's button in
  a browser, "Start the engine" (the preview has no loader), the console.
- **The app as an image:** `app/build.sh` also writes `<TITLE_ID>.ffpkg`
  with Alembic's own UFS2 writer (`ALEMBIC_CLI`, default `build/alembic`;
  time stamp from the commit, `--ufs2-pid 1`, so the same commit gives the
  same bytes; `alembic same` finds the folder's 55 files unchanged). Files
  are 0555 inside, as in every game image, so the 0777 rule of folders
  (CE-107750-0) no longer matters. CI's app job takes the Linux CLI from the
  PC job; releases carry `alembic-app-<version>.ffpkg` instead of the ZIP
  (the local build still makes the folder and its ZIP for testing). The
  converter's "uses the console's APR" warning is filtered there: the APR
  check is a text search and the app's Components page names libSceAmpr.
  Not yet tried on a console: the app started from the image.

### 2026-10-25: the app's image in the update

- **Engine** (`update.cpp`): a release's `alembic-app-*.ffpkg` asset is the
  app. On a console the check also finds the installed app: ShadowMount+'s
  source for PPSA99594, else its scan folders (`/data/homebrew`,
  `/mnt/usbN/homebrew`; on a PC with `--loader`, `<state>/homebrew`),
  `.ffpkg` files and folders whose param.json has that title ID, names with
  "alembic" first; it hashes the image and compares with SHA256SUMS
  (`/api/update` `app`: path, image, version, in_release, current).
  `install {paths, app}` (app defaults to true) and `POST /api/update/app`
  (only when the latest release is this engine's version) download and check
  the image and stage it as `.alembic-app-<v>.ffpkg.new` next to the old one,
  noted in `<state>/update/app-pending.json`. A watcher (every 3 s, consoles
  only) swaps once the app has been quiet for 15 s: the staged file becomes
  `alembic-app-<v>.ffpkg`, the old image is deleted (ShadowMount+ then drops
  its link and mounts the new one) and a rescan is requested. The app is
  told by its User-Agent (`Alembic-app/<version>`); quiet counts from the
  engine's start, so a new engine waits for an open app too, and
  `POST /api/app/closing` (sent when the app quits) makes it 3 s.
  Folders (development installs) are never replaced.
- **Page and app:** Updates shows "Console app" (version, up to date or
  not, a folder, not found) and its path; "Update the app" when the release
  is the engine's version and the image differs; a switch for the app in the
  list of files an update replaces; a note while a new image waits. The app
  says when the engine is newer than itself (once per engine version).
- Checked on the Mac (stand-in GitHub on 127.0.0.1, `--update-url`,
  `--loader`): "Update the app" from the app preview staged the image while
  the preview ran and swapped it 17 s after it closed; a full update staged
  it, handed off, and a new engine finished the swap 17 s after it started.
  Unit test `update_replaces_the_console_app_image`. Not checked: the page
  in a browser, anything on a console (ShadowMount+ switching the app's
  image while installed, closing the app from the home screen).

### 2026-10-25: the app's home-screen background

- `sce_sys/pic0.dds` (behind the selected tile) and `pic1.dds` (while it
  starts), the same picture: `app/tools/background.cpp` draws it from the
  icon's shapes (a large glass retort with its liquid and drop on the right,
  on the app's navy with blue and violet light; the left stays calm for the
  title and Play button) and encodes BC7 mode 6 itself (principal-axis
  endpoints, a least-squares pass, p-bits 1 so alpha is 255), into the
  single 3840×2160 BC7_UNORM DX10 DDS the kit validates. No image tools and
  no committed binaries: `app/build.sh` compiles it with the host compiler
  and the kit's `validate-assets.sh` checks the result. Checked by decoding
  a crop with an independent Python decoder. The `.ffpkg` grows to ~65 MB.
  Not yet seen on a TV.

### 2026-10-25: paired engines that go offline

- **Owner:** with a paired engine switched off, the app's Files showed its
  drive as a raw `/peer/<id>/...` path. Also (owner): the app and the page
  stay on par both ways (CLAUDE.md).
- **Engine:** `/api/peers` keeps every paired engine, with `offline: true`
  when it is no longer announced (then without address) or did not answer
  the last drive request (`peer_down_`, from `peer_drives()`). A request to
  a paired engine that is gone says "<name> is offline"; a failed listing of
  another engine's folder passes that engine's reason on (502) instead of
  "cannot open <path>".
- **App and page:** Files lists an offline paired engine under Other
  engines, dimmed, with "offline" (choosing it says so); the drive list asks
  again when paired engines' drives change, and a row on an offline engine
  reads "<name> · <drive>" / "offline"; inside its folders the line under the
  path says "<name> is offline" and the path starts with its name; a failed
  listing there goes back to Drives. Settings shows "Paired · offline". The
  app now names other engines' paths inside messages as the page does
  (`set_path_namer`).
- Checked with two PC engines paired on the Mac and the app preview: the
  engine stopped while one of its folders was open, Back went to Drives, its
  drive came back on its own when it restarted. The page was type-checked,
  not looked at in a browser.

### 2026-10-25: the app's folder ZIP back in releases

- **Owner:** releases carry the ZIP again, next to the `.ffpkg`, for a
  homebrew store that takes no images. CI uploads both; the bundle job zips
  the downloaded folder again with every entry stored 0777 (as before the
  `.ffpkg`) and lists both in SHA256SUMS. The updater only looks at the
  `.ffpkg` (`alembic-app-*.ffpkg`), so the ZIP changes nothing there.

### 2026-10-25: the app downloads its engine when it carries none

- **Owner:** the store does not take the engine payload as a separate file,
  so the app must get it itself when it is not there.
- **App** (`main.cpp` `engine_payload`): the engine it starts is its own
  `assets/alembic.elf`, else the copy it downloaded before
  (`/download0/alembic/alembic.elf`, used only when its
  `ALEMBIC_PAYLOAD_VERSION:` marker is the app's version), else the GitHub
  release of the app's own version (`releases/tags/v<version>`, one version
  for both): the `.elf` asset, checked against the release's SHA256SUMS and
  its marker, then kept. The connecting screen says what it is doing
  ("Downloading Alembic X from GitHub…", "Checking the download…", "Sending
  the engine to the ELF loader…"). Starting runs on a worker thread now
  (`App::launch`), also for Settings' Start the engine and the restart
  fallback.
- **HTTPS shared** (`src/engine/https_ps5.cpp`, from the engine's
  `platform_ps5.cpp`): the payload keeps its weakly linked libSceHttp/Ssl/Net
  (proven on the console); the app (`ALEMBIC_HTTPS_DYNAMIC`) imports none of
  them and loads them with `sceKernelLoadStartModule` + `sceKernelDlsym` on
  first use (also the payload's fallback when its loader left them empty),
  so a title that cannot load them still starts. The app's imports are the
  same nine modules as before. `app/build.sh` adds `https_ps5.cpp` and
  `src/core/sha256.cpp`; `ALEMBIC_ENGINE_ELF=none` builds an app without a
  payload. `tools/i18n_keys.py` now reads `app/src/main.cpp` too.
- Checked: both build cleanly; the steps run against the real v0.2.3 release
  with curl (asset names, SHA256SUMS line, marker 0.2.3). **Not checked:**
  loading libSceHttp from a native title, and the download itself, on a
  console.

### 2026-10-25: darkxex/ps5-homebrew-catalog (the store)

- **Its rules** (docs/submitting.md, metadata.md, versioning.md,
  artifact-formats.md, review-policy.md), against Alembic:
  - native app with its own title ID: PPSA99594, free in the catalog (138
    records checked), so is the name "Alembic"; never change the title ID;
  - public GitHub repo with a license GitHub detects: `GPL-3.0` (the record's
    `license` must be exactly that);
  - a published (not draft) release tagged with the version; the record's
    `version` is the tag without its `v`;
  - the artifact is a `.zip` of the `PPSA99594/` folder at its top: our
    `alembic-app-<v>.zip` (only the version changes in its name between
    releases, as they ask); `.ffpkg` may sit beside it, the catalog ignores
    it; it never opens or runs the ZIP (only GitHub's SHA-256 of it), so the
    engine payload inside the app is fine; never replace a listed asset;
  - `contentVersion` raised every release, `NN.NNN.NNN`, and the committed
    `app/sce_sys/param.json` at the tag must hold the shipped value:
    `app/build.sh` now stops when it differs from CMakeLists.txt's version
    (raise both in a version bump); set to `00.002.003` for now;
  - icon: square PNG at a URL pinned to the tag (`app/sce_sys/icon0.png`,
    512×512), next to that param.json, where the catalog looks first;
  - release notes: what changed first, under short headings with bullet
    lists (Markdown headings; images, links and HTML are dropped);
  - GitHub immutable releases are recommended (a repository setting).
- **Submitting** after the first release with the ZIP:
  `tools/catalog_record.py v0.3.0 > PPSA99594.json` writes the record
  (`kind` app, `category` utilities, GitHub's digest, icon pinned to the
  tag; author from git's user.name unless `--author`); add it as
  `apps/PPSA99594.json` in a fork of the catalog, changing nothing else, and
  open the PR from the fthdgn-gh account (the repo's owner). Later releases:
  the catalog's daily job proposes them.

### 2026-10-25: catalog PR and build attestations

- Catalog PR: darkxex/ps5-homebrew-catalog#740 (apps/PPSA99594.json, from the
  fthdgn-gh fork, branch add-alembic). Checks pass; the release scan flags,
  as expected, that the app carries a payload not yet on their approved list
  (`helpers/approved.json`, by SHA-256: a maintainer reads its source) and
  that eboot.bin names 127.0.0.1 and port 9021. Every release's payload is
  a new hash, so each update is flagged until approved.
- CI's bundle job now attests every release file
  (`actions/attest-build-provenance@v4`; `id-token` and `attestations`
  write): `gh attestation verify FILE --repo fthdgn-gh/Alembic`. The catalog
  scan reports "attested" for the app ZIP from the next release on (v0.3.0
  has none).
- 0.3.1 released (attestations only); PR #740 moved to it (record from
  `tools/catalog_record.py v0.3.1`, description with the new payload hash
  `c3c95466…`); checks pass, the attestation note is gone, the payload
  still waits for a maintainer's approval. `gh pr edit` fails on GitHub's
  classic Projects deprecation: edit PR text with
  `gh api -X PATCH repos/<repo>/pulls/<n> -F body=@file`.

### 2026-10-26: PS4 packages (read side)

Owner supplied a PS4 fake package ("package P", ~150 MB) in `.refs/samples/`.
It now opens like a PS5 one: `convert` to folder, exFAT, FFPKG, FFPFS and
FFPFSC, `ls`, `fpkg-info`, `fpkg-dump`; the engine shows its title and icon.

- **Layout:** a bare CNT (the same header and entry table as PS5's, so
  `read_cnt` needed nothing). CNT 0x400 u32 PFS image count, 0x410/0x418
  BE u64 PFS offset/size, 0x438 BE u32 signed size, 0x440 SHA-256 of the
  whole stored PFS, 0x460 SHA-256 of its first signed-size bytes (both
  verified on package P). `Kind::ps4_package` = CNT at 0 whose header
  points at a PFS superblock.
- **Crypto:** PS5 with SHA-256 for SHA3 everywhere (entry keys, entry
  digests, `compute_keys_ps4`); EKPFS = keys(1) from the passcode; PFS
  tweak/data keys = HMAC-SHA256(EKPFS, LE32(1) || seed), one step (PS5
  adds an inner HMAC over the seed); the superblock ICV at 0x380 is
  HMAC-SHA256(HMAC-SHA256(EKPFS, LE32(2) || seed), superblock[0:0x5A0]
  with the ICV zeroed), so a match proves the passcode. XTS per 4 KiB
  sector numbered by offset / 0x1000 (block 0, the superblock, is plain).
- **Outer PFS:** mode 0xD (signed, encrypted, case-insensitive), superblock
  at block 0, the same 0x2C8 signed inodes as PS5; uroot holds one
  `pfs_image.dat`: PFSC with version field **2** (PS5 tools write 6; the
  reader takes both). Inside, an unsigned PFS (mode 0x8, version 1) that
  `pfs::open_image` reads as is.
- **Tree:** the inner uroot plus CNT entries under `sce_sys` (the PS5 rule:
  param.sfo, icons, DDS, trophy, changeinfo, PlayGo files, licence files
  decrypted). The inner `sce_sys` has `keystone` and `about/right.sprx`.
- **A PS4 game** = `sce_sys/param.sfo` and no `param.json`
  (`ffpfsc::is_ps4_game`), so it holds after a conversion too. Such games
  are game roots, take their title ID from param.sfo (new `alembic/sfo.hpp`),
  get no AMPR emulator (also refused by `ampr-emu`), and refuse `--lz4` and
  FPKG output (Alembic writes PS5 packages).
- **Checks:** `tests/test_ps4.cpp` builds a small synthetic PS4 package
  (inner PFS from our writer, PFSC v2, signed + encrypted outer PFS, CNT)
  and reads it end to end; key vectors from Python's hashlib/hmac.
  `check_samples.sh` checks PS4 packages by their own digests and
  signature plus PKG -> exFAT/FFPFSC -> folder. Package P: all digests and
  the ICV verify, every zlib block inflates, round trips equal.
- **Not done:** building maxton/LibOrbisPkg's `PkgTool.Core` as a
  third-party oracle (`pkg_extract`, and `pkg_build` for synthetic PS4
  packages) needs the owner's go-ahead to run that external code. The
  fake-RSA image-key unwrap (packages with another passcode) would need
  LibOrbisPkg's published fake key: establish its origin first (CLAUDE.md).
  Not tried on a console: does ShadowMount+ start a PS4 game from these
  images?

### 2026-10-26: PS4 packages (write side)

Owner: "we'll need to build that PS4 package back from the image file"
(ShadowMount+ beta8 mounts PS4 images). **Owner decisions:** the keys come
from LibOrbisPkg's `Util/Keys.cs`, downloaded by the engine (component
`ps4_keys`, `643477263b26`, SHA-256 `48b44dd0…`, never committed; the CLI
takes `--ps4-keys FILE`); a licence is **kept** when the game has one and
**generated** when missing (that needs the debug RIF key, which is in the
same file).

- **Port of LibOrbisPkg** `pkg_makegp4` + `pkg_build` (`fpkg_ps4_*.cpp`):
  the tree is filtered as makegp4 does (license.*, psreserved.dat and the
  PlayGo files are made again; param.sfo's PUBTOOLINFO/PUBTOOLVER rewritten),
  then PkgBuilder's layout: CNT body at 0x2000 (entry keys, image key, general
  digests, metas at 0x2A80, digests, names, PlayGo chunk.dat/sha/manifest,
  licence, param.sfo, psreserved, then sce_sys files with an entry name,
  directly in sce_sys only: trophy/ and changeinfo/ go into the inner PFS, as
  LibOrbisPkg does), outer PFS signed + AES-XTS with a zero seed and an
  unencrypted empty block after the flat path table, inner PFS unsigned
  (min 0x55 blocks), pfs_image.dat a **stored** PFSC version 6.
- **.NET string order matters:** LibOrbisPkg sorts paths, entry names and
  SFO keys with culture-aware `CompareTo` (ICU): `_ - , ; : ! ? . ' "` …
  before digits before letters, case second, lowercase first. Checked with
  a scratch .NET 10 program; `unicode::dotnet_compare`.
- **RSA:** `rsa.cpp` now holds the MT19937-padded wrap for any modulus size
  (PS5 3072, PS4 2048; PS5 output checked unchanged byte for byte) and
  PKCS#1 v1.5 SHA-256 signing for the generated licence.
- **Streaming:** the inner image is a virtual file (game files read on
  demand); the outer PFS's data blocks are signed and encrypted on worker
  threads on the way out; metadata blocks are signed last in LibOrbisPkg's
  order; one read-back pass for the PFS digests and PlayGo chunk digests.
  Package P (150 MB) builds in ~0.5 s.
- **Time stamps:** LibOrbisPkg stamps every PFS inode with the project
  time. Default here: the param.sfo creation date (PUBTOOLINFO c_date and
  c_time, UTC), so package -> package and package -> exFAT/FFPFSC/FFPKG/
  FFPFS -> package give the **same bytes** (image writers drop file times).
- **Reader fixes found on the way:** an empty file with no blocks (as
  LibOrbisPkg writes it) no longer fails the PFS reader.
- **Checks:** unit tests write a synthetic game with made-up keys and read
  it back (digests, ICV, every file, licence, keystone, determinism, a
  rebuild from the package); check_samples.sh rebuilds package P;
  `tools/ps4-oracle/compare.sh` compares with LibOrbisPkg byte for byte and
  runs its `pkg_validate` (the owner runs it: the session's safety check
  refused to build LibOrbisPkg; results in the next entry).

### 2026-10-26: PS4 images need a param.json (console test)

Owner's console test (ShadowMount+ 1.7beta8): Alembic's image of package P
mounted and installed, but "the game or app couldn't be started"
(NP-103270-3, undocumented); ShadowMount+'s log showed the launch released
within a second, no game process. SadykovIV/PS4pkg_to_ffpfsc (GPL-3.0,
shadPS4's extractor + MkPFS; built locally by the owner from
`.refs/PS4pkg_to_ffpfsc`, see its scripts) made an image that **starts**.
`alembic same` showed 59 of 67 files equal; it adds `sce_sys/param.json`
(and the package's internal tables as files, and a `license.info` that
shadPS4 decrypts with the wrong key). Ours plus only their param.json
**starts** too: the param.json is what was missing.
- Alembic now adds it (`sfo::ps4_param_json`, ported from their
  `build_param_json`: TITLE_NN per locale, en-US fallback, titleId,
  titleName, non-zero USER_DEFINED_PARAM_N; byte-identical to theirs for
  package P) to every image and folder of a PS4 game, never to a package.
- A PS4 game is now "param.sfo, and no param.json or one without
  contentId" (`ffpfsc::is_ps4_game(source, root)`); the PS4 package writer
  leaves param.json out, so package -> any image -> package is still the
  same bytes; `alembic same` ignores a PS4 game's param.json; the engine's
  game info reads param.sfo first for PS4 games.
- Open: trophies (ShadowMount+ issue #97 reports "Trophy registration
  failed" for PS4 images); rebuilt PS4 packages not installed yet.
- **PS4 or PS5, decided like ShadowMount+** (`ffpfsc::game_platform`): the
  title ID first (`CUSA` PS4, `PPSA` PS5; other prefixes are homebrew or
  system software, `NPXS`), then eboot.bin's process parameters
  (`PT_SCE_PROCPARAM`, "ORBI"), then the metadata (param.sfo with no
  param.json, or one without contentId). In the process parameters the
  64-bit SDK field at +16 holds the PS4 SDK in its low half; **PS5
  executables put their SDK in the high half, PS4 ones leave it 0** (package
  P: version 1, SDK 1.75; package Q: version 5, PS4 SDK 11.00, high half 0;
  every PS5 sample: version 5, PS5 SDK 4.00; Alembic's app 2.00). The version
  alone does not tell (Q is version 5). ShadowMount+'s reader only takes the
  `EEF51454` SELF magic; Alembic also takes the fake-signed `1D3D154F` one.
  `alembic game-info <input>` shows all of it.

### 2026-10-26: a second PS4 package (Q, ~270 MB, newer tools)

- Its PFS did not decrypt although the all-zero passcode was right (entry
  keys digest 0 matches): CNT 0x408 has bit `0x2000000000000000` set, and
  then the PFS keys (encryption **and** signing) come from
  HMAC-SHA256(EKPFS, seed) instead of the EKPFS (LibOrbisPkg PfsReader's
  "newCrypt"; PS5 always does this). `Package::ps4_new_crypt()`; the ICV
  verifies. Alembic's own packages keep LibOrbisPkg's 0x80...03CC.
- Rebuilding Q: LibOrbisPkg reserves playgo-chunk.sha's room from an
  estimate made before the layout and throws when the package outgrows it
  (Q's large pictures and snd0.at9 push the body past another 512 KiB).
  Alembic keeps the estimate when it fits (P's bytes unchanged) and
  otherwise reserves what is needed and lays out again. Q's rebuilt package
  verifies; Q -> FFPFSC -> package equals Q -> package.
- Q's own sce_sys also has `nptitle.dat.encrypted`/`npbind.dat.encrypted`
  (from its packager, inside the image); kept as game files.

### 2026-10-26: PS4 packages byte-identical to LibOrbisPkg

`tools/ps4-oracle/compare.sh` (run by the owner) now passes everything:
both synthetic games byte-identical to `pkg_build` with `pkg_validate`
passing; package P's files equal `pkg_extract`'s; package P rebuilt by
Alembic equals `pkg_makegp4` + `pkg_build` byte for byte, and validates.
What it took:
- **The oracle on .NET 10:** a `-p:TargetFramework` does not reach the
  library PkgTool references, so the script builds a copy whose two
  `.csproj` files are retargeted; `pkg_build` needs its output folder to
  exist; `pkg_extract` writes under `uroot/`; `pkg_makegp4` writes
  `Project.gp4` inside the game folder and moves param.sfo's creation date
  into the project's `c_date` (passed as `--creation-date/-time`).
- **A LibOrbisPkg bug on .NET 6+:** its PFSCReader inflates a block with one
  `DeflateStream.Read`, which may now return less than asked; one file of
  package P came out short. The script patches the copy to read until full
  (as `patch_libprosperopkg.py` does for LibProsperoPKG).
- **Alembic's order** is now `pkg_makegp4`'s, whatever the source: sce_sys
  first at the top; in each folder, sub-folders holding CNT entry files
  first (lowest entry id), then by name; entry files first (by id), then by
  name (`entry_id_for_path` has all of LibOrbisPkg's EntryNames). Sony's
  images list folders in byte order, which this keeps; rebuilds from any
  image are still identical.

### 2026-10-26: 0.3.2 released

- Tagged and pushed; CI published the release (app `.ffpkg` and `.zip`,
  payload, PC packages, bundle, SHA256SUMS, all attested). The tag message
  lost its `###` headings (git's comment cleanup); the release body was
  fixed by hand (see "Making a release").
- ProsperoStore: `tools/catalog_record.py v0.3.2` (description now names PS4
  packages); the owner opens the PR from fthdgn-gh, since the description
  changed (the catalog's daily job would only propose the version).
- Open for ShadowMount+ (#97): it copies only PS5 trophy metadata
  (`update_trophy_metadata` in `src/sm_install.c`: trophy2/ and uds/
  npbind.dat, param.json), never a PS4 game's sce_sys/npbind.dat and
  trophy/, hence "Trophy registration failed" for PS4 images.

### 2026-10-26: PS4/PS5 pill, README, ProsperoStore (for 0.3.2)

- Owner: games carry a platform pill in the file list and the queue, on
  the page and in the app: **PS5** white with black text, **PS4** black
  with white text (outlined, to show on the dark rows). `/api/meta` sends
  `platform` ("ps4" from param.sfo, "ps5" from param.json, "" otherwise);
  the meta cache rereads entries saved without it.
- README: PS4 support in every section; the app's install steps put
  ProsperoStore first (https://prosperostore.net/app/PPSA99594/, listed since
  PR #740), the `.ffpkg`/folder second. Screenshots remade (the stand-in
  engine marks two games PS4; Chrome was missing, `BROWSER=` Playwright's
  Chromium worked).
- Catalog record: the description now names PS4 packages
  (`tools/catalog_record.py`). The catalog's daily job proposes new
  versions by itself, but a description change needs our own PR: after the
  v0.3.2 release, `tools/catalog_record.py v0.3.2` and a PR from fthdgn-gh.
- ShadowMount+ 1.7beta8 (pre-release) installs PS4 games from images and
  folders: the same four image types (`.exfat`, `.ffpkg`, `.ffpfs`,
  `.ffpfsc`), found by `sce_sys/param.sfo`.

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

### 2026-10-30: SMB shares and WebDAV servers beside FTP

- **Why (owner):** "We already have FTP support but let's add more options.
  SMB, WebDAV, you name it."
- **Shape:** `netfs.hpp` holds what all protocols share: the server list
  (`<state>/servers.json`, read from `ftp.json` when it is missing), the
  read-ahead cache for inputs, uploads under `.alembic-part`, the socket
  helpers. Each protocol is a `netfs::Session` (size, list, mkdir, rename,
  remove, download from an offset, front-to-back upload): `ftp.cpp` (as
  before), `smb.cpp`, `webdav.cpp`. Paths: `/ftp/ID`, `/smb/ID`, `/dav/ID`.
  API: `/api/servers` (`protocol`, `root`); `/api/ftp` answers the same.
  Browse and locations keep `ftp: true` (older pages and peers read it as
  "on a network server") and add `server: <protocol>`.
- **SMB (`smb.cpp`), from [MS-SMB2]/[MS-NLMP], nothing ported:** dialects
  2.0.2-3.1.1 (3.1.1 preauth SHA-512), NTLMv2 in SPNEGO with key exchange,
  every request signed after login (HMAC-SHA256, AES-CMAC from 3.0; keys by
  the SP800-108 KDF), 4 reads or writes of up to 1 MiB in flight, credits
  tracked. Guest login with no user. `root` is the share plus an optional
  folder ("Games/PS5"); "DOMAIN\user" names a domain. Refused with a
  message: encryption, SMB1-only servers. Responses are not verified.
- **WebDAV (`webdav.cpp`):** HTTP/1.1 keep-alive; PROPFIND (Depth 0/1),
  GET with Range, PUT with chunked coding, MKCOL (trailing slash, for
  nginx), MOVE with `Overwrite: F`, DELETE; Basic and Digest (MD5,
  MD5-sess, qop=auth) logins. An address typed as a URL fills port and
  path; https is refused. A server without chunked PUT says so (411).
- **Hashes (`netcrypto.cpp`):** MD4, MD5, HMAC-MD5, SHA-512, AES-CMAC on
  the core's AES; tested against RFC 1320/1321/2104/4493, FIPS 180 and the
  [MS-NLMP] 4.2.4 NTLMv2 example (`tests/test_netfs.cpp`).
- **Page and app:** Settings > Network servers (was FTP servers): a
  protocol picker (SMB, WebDAV, FTP) on add, Share or Path fields, the
  address shown the protocol's way (`\\host\share`); "On an SMB share" /
  "On a WebDAV server" where the free space would be. The stand-in engine's
  NAS is now an SMB share (`/smb/nas`), so the README screenshots will show
  that when remade.
- **Checked on the Mac:** Homebrew Samba 4.25 (`server signing =
  mandatory`, own smb.conf on port 4450; its runtime folders need a short
  path, and `--no-process-group` makes it kill its caller's group on exit)
  capped at each of SMB2_02, SMB2_10, SMB3_00, SMB3_02, SMB3_11: SMB ->
  SMB exFAT byte-identical each time. SMB -> local exFAT of a 1.4 GB
  package byte-identical (2 s); SMB -> SMB and SMB -> WebDAV exFAT 3.7 s
  and 4.6 s; FFPKG/FFPFSC equal to local builds but for the time stamps
  two local builds differ in; folder outputs identical; pause/resume of
  an SMB input; cancel mid-upload on SMB and WebDAV leaves no part file.
  WsgiDAV anonymous, Basic and Digest, with spaces, '&' and non-ASCII names
  in folders, files and the password. Wrong passwords and shares give
  plain messages. FTP (pyftpdlib) unchanged. Windows and PS5 builds pass.
  **Not tried:** a Windows 11 share, a NAS (Synology/QNAP), Nextcloud,
  the console.

### 2026-10-30: the queue stopped updating in the app (store review)

- **Report (Prospero Store review):** while compressing from and to USB,
  "calculating folder size" made the queue lose track (progress and items
  stopped updating) until that finished; the job itself went on.
- **Cause:** the app's client runs each lane's requests one after another,
  and `/api/jobs`, `/api/peers/jobs`, `/api/pair/incoming`, game info
  (`/api/meta`) and icons all used lane 1. On a USB drive busy with a
  conversion, game info, inspect (the Convert dialog walks the whole game
  folder) and plan take seconds each, and the queue's poll waited behind
  them. The page is not affected (it asks for two games' info at a time,
  under the browser's connection limit).
- **Fix:** a third lane: `Engine::kQuick` (what the screen waits on),
  `kSlow` (meta, icons, inspect, plan), `kPoll` (jobs, engine state,
  pairing). `App::api` picks the lane from the request (`App::lane_for`)
  unless a caller names one.
- **Reproduced on the Mac:** the app preview against the real engine with a
  job reading from an FTP server limited to 8 MB/s, behind a proxy that
  delays meta, inspect and plan by 4 s (as a busy drive would): the queue's
  poll went from every 1.2 s to once in 8-9 s while a folder of six games
  loaded; after the fix it stays at 1.2 s while game info keeps loading.

### 2026-10-30: NFS exports, and SMB servers without a share

- **Owner:** "add NFS support", "let's have the SMB shares too";
  encryption (SMB, TLS) comes later as its own piece of work.
- **SMB shares (`smb.cpp`):** a server added without a share lists its
  shares as the top folders (`/smb/ID/<share>/...`): DCE/RPC over the
  `srvsvc` pipe on `IPC$` ([MS-RPCE], [MS-SRVS] NetrShareEnumAll level 1,
  SMB2 WRITE then READ until the last fragment). Disk shares only; C$,
  ADMIN$ and other names ending in '$' are left out (they can still be
  named as the share). Each share gets its own tree connect when first
  used; files remember their tree, so pipelined reads and writes go to the
  right share. Moving between shares is refused.
- **NFS (`nfs.cpp`), NFSv3 from RFC 1813 / 5531 / 1833 / 4506:** ONC RPC
  on TCP with record marking, replies matched by xid (4 reads or writes in
  flight, sizes from FSINFO up to 1 MiB), AUTH_SYS with the server's
  "uid:gid" (the User field; empty: 0:0; no password). The portmapper on
  111 gives MOUNT's port, else MOUNT is asked on the NFS port (rclone,
  go-nfs). Calls come from a source port below 1024 when the system allows
  it (Linux "secure" exports), else any (`net::connect` can bind one).
  READDIRPLUS, else READDIR with LOOKUP; LOOKUP results for folders are
  cached per session; uploads are CREATE UNCHECKED (size 0), UNSTABLE
  WRITEs and one COMMIT whose verifier must match the writes' (else "the
  server restarted during the upload"). Without an export, MOUNT EXPORT's
  list is shown as a tree (`/volume1/games` as volume1 > games); a server
  that lists none serves "/". Address may be typed `nas:/volume1/games`.
- **Page and app:** a fourth protocol tile (2x2 grid, `.opts.two`), Export
  and "User and group ID" fields for NFS (no password), the share and
  export optional ("...; empty: all"), "On an NFS export".
- **Checked on the Mac:** Samba with a second share, a non-ASCII share
  name and a `$` share: listed (not the `$` one), a conversion into a share
  of the whole server byte-identical, a guest login. rclone 1.75 `serve
  nfs` (go-nfs; writes go through its cache, so files show up a few
  seconds after the job): NFS -> local and NFS -> NFS exFAT of a 1.4 GB
  package (about 3 s each) and a folder output byte-identical, a single
  export typed as `host:/path`, non-ASCII names, cancel mid-upload leaves
  nothing, clear errors for a wrong port or a uid that is not a number.
  rclone `serve webdav` as a second WebDAV server. **Not tried:** a Linux
  kernel or NAS NFS server (with a portmapper and "secure" exports), NFS
  from the console (can the payload bind ports below 1024?), a Windows
  share list.

### 2026-10-30: faster CI

- **Owner:** builds got slower as artifacts were added. The longest path
  was the PS5 payload (131 s of compiling, tests and CLI included) -> the
  app (99 s, all of the UI kit compiled again) -> the bundle: ~5 min.
- **Changes (`.github/workflows/build.yml`):** ccache in every compiling
  job (hendrikmuhs/ccache-action pinned to v1.2.24's commit; CMake
  launchers, the kit's `USE_CCACHE=1`); the PS5 job builds only
  `alembic_engine`; apt only for packages the runner image lacks (it has
  LLVM 18 and zlib; the app needs ninja); the app kit's downloads and host
  tools cached on the kit's scripts. Tags restore main's caches.
- **Measured:** v0.3.3 (old workflow) 309 s; the first run with caches
  (filling them) 342 s; the next, warm, 139 s: payload job 141 -> 28 s,
  app 126 -> 58 s, Linux PC 77 -> 46 s. Outputs are the same with and
  without the cache (payload and eboot.bin compared on the Mac).
- **ProsperoStore (0.3.3):** darkxex/ps5-homebrew-catalog#956 from the
  fthdgn-gh fork, branch update-alembic-0.3.3: `tools/catalog_record.py
  v0.3.3` with the description now naming SMB, NFS, WebDAV and FTP servers
  (168 of 200 characters); payload SHA-256 `b27478ae…` (the same file as
  the release's .elf).
- **0.3.3's first tag failed** in the app job: `app/sce_sys/param.json`
  still had 0.3.2's contentVersion. No release was made; the tag was moved
  to the fixed commit (owner agreed) and the release steps now name the
  file.

### 2026-10-31: ampr-emu-ng as a second AMPR emulator

(Dropped the same day at the owner's request, unpushed; brought back on
2026-11-02 by cherry-picking both commits, now following ng's latest
release: see "components follow their latest releases" below.)

- **quer3q/ampr-emu-ng** (GPL-3.0, created 2026-10-27, v0.1.0): a Rust
  rewrite of drakmor's ampr_emu, one `libSceAmpr.sprx` like it. Reads the
  same AMPRPAK4/AMPRDAT3 LZ4 packs Alembic writes, plus its own zstd packs
  (`ampr_zstd.index` + `ampr_zstd-NNN.pak`, `docs/zstd-format.md` there,
  written by quer3q/ps5-dump-forge's `ps5-dump-forge-zstd` crate, GPL-3.0).
  Its README measures ~90% less game wait per read and ~75% less memory
  than ampr_emu, and zstd packs 27% smaller than LZ4 on one game.
- **Integrated as a component** (`ampr_runtime_ng` in deps.cpp): the
  official release asset, SHA-256 `69ee3b42…af705956`, 667,408 bytes, kept
  at `<state>/runtime/ampr-emu-ng/libSceAmpr.sprx`. Both emulators are in
  the group `ampr_runtime`; `<state>/runtime/in-use` names the one
  conversions and "Add the AMPR emulator" use (`EngineConfig::runtime_path`,
  `--runtime` still wins). Downloading one makes it the one in use;
  `POST /api/deps/ID/use` (Components' "Use this one") switches back;
  removing the one in use falls back to the other. `/api/info`'s
  `lz4_runtime.pinned` is true for either, `name` says which.
- **ShadowMount+'s `update_emulators=1`** replaces a game's fakelib
  `libSceAmpr.sprx` with the one in its emulators folder, if there is one
  there: the component's text says to turn it off.
- **Replacing a game's emulator** (owner: "just add it to the fakelib
  folder, like the ones we added after the fact"): "Add the AMPR emulator"
  on a game whose libSceAmpr.sprx (its backport's, for an image) differs
  from the one in use offers "Replace" (`ampr_emu: "replace"`, CLI
  `ampr-emu --replace`): a folder's file is overwritten in place, an image
  gets (or its backport gets) the new one in a backport folder. No backup,
  no Convert option (owner's choice). The plan names both (`emulator_name`,
  `runtime_name`, `emulator_current`). A game with `ampr_zstd.index` only
  gets ampr-emu-ng. ampr_emu.index is left as it is: its size for
  `fakelib/libSceAmpr.sprx` goes stale, which nothing reads (ShadowMount+
  mounts fakelib outside /app0).
- **Not yet:** tried on the console (owner, with a local build); writing
  zstd packs (a port of the ps5-dump-forge writer, libzstd 1.5.7 to match
  it, its random archive ID would need a seed for an oracle). Waiting on
  how ng does on the console.

### 2026-11-02: zstd asset packs (ampr-emu-ng's format), written and read

- **Format** (quer3q/ampr-emu-ng `docs/zstd-format.md`): `ampr_zstd.index`
  (AMPRZPK1: 128-byte header with two CRC-32s, 32-byte file records sorted
  by (FNV-1a 64 of the path, path bytes), one u64 per 256 KiB chunk:
  offset, volume, stored size - 1, codec raw/zstd; volume sizes; paths)
  and `ampr_zstd-NNN.pak` volumes (AMPRZDT1: 64-byte header with the
  archive ID, chunks back to back from 64, at most 4 GiB - 64 KiB each).
  One chunk is one zstd frame (content size and checksum on, no
  dictionary, window 2^18) or raw when the frame is not smaller; a file
  whose chunks save under 1% (`stored * 100 > size * 99`) is all raw.
- **Port** of quer3q/ps5-dump-forge v0.5.1 (`df90a5c`, GPL-3.0):
  `ps5-dump-forge-zstd` (`lib.rs`, `index.rs`, `volume.rs`: encoder and
  validating decoder; `writer.rs`: frames, store-only rule, order, volume
  roll-over) and which files are packed (`ps5-dump-forge-lz4/src/rules.rs`
  `always_loose`/`keep_loose`/`select`, `ps5-dump-forge-core/src/
  zstd_pack.rs`): not empty, not at the root, not an executable or under
  `sce_sys/`, `sce_module/`, `fakelib*/`, not a config/media/container
  suffix or a protected folder; an LZ4-packed input keeps its LZ4 pack set.
  Note: ampr-emu-ng's doc says "everything else is packed"; ps5-dump-forge
  (what we match) is far more conservative (keep-loose list, root files).
  `PackOptions::packed_set` takes any other set.
- **Bytes as ps5-dump-forge makes them:** its Rust `zstd` 0.13.3 is
  `zstd-sys` 2.1.2 = libzstd 1.5.7, vendored here from the official
  release (`third_party/zstd`, SHA-256 checked, BSD, no asm, single
  thread; neither changes output). Each worker keeps a context with
  `ZSTD_c_compressionLevel`, `windowLog` 18, `checksumFlag`,
  `contentSizeFlag`; per chunk: reset (session only), pledge the size,
  `ZSTD_compressStream2` with `ZSTD_e_continue` until the input is in,
  then `ZSTD_e_end`, into an output of the chunk's own size (full = raw).
  The call sequence matters: a whole chunk then ends in an empty last
  block (`ZSTD_compress2` would make other bytes). Its archive ID comes
  from getrandom: Alembic takes `--zstd-archive-id HEX32`
  (`PackOptions::archive_id`, like FPKG's `--seed`), random otherwise.
- **Code:** `alembic/zstdpack.hpp`, `src/core/zstdpack_{format,build,
  read}.cpp`. A tree -> tree step like the LZ4 packer, no temporary copy:
  pass 1 compresses every chunk on all cores (reads on one thread) and
  keeps codec, size and CRC per chunk (~12 bytes); the volumes are made
  while the output writer reads them (compressed again, checked against
  pass 1). ps5-dump-forge writes from its first pass while the frames fit
  32 MiB and reads twice otherwise; same bytes either way. The runtime
  (`--zstd-runtime`, ampr-emu-ng's `libSceAmpr.sprx`, never committed)
  replaces `fakelib/libSceAmpr.sprx`; without it the game's own must be
  there (refused otherwise). A fresh `ampr_emu.index` lists every file,
  loose and packed (ps5-dump-forge writes one too); the exFAT, PFS and
  AMPR-emulator code now keep the index of a tree with `ampr_zstd.index`.
  Reader: `verify` (index, volume headers against it, every chunk decoded
  with ampr-emu-ng's frame checks plus the checksum), `unpack`;
  `convert`, `same` and digests unpack zstd inputs; `--zstd` on an
  LZ4-packed input unpacks it and keeps its set, on a zstd-packed one
  repacks; `--lz4` on a zstd-packed input unpacks first.
- **CLI:** `convert <in> <out> --zstd [--zstd-runtime F] [--zstd-level 1-22,
  default 5] [--zstd-archive-id HEX32] [--zstd-timestamp UNIX]`, any output
  but `.pkg`, not with `--lz4`; `--zstd-unpack`; `zstd-info`,
  `zstd-verify`. Core API for the engine: `convert::Options::zstd`
  (`zstdpack::PackOptions`: level, archive_id, runtime_module, timestamp,
  workers, packed_set), `zstd_unpack`, `Report::zstd` (`PackStats`);
  progress stage "zstd pass 1".
- **Tests:** 13 unit tests (`tests/test_zstdpack.cpp`): FNV-1a, chunk word,
  volume header and index fields read back at the documented offsets with
  zlib CRCs, damage refused, frames looked at with libzstd, the selection
  table, round trips (256 KiB, 256 KiB + 1, empty, noise store-only, the
  1% edge files of ps5-dump-forge's own tests), volumes rolled over with a
  small test limit, determinism over thread counts, images and back, LZ4
  -> zstd keeping the set. `tools/zstd-oracle/check_pack.py` reads a
  packed folder by the spec alone and makes every frame again with
  Homebrew's libzstd 1.5.7 through ctypes in ps5-dump-forge's call
  sequence: identical at levels 1, 3, 5, 19 on the synthetic game (also
  `--big`). **`tools/zstd-oracle/compare.sh` (ps5-dump-forge built with
  cargo from a scratch copy, its archive ID fixed by a patch, packs and
  loose files compared, each reading the other's packs) passes** (run
  by the owner, 2026-11-02): `ampr_zstd.index` and the volume byte-identical
  at levels 1, 3, 5 and 19, the same loose files, each tool reads the
  other's packs, and the exFAT, FFPKG and FFPFSC outputs carry the same packs. `compare.sh --no-cargo` passes. LZ4 oracle and the PS5PKGTool/MkPFS
  oracle unchanged; Windows and PS5 builds compile.
- **Measured** (14-core Mac, `--rebuild` to exFAT, level 5): a 2.4 GB exFAT
  sample: 0.32 GiB packed into 0.15 GiB (LZ4 scan profile: 0.24 GiB),
  1.8 s; the 13 GB FFPFSC: 26.2 GiB packed into 11.1 GiB in 3 volumes,
  32.7 s, image 13.2 GB (LZ4 scan: 17.5 GiB, 76.6 s, image 20.1 GB);
  `zstd-verify` and `same` pass. Level 19 on the small one: 14x slower for
  0.2% less.
- **Left:** run the ps5-dump-forge comparison; the engine, page and app
  (`--zstd` next to LZ4); pass-1 checkpoints for resuming; a console test
  with ampr-emu-ng (ShadowMount+'s `update_emulators` off).

## Making a release

1. **Update the README first** (owner): new features, limits, options;
   remake the screenshots if the page changed (`tools/screenshots/shoot.sh`).
   Carry every change into the translated READMEs (`README.<code>.md`, one
   per page language, English is the main one) and run
   `python3 tools/readme_check.py` (code blocks, images, links, headings).
2. Bump the version in `CMakeLists.txt`, `contentVersion` in
   `app/sce_sys/param.json` (`00.00Y.00Z`; `app/build.sh` refuses a
   mismatch, so CI's app job fails without it: 0.3.3's first tag did) and
   the "Where we are" heading above ("Version X.Y.Z" commit). Run
   `app/build.sh` once before tagging.
3. Write the release notes **cumulatively** (owner, 2026-11-08): a patch
   release keeps the minor release's notes whole and adds its own changes
   on top ("Fixes in 0.6.2", then "Fixes in 0.6.1", then 0.6.0's notes);
   after a major release, its notes stay and minors and patches are added on
   top the same way, summarized each time. Start from the previous tag's
   message (`git tag -l --format='%(contents)' vX.Y.Z`).
   Tag `vX.Y.Z` with the release notes as its message, **`git tag -a
   --cleanup=verbatim`** (otherwise git drops the `###` heading lines as
   comments; 0.3.2 lost them and the release body was fixed with `gh release
   edit --notes-file`), and push the tag: CI builds and publishes the
   release with the tag's message as its notes. Name the app files as CI
   does: `alembic-app-v<version>.ffpkg` / `.zip` (0.3.3's and 0.4.0's notes
   left out the `v`).
4. Check CI and the published release (every job green; the `.elf`, the
   app `.ffpkg` and `.zip`, the CLI archives, `SHA256SUMS`).
5. **ProsperoStore: open the catalog PR (Claude does it, owner,
   2026-10-31).** `tools/catalog_record.py vX.Y.Z > PPSA99594.json`; sync
   the fthdgn-gh fork of darkxex/ps5-homebrew-catalog (`gh repo sync
   fthdgn-gh/ps5-homebrew-catalog --source darkxex/ps5-homebrew-catalog`),
   branch `update-alembic-X.Y.Z` from upstream `main`, replace
   `apps/PPSA99594.json`, commit, push to the fork, and `gh pr create -R
   darkxex/ps5-homebrew-catalog` with the body of the previous one (#956,
   #993): what changed, the payload's SHA-256 (the app's
   `assets/alembic.elf`, the same file as the release's `.elf`), the
   checklist. The catalog's own CLAUDE.md is for its owner's direct pushes,
   not for contributors.

### 2026-10-31: Kraken-compressed FPKG output

- `fpkg-build` / `convert --to fpkg` take `--compression stored|kraken`;
  the engine's job spec has `pkg_compression` (default stored), the page
  and the app a Compression choice for FPKG (hidden for PS4 games: their
  packages are always stored).
- Encoder (`src/core/kraken_encode.cpp`): ProsperoPkgTool's (MIT, `f2c91cd`)
  KrakenLzEncoder + KrakenHuffmanArrayEncoder, byte for byte: hash chains
  (4-byte hash, 2^18 heads, 128 candidates), lazy matching, raw or
  package-merge Huffman arrays (code lengths up to 11), the excess framing
  for long lengths. A block is kept only when smaller; each file's last
  piece and executables / keystone / PlayGo-boundary files stay stored.
- Around it: the PFSv3 metadata container compresses its 256 KiB blocks
  (`pfsv3_compressed`), NAPS gets Kraken records (codec selectors from the
  encoder flags), naps_meta_18 the per-sub-chunk sizes and 0x40450000 flag,
  the NAPS header compression type 2. Pieces are encoded on every core in
  batches and written in order (same bytes for any thread count).
- Checkpoints: a Kraken piece's size on disk varies, so a `.chunks` side
  file (8 bytes per 128 KiB piece, appended like `.digests`) keeps how each
  piece was written; the State says which compression it was made with.
  Stored checkpoints from 0.3.3 still continue.
- Engines up to 0.3.3 ignore `pkg_compression`: a Kraken job is not sent to
  such a paired engine (409, "update it, or convert on this engine"); no
  protocol bump.
- Checked: `tools/pkg-oracle/compare.sh` now also builds the test game with
  both tools and compares (stored 5.6 MB, Kraken 1.6 MB: same bytes); a real
  118 MB package is byte-identical too (Alembic 0.4 s, ProsperoPkgTool
  5.9 s); a 1.3 GB package: 10 s, 1.6 GB (2.7 GB stored), every file reads
  back. Unit tests: encoder round trips through our decoder; a Kraken build
  paused and resumed equals an uninterrupted one (1 and 3 threads).
- App pictures (`app/preview.sh`): the Compression row shows for FPKG and
  steps to Kraken; hidden for the PS4 game. (A first try seemed to stall:
  the drives list's ✕ opens Internal, whose only row is homebrew, so a
  `down` there does nothing. preview.sh now carries a working example.)

### 2026-10-31: SMB 3 encryption; real packages written byte for byte

- **SMB encryption** (owner: "go ahead with encryption"; SMB first, TLS
  next): `smb.cpp` offers AES-128-GCM and AES-128-CCM (3.1.1 encryption
  context; 3.0/3.0.2 the capability bit, CCM), derives the keys at login
  ([MS-SMB2] 3.1.4.2: "SMBC2SCipherKey"/"SMBS2CCipherKey" over the preauth
  hash for 3.1.1, "SMB2AESCCM" with "ServerIn "/"ServerOut" before) and,
  once the session or a share asks for it, sends every request in a
  TRANSFORM_HEADER (nonce: a counter and a per-session salt) and opens the
  encrypted replies, checking their tags. Encrypted requests are not
  signed. Guest logins and 2.x dialects cannot encrypt: a share that
  requires it says so. Windows' client behaves the same (encrypts only on
  demand); an "always encrypt" setting is possible later.
- Ciphers in `netcrypto.cpp`, from SP 800-38D/38C on the core's AES
  (keystream 64 blocks at a time for the AES instructions; GHASH with
  4-bit tables): GCM spec test cases 2 and 4, RFC 3610 vector 1, an SMB-
  shaped CCM vector from Python's cryptography; tampered data is refused
  and left as it was. ~320 MB/s GCM, ~480 MB/s CCM on one Mac core.
- `tools/netfs/samba_check.sh` runs Homebrew Samba 4.25 (unprivileged,
  127.0.0.1:4450) in seven setups and the live test `smb_live_session`
  (3 MiB up, list, down whole and from an offset, rename, remove): 3.1.1
  GCM, 3.1.1 CCM-only, 3.0.2, 3.0 (all "required"), share-only required,
  no encryption: all pass; 2.1 with a share requiring it: refused. With
  sealing switched off on purpose the five "required" setups fail, so a
  pass means the traffic was encrypted. Samba on macOS notes: users with
  `pdbedit -a` (`smbpasswd -a` needs root), the share path resolved
  (`/tmp` is a link), `nt acl support = no` and fixed modes, and it cannot
  make folders unprivileged (4.25 renames a temporary folder), so the live
  test uses files only.
- **Real packages:** `tools/pkg-oracle/write_samples.sh` unpacks every
  sample PS5 package and builds it with both tools: all byte-identical,
  stored and Kraken (a 1.9 GB game: 374 MB with Kraken). Two samples
  ProsperoPkgTool refuses (no contentId, PS4-style PlayGo); the 80 GB one
  is over the default size limit. `patch_prosperopkgtool.py` gives
  ProsperoPkgTool's SI hashing its own managed SHA3 so it can build
  packages over 1 GiB on macOS.
- `app/preview.sh` was fine: a first try of the Convert dialog pressed
  `down` on the Internal drive's single folder. Its usage now has a
  working example.

### 2026-10-31: TLS for FTP and WebDAV (FTPS, HTTPS), pinned on first use

- **Decision (owner):** mbedTLS everywhere, not the console's libSceSsl:
  the payload SDK declares only `sceSslInit`/`sceSslTerm` and no homebrew
  we have runs TLS on its own sockets, so that would have been an
  undocumented API tried only on the console. Mbed TLS 3.6.7 LTS
  (Apache-2.0) is vendored in `third_party/mbedtls` (sources unchanged,
  SHA-256 in its UPSTREAM.md) with `alembic_config.h`: no sockets, files or
  entropy of its own (our `net`, `platform::secure_random` as PSA's
  external RNG), threading on, client only, no DTLS. Payload 5.9 MB.
- `src/engine/tls.hpp`: `tls::Link`, a connection that can switch to TLS
  (1.2 or 1.3) after it opened (FTPS's AUTH TLS). Trust on first use: the
  server certificate's SHA-256 (DER) is compared with the pinned one;
  none yet: refused as "not trusted yet (SHA-256 ...)", a different one:
  "changed". mbedTLS still checks the handshake's signature with the
  certificate's key, so the pin binds the key.
- FTP: explicit FTPS (AUTH TLS, PBSZ 0, PROT P), implicit on port 990,
  TLS on each data connection after the 1xx reply, resuming the control
  connection's session (vsftpd and ProFTPD require it). mbedTLS 3 hands a
  session out only once and drops TLS 1.3 tickets unless told to signal
  them: the Link keeps a copy (refreshed per ticket) for every resumer.
- WebDAV: HTTPS on the same keep-alive connection; Host and Destination
  use https and port 443 by default. `https://`, `davs://` and `ftps://`
  addresses switch TLS on (SFTP still refused).
- Servers keep `tls` and `fingerprint` (servers.json, `/api/servers`). An
  add or edit that meets an unknown or changed certificate answers 400
  with `certificate`; the page and the app ask ("Trust {name}'s
  certificate?", the fingerprint to compare; a changed one warns and its
  button is red) and send the same request again with `fingerprint`.
  Settings show FTPS / WebDAV (HTTPS); the stand-in engine has an FTPS
  server whose first save brings up the dialog (pictures of both checked).
- **Checked:** `tools/netfs/tls_check.sh` (self-signed certificates,
  pyftpdlib with TLS required on control and data and, like vsftpd,
  session reuse required; WsgiDAV on cheroot): FTPS TLS 1.3 and 1.2,
  HTTPS WebDAV 1.3 and 1.2, ECDSA P-256 for both, plain FTP and WebDAV:
  all pass the trust steps and the live session. macOS's curl (LibreSSL
  3.3) fails the TLS 1.3 reuse check that Alembic passes. Through the PC
  engine's API: unknown certificate -> 400 with it, wrong pin -> changed,
  right pin -> saved and browsable. PS5 payload and Windows build.
  **Not tried:** implicit FTPS (port 990 needs root here), a real NAS,
  vsftpd itself, the console.

### 2026-10-31: installing over HTTP (the experiment toward installable images)

- **Why (owner):** images should become installable like FPKGs. Research:
  `sceAppInstUtilInstallByPackage` takes an `http://` URL as `uri` as well
  as a path; ps5-web-file-manager (`src/pkg_installer.c`) hands SMB
  packages over as `http://127.0.0.1:<port>/pkg-source/<id>.pkg`,
  itsPLK/ps5-pkg-manager and ps5upload install from HTTP with byte ranges,
  the etaHEN writeup says so too (unverified there). Unknown: what the
  installer asks for (ranges, order, HEAD), whether it copies the package.
- **Engine:** `GET`/`HEAD /pkg-source/<token>.pkg` (`pkg_sources.cpp`,
  127.0.0.1 only, random 128-bit tokens) streams any readable path with
  `Range` (206, `Content-Range`, `bytes=-n`, 416); the HTTP server can now
  stream a file range in 1 MiB pieces. Every request is recorded:
  `GET /api/pkg-sources` (method, range, offset, length, ms, status).
- **Install jobs:** a package the installer cannot see (network server,
  paired engine, a browser's file) now goes over that URL by itself;
  `install_via: "http"` forces it for a local one. Files -> Install by
  streaming (test), in the page and the app (pictures checked).
- **Console test (owner, 2026-10-31):** a 1.3 GB debug package (not
  Alembic-made) installed over HTTP from `/data`; the title shows as
  installed. (A first try ran an older engine the app had carried: it
  ignored `install_via` and installed by path. The app bundled the payload
  a minute before the payload was rebuilt; rebuild the app after the
  payload.) What the installer asked for (`/api/pkg-sources`), against the
  package's segments FIH 0x0, PFS 0x10000, SC (CNT) 0x4f5e0000, SI
  0x523d0000:
  1. 64 KiB probes of the FIH header and the SC start, repeated ~6 times;
  2. the whole SI (2 MB at the end), then two 64 KiB pieces of the SC;
  3. the rest of the SC in order (~48 MB: CNT entries, media, trophies);
  4. then the PFS front to back in 18 MiB ranges, two in flight, ~188 MB/s.
  Every byte once (plus the repeated probes); ~9 s in all; nothing read
  after, so the installer copies the package.
- **For packages built on the fly from images:** the header, SC and SI
  (digests of the whole image, imagedigs, naps_meta_18, the CRCs) are read
  before any PFS byte, so a first pass must compute them all; the PFS can
  then be generated front to back as it is read (two 18 MiB ranges ahead).
  Deterministic output (fixed seed, deterministic Kraken, XTS per block)
  makes the second pass byte-exact. Next check: the same with an
  Alembic-made package (does it install, same order?).

### 2026-10-31: PS5 packages get a licence (the lock on the home screen)

- **Console (owner):** an Alembic-made Kraken package (from an FFPFSC
  dump, installed over HTTP) installed and shows on the home screen, with a
  lock next to the title. The owner cannot launch titles on that console.
- **Why:** dumps have no `sce_sys/license.dat`/`license.info`, and like
  ProsperoPkgTool we only packed them when present. Every sample package
  carries them (CNT 0x400/0x401, protected): a debug RIF exactly as
  LibOrbisPkg's `Rif/LicenseDat.cs` makes it (SkuFlag 3, IV and secret
  from SHA-256 of the content ID, the secret AES-CBC under
  `rif_debug_key`, signed with `DebugRifKeyset`), with DRM type 0x10 and
  content type 0x20 for PS5 (0xF/0x1A for PS4), and `license.info` with
  the content ID and content type. Generated from each sample's content
  ID, three PS5 and two PS4 samples are byte-identical (one more carries
  an unsigned licence from another tool).
- **Now:** `fpkg::ps4::debug_license_dat/info` take the DRM and content
  type; a PS5 game without a licence gets one when LibOrbisPkg's Keys.cs
  is given (`--ps4-keys`; the engine uses the ps4_keys component, which
  PS5 PKG output now needs too); without it, a warning. Placed as if the
  files were in sce_sys: `tools/pkg-oracle/compare.sh` checks that
  ProsperoPkgTool, given those files, builds the identical package.
- **Console (owner):** converted again with the licence and installed over
  HTTP: the lock is gone. The installer read it as it read the other
  package (probes, SI, SC, then the PFS in 18 MiB ranges in order).

### 2026-11-08: the Images tab

- **Why (owner):** games carrying an AMPR emulator (in the image or a
  backport) need it updated now and then; first a tab for that, then
  (owner, same day) a library: every image, game folder and package in
  ShadowMount+'s folders with Files' actions except moving and renaming.
  Owner's picks: tab named Images; it lists games that need an emulator too;
  an update keeps each game's emulator kind; Hide from ShadowMount+, Select
  several, Send to another engine (not Copy to a folder), Delete; packages
  listed.
- **Engine (`src/engine/images.cpp`, `GET /api/images`, `?refresh=1`):**
  scans ShadowMount+'s scan paths (its API's `scan_paths`, else config.ini's
  `scanpath`, else its defaults; on a PC `<state>/homebrew`) two levels
  deep: game folders, `.exfat/.ffpfsc/.ffpkg/.ffpfs/.pkg`, hidden `.name`
  ones too (not inside hidden folders, not `backports`). Each entry has
  Files' fields plus folder, hidden, allowed and `ampr` {status (current,
  update, missing, unknown, not_downloaded, none, unreadable), emulator,
  where, target, zstd_packs}; null for packages. ShadowMount+'s
  `/api/v1/games` only supplies `backport_path`; its `update_emulators`
  setting is shown as a note. Built on its own thread, cached 10 min.
- **Naming builds:** the components check now also reads each emulator
  repo's last 100 releases and keeps every `libSceAmpr.sprx*` asset's
  GitHub digest in components.json `known` (ampr_emu's older releases name
  them `libSceAmpr.sprx-0.3.6.1-test-debug` and so on: version
  "0.3.6.1-test-debug"); 22 builds of both at the time.
- **Updating:** "ampr_emu" jobs as before (`replace` with the game's kind,
  `auto` with the one in use where there is none, ampr-emu-ng for zstd
  packs); emulators not downloaded are asked for once
  (`pages/images/ampr.tsx`, app `images.cpp`). An unknown build is replaced
  only one at a time, after a question.
- **Page and app:** Files' Options and bulk menus take a library mode
  (`openActions(e, item)`, bulk with `tab === "images"`; app
  `open_actions(e, &item)`, bulk with `tab == Tab::images`); selection works
  on the current tab's rows. ✕ Convert, □ Options, as in Files. The list
  reads its folders again when a job ends. README screenshot
  `docs/screenshots/images.png` from the stand-in engine (mock.mjs).
- **Then (owner, same day):** Rename is back in Images (one, and Rename
  with a format for several); only moving and copying to folders stay out
  (Send to another engine stays). **Show in Files** opens the item's folder
  in Files with it focused. **Grid view:** Files and Images each have a list
  and a grid (tiles with the game's icon, the AMPR pill on it), switched by
  two buttons above the content (`components/ViewSwitch.tsx`, app
  `view_switch` in `tiles.cpp`); remembered per browser (`view.files`,
  `view.images` in localStorage) and in the app's prefs.json (`view`).
  Images starts as a grid, Files as a list; Files' drive list stays a list.
  Tiles are square by padding (the PS5 browser has no aspect-ratio); a grid
  is not a focus "column", so up from its top row reaches the buttons above.
- **Then (owner):** the drive list follows the grid switch too (drive
  tiles: symbol, name, room and meter). Images opened slowly: the list now
  comes at once and each game's AMPR state fills in as it is read (three at
  a time; "checking" until then), and what reading eboot.bin gave is kept in
  `<state>/images_cache.json` by path with size and time (for a folder its
  fakelib emulators too), so unchanged games are not opened again after a
  restart; the emulators here are hashed once per refresh, not per game.
  Ten real samples on the Mac: list after 0.13 s cold, 0.11 s cached. The
  page no longer opens with AMPR text: a Files-style bar (title "Images",
  summary; Update AMPR emulators only when there are some, Refresh, the
  switch), folder headings by drive name ("Internal · homebrew"), the first
  game focused.
- **Downloads (owner):** apps download packages to `/data/downloads`; the
  Images tab searches it too, every folder below it (16 deep, not hidden
  ones), for `.pkg` files only (`images::scan_packages`). On a PC
  (owner): the system's Downloads folder (`platform::downloads_folder`:
  Windows' FOLDERID_Downloads through SHGetKnownFolderPath, so shell32 and
  ole32 are linked; Linux's XDG_DOWNLOAD_DIR in user-dirs.dirs; macOS
  ~/Downloads).
- **Other engines (owner):** Images lists paired engines' images too, a
  section per engine. `GET /api/peer/images` (signed) answers an engine's
  own snapshot; `GET /api/peers/images` gathers every paired one's with
  paths under `/peer/<id>` (Convert, Install, Rename, Hide, Delete, Send,
  Show in Files work through the pairing as in Files) and its state: ok,
  offline, old (404: an engine before this, "update it"), error. Optional
  requests: no peer protocol change. **AMPR is a PS5 matter (owner):** an
  engine that is not a PS5 does not read games for it (`ampr: false`; the
  env var ALEMBIC_IMAGES_AMPR=1 turns it on for tests), and other engines'
  items carry no AMPR state, so Images offers AMPR actions only for a
  PS5's own games. Checked on the Mac with two paired engines (page and app).
- **Images first (owner):** the tab order is Images, Files, Queue,
  Components, Settings everywhere; the page and the app open on Images when
  the engine is a PS5 and on Files when it is a PC's (the page guesses from
  the console's browser until the engine answers; the app switches once
  its first info arrives, unless a tab was already chosen). Leaving (L2 +
  R2) goes back to that tab. A PS5 engine reads the Images list at start,
  so it is there when the tab first asks. README: the Images grid is the
  first picture, the list in the Images section, Files beside Convert.
- **Not tried on a console yet.**

### 2026-11-08: Alembic's own app and the AMPR emulator (after 0.6.0)

- **Owner:** adding the AMPR emulator to Alembic's app from Images made it
  crash at its next start (not while running). The APR check is a text
  search for "libSceAmpr" in eboot.bin, and the app names it in its texts
  (the Components page), so Images listed the app as a game with no
  emulator. The job then put `libSceAmpr.sprx` and `ampr_emu.index` in the
  app's fakelib or a `backports/PPSA99594` folder, which ShadowMount+ laid
  over the app at its next start.
- **Fixed:** `is_alembic_title` (PPSA99594, and the Probe Bench PPSA99595):
  never an APR user (`ampr_state`, and results 0.6.0 cached), an
  "ampr_emu" job on it is refused, and Images does not list it at all
  (owner). To recover by hand: delete `/data/homebrew/backports/PPSA99594`
  (image installs), or `fakelib/libSceAmpr.sprx` and `ampr_emu.index` in
  the app's folder (folder installs).
- **Clean-up at start (owner):** a PS5 engine undoes it when it starts
  (`images::repair_alembic_app`): a `PPSA99594*` backport in
  `backports_root` or a scan folder's `backports` (or one folder below) that
  holds only `fakelib/libSceAmpr.sprx` and `ampr_emu.index` is removed, and
  in the app's own folder those two files. The engine is a payload, so it
  gets there even when the app does not start: update it from the page in
  the console's browser (Settings > Updates) or load `alembic.elf`.
- **After 0.6.1 (owner):** Files does not offer "Add the AMPR emulator" on
  Alembic's own app either (page `isAlembicTitle` from the game's cached
  info, app the same; one and several). 0.6.1's engine already refused the
  job; this hides the option. For the next release.
- **No emulator for titles that do not use the APR (owner):** it has no use
  there, and on Alembic's app it broke the start. The engine refuses an
  "ampr_emu" job on a title whose eboot.bin does not use the APR, unless an
  emulator is already in place (switching stays possible); Files' "Add"
  says so instead of "It does no harm to add it", the bulk "Add" skips
  those games and says how many, and Images offers "Add" only for games
  that use the APR. Conversions only ever added it when the game uses it.
- **An exact APR check (owner):** `ampr_emu::check` reads eboot.bin's
  import table instead of searching it for "libSceAmpr": the dynamic table
  (PT_DYNAMIC; in a SELF, through each segment's data block, flag 0x800,
  stored unpacked), DT_NEEDED and the SCE needed-module / import-library
  tags (PS4 0x6100000F / 0x61000015 with DT_SCE_STRTAB in
  PT_SCE_DYNLIBDATA; PS5 0x61000045 / 0x61000049 with DT_STRTAB at an
  address), a name `libSceAmpr` or `libSceAmpr.<ext>`. An executable it
  cannot read that way (encrypted) falls back to the text search. On the
  owner's samples (nine games and Alembic's app): every game the same as
  before (six import it, three do not), the app no longer (its texts name
  it); check_samples.sh all "same". Unit test with two built ELFs.

### 2026-11-08: Probe Bench, the engine's checks in a game process

- **Why (owner):** do the ELF payload's process and a game process differ,
  and how? A bench to compare them before any engine work moves into the app.
- **`src/engine/probe.cpp`** (libc and libkernel only, none of the payload's
  kernel helpers, so the app links it): identity (pid, uid, jailed, CPU mode,
  direct memory), CPU (thread mask and priority, process cpuset, SHA-256 MB/s
  on 1-16 threads and the CPUs they ran on), memory (largest malloc; anonymous
  memory mapped and touched in 64 MB pieces up to 2 GB), files (stat, listing,
  file system, free space of system and storage paths; a test file only in
  /data, /download0 and drives; names listed only for system folders),
  256 MB write/read speed on /data and the first drive, network (listen,
  loopback to 5940, UDP broadcast, HTTPS to api.github.com; 9021 left alone),
  loading three system modules. Off the console: CPU only.
- **Engine:** `GET /api/probe` (`?io=0`, `?https=0`, `?mem=MB`); `POST
  /api/probe/app` keeps the bench's report (`<state>/probe_app.json`), `GET
  /api/probe/app` returns it. Diagnostic only: no page or app counterpart,
  no peer protocol change.
- **The title:** `ALEMBIC_APP_VARIANT=bench app/build.sh` ->
  `build-app/dist/PPSA99595{,.zip,.ffpkg}` ("Probe Bench (dev)", so it is
  not taken for Alembic on the console: its icon is the app's with red and
  blue swapped in an amber frame, `app/bench/icon.py`; the app's param.json
  with its IDs swapped). It carries the same engine payload as the app:
  with no engine on 5940 it sends it to the ELF loader; with an engine that
  has no `/api/probe` (404), □ sends it (a new engine on the console takes
  over the port whatever the versions). It runs its
  checks, then the engine's, shows both (differences coloured), posts its
  report to the engine and saves both JSONs in /data/alembic-bench (else
  /download0/alembic-bench). △ again, ○ close. Not drawn by preview.sh.
- **First console run (owner, 2026-11-08): SIGSEGV** ("user read
  instruction, page not present"). `statfs` exists only in libkernel_sys,
  which the bench linked as a NEEDED library; a game process does not have
  it, so the call jumped to an unresolved import. Now looked up at run time
  (load libkernel_sys, `sceKernelDlsym`; `files.statfs` says how it went),
  and each checked folder is logged as a step. Lesson for any engine code in
  the app: check `llvm-readelf -d` for NEEDED libraries beyond the app's.
- **Second run: the same crash.** So the payload SDK's libkernel stubs
  offer functions a game's libkernel may lack too. Now, in the app
  (`ALEMBIC_PROBE_DYNAMIC`), every function the app does not already import
  (scePthread*, cpuset_getaffinity, sceKernelGetCpumode/IsNeoMode/
  GetCurrentCpu, bind, listen, getsockname) is looked up with sceKernelDlsym
  on libkernel's handle (found by trying candidates; `identity.libkernel`
  says which) and reported "not in this process" when missing; getcwd and
  getppid dropped. The bench's imports are now a subset of the app's (diff
  of `llvm-readelf --dyn-syms`). Imported is not proven: the app never calls
  some of them. Hence breadcrumbs: each check is written to
  /download0/alembic-bench/step.txt first; a run that never ends puts it in
  crashed.txt, and later runs skip and list those (OPTIONS forgets them).
  The checks start after the first frames, so a crash before the screen
  shows is the UI, not the checks.
- **Third run (2026-11-08): it ran** (no crash, no check skipped; 1.9 s).
  Engine 0.5.3+probe vs the bench, same console, engine idle:
  - **CPU:** SHA-256 on 1/2/4/8/16 threads: engine 1047/1618/1674/1765/1672
    MB/s (mask 0xEA00, prio 700, ran on CPUs 13-15), game process
    1305/2574/5097/7005/7953. A game process gets ~4.7x the engine's CPU.
  - **Identity:** engine euid 0; game uid/euid 1. `security.jail.jailed`
    does not exist in the game (ENOENT).
  - **Files:** the game sees `/`, /data, /data/homebrew, /preinst, /mnt,
    /mnt/ext1, /app0, /download0, /dev; not /system*, /user*, /mnt/usb*,
    /mnt/sandbox, /mnt/shadowmnt, /hostapp. It can write /data,
    /data/homebrew and /download0 (mode 0777), not /data/alembic or
    /mnt/ext1 (root 0755). opendir fails everywhere (EPERM), /app0 too
    (libc's opendir; the kit already notes /app0 lists nothing) — listing
    needs another way (sceKernelGetdents?), not tried.
  - **Memory:** anonymous mmap stops at 256 MB in the game (ENOMEM; engine
    reached the 2 GB cap); sceKernelGetDirectMemorySize says 11712 MB, so a
    game's big buffers would be direct memory. largest_malloc (4096 both)
    means nothing: never touched.
  - **I/O on /data:** write 170 MB/s (game) vs 146 (engine); reads cached.
  - **Modules:** every sceKernelLoadStartModule by /system/common/lib path
    fails in the game with 0x80020002 (ENOENT: no /system in its
    sandbox). So **the app's HTTPS (libSceNet/Ssl/Http by path) cannot work
    as built: the shipped app's "download the engine from GitHub" fallback
    fails** (it has never been tried on a console). Games load these with
    sceSysmoduleLoadModule.
  - **Not answered yet:** no libkernel handle answered sceKernelDlsym
    (0x2001, 0x1, 0x2, and loading libkernel.sprx by path failed as above),
    so every "not in this process" (scePthread*, cpuset_getaffinity, bind,
    listen, ...) means "could not look it up", not "absent". bind/listen in
    the game are unknown.
  - Engine side: cpuset_getaffinity gives ERANGE (the SDK's cpuset_t size
    is not the kernel's); /mnt/ext0 with no drive in it is a small writable
    mount (ENOSPC).
- **Owner's decision (2026-11-08):** not pursued. The point was to see
  whether a game process gets more of the hardware (it does: ~4.7x CPU),
  but it is far more limited than the engine needs (files, drives,
  listing, modules); widening its rights is not explored. The engine stays
  a payload. The bench stays as a development tool.
- **The app's HTTPS fix (owner: fix it), step 1:** `https_ps5.cpp` gains
  ALEMBIC_HTTPS_IMPORTED (plain imports of libSceNet/Ssl/Http, which the
  loader fills at start, as for games). The bench is built that way first
  (its NEEDED list gains the three modules); the app stays on
  ALEMBIC_HTTPS_DYNAMIC until the bench starts on the console and its
  `network.https_github` says ok. Risk being tested: a title whose imports
  the console cannot provide does not start at all.
- **Step 2 (2026-11-08):** the bench built that way started on the console
  and `network.https_github` was ok (0.18 s). The app now imports them too
  (same NEEDED list as the bench); ALEMBIC_HTTPS_DYNAMIC is gone. Not yet
  tried end to end: an app with no engine inside
  (`ALEMBIC_ENGINE_ELF=none`) downloading its engine from GitHub; that also
  needs its version to be a published release.
- **Known going in:** the app's malloc is the UI kit's fixed 128 MiB heap
  (`app_heap.c`), so an engine inside the app would need its own heap; the
  bench shows that as `largest_malloc_mb`.
- **To do (owner, on the console):** install the bench image, run it, then
  `tools/probe/compare.py CONSOLE_IP`. Not run on a console yet.

### 2026-10-31: why conversions crawl on the console (/api/bench)

- Kraken conversions ran at 7.8 MB/s on the console (95 MB/s on one Mac
  thread). `GET /api/bench` on the console: it reports 16 cores; the
  Kraken encoder does 64 MB/s on 1 thread, 38 on 2, 10 on 4, 7.3 on 8,
  7.1 on 16: every thread added makes it slower (the Mac scales to
  1.7 GB/s on 14). Conversions use every core, hence 7 MB/s. Fresh vs
  reused tables on one thread: 58 vs 64 MB/s.
- Second bench on the console: SHA-256 (no allocation, no locks) 1029,
  1332, 1559, 1587, 1415 MB/s on 1, 2, 4, 8, 16 threads: the engine gets
  about 1.5 cores' worth of CPU, whatever the 16 it reports.
  `pthread_getaffinity_np` gave nothing. Kraken collapses far below that
  (62 -> 7 MB/s): with so little CPU, threads that allocate dozens of
  buffers per block wait on the allocator.
- **Changed:** every encoder buffer (arrays, bit streams, Huffman and
  package-merge lists) lives in the per-thread `EncoderScratch` and is
  reused; output unchanged (compare.sh, write_samples.sh). On the
  console: the same job 140 s -> 69 s; Kraken now ~75-80 MB/s on any
  thread count (no collapse), so no thread cap.
- **Why so little CPU:** a payload thread starts with core mask 0xEA00
  (logical CPUs 9, 11, 13, 14, 15) at priority 700 (low; lower numbers
  are higher). `scePthreadSetaffinity(0xFFFF)` is refused
  (0x80020001, EPERM); `scePthreadSetprio(500)` works but changes
  nothing (the five CPUs are shared with the system). `/api/bench?
  affinity=&prio=&elevate=1` tries these on its own threads only;
  `elevate` sets the mask under the shell's auth ID, as the installer
  calls run.
- **`elevate=1` hung the engine** (no HTTP answer at all, the ELF loader's
  port closed too): every bench thread raised and restored the process's
  auth ID at once through the payload's kernel read/write helpers, which
  are not meant for concurrent use. Removed. Raising privileges stays a
  one-thread, one-call affair (the installer calls). Widening the core
  mask would need kernel-level changes (as jailbreak tools make); not
  pursued: conversions are already 2x faster.

### 2026-10-31: installing an image like a package

- **Why (owner):** "the main goal is to have these images installable
  like they are FPKG files". Building the package on the fly while the
  installer reads it (no temporary file) would take three generation
  passes: the SI the installer reads first holds a SHA3 over [0, SI) in
  file order, starting with the header computed last. So: convert, then
  install, then delete.
- **Engine:** convert jobs to FPKG take `install_after`: the package goes
  to the installer over HTTP (`pkg_sources`), the job shows "Installing
  (the console reads the package)" by the bytes served, and once all of it
  was read and the installer has been quiet for 20 s the package is
  deleted (`installed`, `package_removed` in the result). No reading within
  2 min, or a 10-min pause part way: the job fails and the package stays.
  Not with `delete_source`. `keep_package` (Convert: "Keep the package",
  owner's request) leaves it after installing.
- **Page and app:** Convert -> FPKG -> "Install when done" (PS5 engines,
  PS5 games); Files -> Install... on images and folders opens Convert that
  way. Pictures of both checked.
- **Tested on a PC** with a stand-in installer (`ALEMBIC_FAKE_INSTALLER`:
  the PC engine reads the URL as the console's installer did): converted,
  read, deleted, the source forgotten. **Not tried on the console yet.**

### 2026-10-31: installing on the fly, measured and set aside

- **Console tests:** a package with a deliberately wrong SI (whole-package
  SHA3 and block CRC32Cs) was refused at once (`0x80b211cd`); the same
  game with a correct SI installed (stored, from an FFPFSC, with
  Install..., 89 s, package deleted). So the installer checks the SI, and
  a package built while the installer reads it needs three generation
  passes. Estimates on the console: this 1.3 GB game ~90 s stored / ~150
  s Kraken on the fly vs ~70 / ~80 s today; from a PC engine (network
  bound) ~25 s; a 100 GB game ~25-30 min from a PC vs ~1.2 h on the
  console. **Owner: scratch it; keep the improvements.** Install... stays
  convert, install over HTTP, delete.
- Removed the test-only pieces: the wrong-SI job option and "Install by
  streaming (test)". Kept: the HTTP package route (Install..., packages on
  servers and paired engines), `/api/pkg-sources` and `/api/bench` for
  diagnosis, the PC stand-in installer (`ALEMBIC_FAKE_INSTALLER`).
- **Not pursued:** installing on the fly (on the console or from a PC
  engine), widening the payload's CPU set (kernel-level), a shorter
  20 s settle before deleting.

### 2026-10-31: version 0.4.0

- Engine and app 0.4.0 (CMake, the app's contentVersion 00.004.000).
  README: what was tried on a console (Alembic-built packages install,
  no lock), what was not (starting such a game; NFS, WebDAV, SMB
  encryption, FTPS, HTTPS from a console), Kraken parity, Install when
  done; screenshots retaken (the stand-in engine has an FTPS server and a
  USB package now; page shots with Brave as BROWSER).
- Owner: the release text does not call Install when done "installing
  images" (misleading): it converts to a package and hands that over.
- Tagged (`--cleanup=verbatim`) and pushed; every CI job passed and the
  release is published (elf, app .ffpkg and .zip, CLI for Linux, macOS and
  Windows, SHA256SUMS). The notes named the app file without its `v`
  (as 0.3.3's did): fixed in the published notes with `gh release edit`;
  the files are `alembic-app-v<version>.*`. ProsperoStore:
  darkxex/ps5-homebrew-catalog#993 (opened by Claude, owner's request;
  payload SHA-256 `a64e40ea…`, the release's `.elf`).

### 2026-11-02: components follow their latest releases; ampr-emu-ng back

- **ampr-emu-ng** (quer3q) is back as a second AMPR emulator: the two
  2026-10-31 commits cherry-picked onto 0.4.2 (conflicts: engine flags in
  main.cpp, i18n lists, the CLI's `fpkg-build` help, Rename in actions.tsx).
  ng 0.2.0 (2026-11-02): libc exports, case-insensitive module loads, USB
  read retries, index misses fall back to the filesystem, APR waits on
  unmapped addresses fail instead of crashing (a save corruption). The
  bulk "Add the AMPR emulator" still skips games that ship another
  emulator (no bulk Replace yet).
- **Owner (2026-11-02): don't pin the components; follow their updates.**
  The emulators (`Dependency::feed`: `drakmor/ampr_emu`, `quer3q/ampr-emu-ng`,
  asset `libSceAmpr.sprx`) download GitHub's latest release. A download is
  kept only when its size matches and its SHA-256 matches the asset's
  `digest` GitHub's API gives (every asset uploaded since mid-2025 has one)
  and the release's `SHA256SUMS` when it has one; with neither it must be
  the fallback build. The fallback build (`files[0]`, the one last tested:
  ampr_emu 0.4.2.1, ng 0.2.0) is what a download gets when the API can't be
  reached. Trust is GitHub over TLS, as for the engine's own update (our
  TLS pins on first use, it checks no CA).
- **Key files stay pinned**: they are constants the writers check
  (fpkg_keys.cpp, fpkg_ps4.hpp), and byte-identical output depends on
  them; a change upstream would be a fault, not an update.
- **`<state>/components.json`**: `auto_update` (default on), `installed`
  (id → version, sha256, size, url) and `known` (every build downloaded
  here, so a game's emulator keeps its name, "ampr-emu-ng 0.1.0", after an
  update: `ampr_runtime_by_hash` now returns a `KnownBuild`). An emulator
  present with no record counts as its fallback build (an ampr_emu 0.4.2.1
  from an earlier engine stays "installed").
- **Checks**: at engine start with the engine's own update check
  (`--no-update-check` stops both), and `POST /api/deps/check`; with
  `auto_update` on, a newer build of a downloaded emulator is fetched (the
  emulator in use stays the same). `GET /api/deps` is still a plain list
  (older apps), each emulator now with `follows`, `version`, `latest`,
  `checked`, `check_error`, `update`; `GET /api/deps/status` gives
  `{auto_update, checking}` and `POST /api/deps/auto_update {on}` sets it.
  Games keep the build they got; "Add the AMPR emulator" offers Replace.
- **Page and app**: Components gets "Update automatically", "Check for
  updates", a Version row ("0.1.0 · 0.2.0 is out" / "· the latest") and
  "Update to X". Tried against a PC engine (download, a faked older record
  updated by hand and automatically, the switch off); pictures of both.
- **Not yet**: on the console.

### 2026-11-02: zstd packs in the engine, page and app

- Merged the `zstd-packs` branch (core, CLI, tests, oracle; see the entry
  above). The owner ran `tools/zstd-oracle/compare.sh` (it builds
  ps5-dump-forge with cargo; the agent's sandbox refuses that): every check
  byte-identical.
- **Engine**: `JobSpec` `zstd`, `zstd_level` (1-22, default 5),
  `zstd_unpack`; zstd excludes LZ4 and unpacking. The runtime put in
  fakelib/ is always ampr-emu-ng's (`EngineConfig::zstd_runtime_path`,
  the `ampr_runtime_ng` component, whichever emulator is "in use");
  missing, the plan asks for it in `needs`. Outputs get `-zstd`
  (`-unpacked` for unpacking); the result has `zstd` (files, bytes,
  volumes, warnings); the checkpoint fingerprint carries the settings
  (only when set, so older checkpoints still match). `/api/inspect` says
  `zstd_packed`. A job for a paired engine of 0.4.2 or older is refused
  (it would ignore `zstd` and convert without packs).
- **Page and app**: Convert offers "Pack into zstd packs" (PS5 games, not
  for FPKG; hides the LZ4 switch while on, and LZ4 hides it), a level
  (1 / 5 / 19) and a note to turn off ShadowMount+'s "update emulators";
  "Unpack zstd packs" for a zstd-packed game; "zstd-packed" in its facts;
  job cards tag "zstd · level N" and show what was packed. Tried against
  a PC engine (conversion verified with `zstd-verify`, ng 0.2.0's
  `libSceAmpr.sprx` in fakelib); pictures of the page and app dialogs.
- **Files list tag** (owner: "zstd is a compression type, not a new image
  format"; right: packs live inside any folder or image): `/api/meta`
  gives `packs` ("lz4", "zstd" or "", from `ampr_assets.index` /
  `ampr_zstd.index` at the game's root; meta cache entries without it are
  read again), and the page and app show an outlined LZ4 / ZSTD tag after
  the format pill.
- **Switching a game's emulator** (owner: "an option for the LZ4 packed
  games to switch between them"): `ampr_emu` jobs take `emulator`
  (`ampr_runtime` or `ampr_runtime_ng`; "" is the one in use), and their
  plan lists `emulators` (name, downloaded, in_use, in_game, reads_packs)
  plus `lz4_packs`. "Add the AMPR emulator" on a game that has one offers
  the others (a list when there are two; ampr_emu is not offered for a
  zstd-packed game, and a job asking for it is refused); one not
  downloaded yet is fetched first. Engines before this list no emulators
  and the page and app fall back to "replace with the one in use". Tried
  on a PC engine: an LZ4-packed folder switched from ampr-emu-ng to
  ampr_emu and back, from the page and the app; pictures of both.
- **Switching packs in Convert** (owner: an exFAT conversion "that just
  replaces the compression type", in Convert itself): an LZ4-packed game
  gets "Switch to zstd packs" and a zstd-packed one "Switch to LZ4 packs"
  (each hides its Unpack switch while on). Turning one on keeps the
  game's image type (exFAT stays exFAT; another can still be picked). The
  old packs are decoded straight into the new ones (no unpacked copy; a
  new image is still written). Output names drop the old tag (game-lz4 ->
  game-zstd). The engine picks LZ4 settings from a zstd-packed game's
  files, not its packs. Tried on a PC engine both ways (app: LZ4 -> zstd;
  API: zstd -> LZ4), `zstd-verify` / `lz4-verify` and `same` pass;
  pictures of both dialogs.
- **zstd pass 2 no longer compresses again** (owner, after a level-19
  job on the console ran at ~7.7 MB/s per pass): `PackOptions::spill_path`
  keeps pass 1's frames in a scratch file (about the packs' size; deleted
  with the packed tree, and a stale one is replaced), and pass 2 copies
  them, reading no source data for zstd chunks. The engine puts it next to
  the output (`<output>.alembic-partial.zstd-frames`, or
  `<state>/zstd-frames.tmp` when the output is on another engine, a
  server or a browser: it would cross the network twice), the CLI
  at `<output>.zstd-frames.tmp`. Output unchanged (a test compares bytes
  with and without; the 2.4 GB sample at level 19 gave the same image):
  29.4 s -> 20.4 s on the Mac there, where only 0.3 GiB is packed; about
  half the time where packing dominates, as on the console.
- **Hide / Unhide for any file or folder** (owner: a hidden package in a
  PC folder could not be shown again, since packages get no ShadowMount+
  option): the ⋯ menu and the selection menu offer "Hide" (rename to
  .name) / "Unhide" for everything that is not an image ShadowMount+
  uses (packages, plain files, folders); no rescan for those. "Hide from
  ShadowMount+" / "Show to ShadowMount+ again" stay for the images. Page
  and app; tried both ways on a PC engine.
- **Not yet**: on a console.

### 2026-11-03: the app's languages; translations are plain JSON

- **The app never translated** (owner: the language choice did nothing): it
  cut `i18n.js` at its first '=', which was in the file's opening comment,
  so the table never parsed and every language showed English. Section
  headings now upper-case accented letters as the page's CSS does
  ("APLICACIÓN", "ß" as "SS").
- **One plain JSON file per language** (owner): `i18n.js` became
  `src/engine/web/i18n.<code>.json` (es, fr, de, ja, tr; ~865 lines each)
  plus `i18n.messages.json` (the engine-message patterns all languages
  share), each served at `/i18n.<name>.json` (no `/i18n.js` any more; older
  apps could not read it anyway). CMake embeds every `i18n.*.json` it finds
  (`cmake/embed_i18n.cmake`: `kI18nNames` / `kI18nData` / `kI18nSizes`), so
  a new language is a new file (plus `LANGS` in `lib/i18n.ts` and
  `languages()` in the app). The page and the app fetch only the language
  in use (and the patterns, once) and switch once it is in
  (`loadTranslations(code)` in `lib/i18n.ts`, `App::use_language`); English
  needs none. `tools/i18n_keys.py` (no Node any more) also reports a key
  written twice: the old file had "Update to {version}" twice in every
  language (same text), dropped in the split. The old header comment lives
  in `lib/i18n.ts`. Checked against a PC engine: the page in a headless
  browser (Spanish, switched to Turkish and English, fetching only those),
  the app in Spanish, Turkish and German (switched in Settings).
- **Japanese in the app** (owner): IBM Plex Sans JP (OFL, google/fonts at
  `2fe1e8c`, Regular and SemiBold to match Inter; SHA-256s pinned in
  `app/tools/fonts.sh`, downloaded at build time like the SDKs) is baked
  into `ibm-plex-sans-jp-{regular,semibold}.huifont` (48 px, spread 6,
  4096-wide atlas, ~11 MB each) and set as the kit's fallback face: Inter
  Regular and DejaVu Mono fall back to Regular, Inter SemiBold to
  SemiBold (5 of the batch's 6 font slots). Glyphs:
  `app/tools/japanese_glyphs.py` (Japanese punctuation, kana, full- and
  half-width forms, the 2,965 JIS level 1 kanji, and whatever
  `i18n.ja.json` uses), 3,339 of them in the font; the kit's baker got a
  `file:` glyph set for it. Noto Sans JP was tried first: its static fonts
  are CFF (cubic curves), which stb_truetype's distance fields drop (kana
  came out in pieces); its TrueType variable font's default is Thin.
  `Font::wrap` in the kit now breaks between Japanese characters (it only
  broke at spaces, so a long run moved whole to the next line) and keeps
  the kinsoku rules. "Japanese" in the language list is 日本語 again, and
  the "English for now" hint is gone. Preview pictures in Japanese (Files,
  Components, Settings, a Japanese folder name) and English unchanged.
  **Not yet**: on a console.
- **Russian** (owner): `i18n.ru.json` (the model's translation, four
  agents over a shared glossary: движок, задача, образ, пакет, архив for a
  pack, связанные движки, Места; reviewed and a few fixed by hand). Russian
  plurals have three forms: the translation of a plural text holds the
  extra ones split by "|" ("{n} файла|{n} файлов" for the plural text,
  "one|few|many" for a text used for every count), and `plural()` on the
  page and in the app picks one (`pluralForm` in `lib/i18n.ts`; tested for
  0–1001). `i18n_keys.py` checks placeholders in each part. Numbers use a
  no-break space in the app; headings upper-case Cyrillic; the console's
  language 8 is "ru". **Language order** (owner): English, Türkçe, then
  Deutsch, Español, Français, Русский, 日本語 (page, app, README). Page
  and app pictures in Russian against a PC engine.

### 2026-11-06: zstd packs follow ps5-dump-forge v0.5.4

ps5-dump-forge moved from v0.5.1 to v0.5.4 (`70b4884`, `.refs` updated).
What changed its zstd bytes, now ported:
- **No 1% store-only rule** (v0.5.3, `writer.rs`): every chunk is
  compressed and kept raw only when the frame is not smaller; no file is
  flagged store-only. The reader still takes the flag (older packs).
  `PackStats::files_store_only` and the job report's `files_store_only` are
  gone (nothing read them).
- **Which files** (`rules.rs::select_everything`): everything not empty and
  not `always_loose` is packed - root files, configs, media, `save/` and the
  rest too (ampr-emu-ng serves plain and libc I/O on packed files). The
  keep-loose list and `PackOptions::packed_set` are gone, and an LZ4-packed
  input is chosen afresh instead of keeping its LZ4 set.
- **`always_loose` folds ASCII case** (`EBOOT.BIN`, `SCE_SYS/`,
  `FakeLib2/`, the artifacts), as images do.
- Packing a game whose eboot.bin does not import libSceAmpr: ps5-dump-forge
  now allows it (it used to refuse); Alembic never refused.

`check_pack.py` now expects no store-only file; the fixture gained
system files in other letter cases. 118 tests pass, `compare.sh --no-cargo`
passes (17 files packed), and so does the full comparison against v0.5.4
(cargo build; levels 1, 3, 5, 19 byte-identical, same loose files, each
reads the other's packs). The agent can run it here now (2026-11-06).

Not ported (their own policy or new features): pack profiles for zstd
(`--pack-profile`, with `include_from`/`exclude_from` files, also new for
LZ4 profiles), LZ4 "pack everything" when the runtime is ampr-emu-ng,
folder outputs writing `sce_sys/` and `eboot.bin` last, the zstd reader
taking a packed file that is also on disk.

**Fake-signing (their v0.5.3, `fself.rs`, not ported):** a dump whose
executables are plain ELF (`e_type` 0xFE10/0xFE18) never starts; every
conversion serves them as fake-signed SELFs, byte for byte what
ampr-emu-ng's `tools/make_fself.py --ptype fake` writes (GPL-3.0; paid
0x3100000000000002, ptype 1), planned from the headers and streamed, and
`ampr_emu.index` gets the SELF size. Executables: `eboot*.bin`, `.elf`,
`.self`, `.prx`, `.sprx`, `fakelib*/` included. ProsperoPkgTool (MIT,
`Content/ProsperoSelfBuilder.cs`) has its own flavour (ptype 0x10000101,
derived PAID, `.sceversion` records filled from eboot.bin for stripped
modules) that `img_create` applies to modules by default (`--no_fself`
turns it off); our FPKG writer refuses raw ELFs instead.

### 2026-11-06: fake-signing in conversions

A PS5 game whose executables are plain ELF (`e_type` 0xFE10/0xFE18) never
starts. `src/core/fself.cpp` (ported from ps5-dump-forge's `fself.rs` and
ampr-emu-ng's `tools/make_fself.py`, both GPL-3.0) serves them as
fake-signed SELFs, byte for byte what `make_fself.py --ptype fake` writes:
- **Which files:** `.elf`, `.self`, `.prx`, `.sprx` and an `eboot*.bin`
  (any case; data `.bin` files are never opened), `fakelib*/` included,
  at least 0x40 bytes, ELF magic and e_type 0xFE10/0xFE18; at most 4096.
  One make_fself would refuse (not x86-64, a segment past the end, headers
  over 64 KiB) is left as it is with a warning.
- **Streamed:** the SELF is planned from the headers; its bytes are made as
  they are read (head, then the copied segments, then PT_SCE_VERSION). The
  ex-info SHA-256 of the whole ELF is worked out on the first read that
  reaches it (the ELF is read twice, as in ps5-dump-forge).
- **Where:** `convert::run`, first thing after the PS4 check, so packers and
  image writers see the SELF sizes; every output but FPKG (still refused
  there) and never for a PS4 game. The game's own `ampr_emu.index` records
  of signed files that list the ELF size get the SELF size (the rest of the
  index byte for byte). `convert::Options::fake_sign` (default on); CLI
  `--no-fake-sign`; `alembic fself IN OUT` signs one file. A signed input
  image is no longer "copied unchanged": it is rebuilt.
- **Engine:** the job report lists `fake_signed`; verify-before-delete
  ignores those files' differences; the convert plan has
  `unsigned_executables` (a PS5 game's only), and Convert (page and app)
  says they will be fake-signed, or for FPKG that packages can't be made
  from them yet. The method reads "fake-signed the executables and ...";
  the page and the app also translate "zstd-packed/-unpacked and ..." now
  (they were left in English).
- **Checked:** `tools/fself-oracle/compare.sh`: six synthetic ELFs
  (`make_elfs.py`: ps5-dump-forge's own test ELF, whose SELF is 18917 bytes
  as its test says, plus a module with every copied type, a zero-size and two
  version segments, no program headers, late program headers, a segment
  over one digest block, a large ehsize) through `alembic fself` and through
  conversions to a folder and to exFAT and back: all byte-identical.
  `tests/test_fself.cpp`: the SHA-256, reads in windows from fresh trees,
  which files are found, the index records, conversions on and off.
  The stand-in engine (`tools/screenshots/mock.mjs`) gives Quiet Orbit 3
  unsigned executables; app pictures of Convert to FFPFSC and to FPKG looked
  at. **Not tried on a console:** the owner can't test it; it ships as
  is and users' reports decide (owner, 2026-11-06).
- **Then FPKG output** (same day, below).

### 2026-11-06: fake-signing in packages

FPKG output no longer refuses raw ELF modules: `fpkg::build::write_package`
signs them as `img_create`'s `FakeSignInnerModules` does (on unless
`--no_fself` there; `PackageOptions::fake_sign`, CLI `--no-fake-sign`),
with `fself::make_prospero_fself`, ported from ProsperoPkgTool's
`Content/ProsperoSelfBuilder.cs` (MIT):
- **Which files:** a file named `eboot.bin`, `*.elf`, `*.prx` or `*.sprx`
  (any case, any folder) whose first bytes are the ELF magic, any ELF type
  (the image flavour takes only 0xFE10/0xFE18), at least 0x40 bytes; signed
  in memory, as img_create does. Right after the files are collected,
  before pfs-version.dat and the keystone.
- **The flavour:** PS5 SELF magic 0xEEF51454, header bytes 10 01 01 12,
  program type 0x10000101, flags 0x32; entries 0x10004 / 0x2804; a segment's
  data right after its digests (aligned only between segments); the ELF
  header and program headers verbatim; authority ID from the ELF type and
  the byte at 0x3F00 (0x40, 0x80, else); ex-info, then 3, then the footer
  marker. Program headers must follow the ELF header (else refused);
  segments past the end are skipped. The module's `.sceversion` section is
  appended; a stripped module other than `/eboot.bin` gets a record made
  from eboot.bin's first one (`name:`, tuple twice; the name as .NET's
  ASCII encoding writes it). A file it cannot sign is packed unchanged with
  a warning, as img_create does.
- **Checked:** `tools/pkg-oracle/compare.sh` builds a game with raw ELF
  modules (`make_elfs.py --package-game`: an eboot with a `.sceversion`
  section and 0x40 at 0x3F00, a stripped module, one with its own record, a
  fakelib module with 0x80, one img_create refuses, an ELF under a data
  name, a module that is already a SELF): fpkg-build equals img_create byte
  for byte, stored and Kraken; and `alembic fself --prospero` equals
  `ProsperoPkgTool fself` on every ELF of both fixture sets (or both
  refuse). `tests/test_fself.cpp` pins four of its outputs. Convert's note
  no longer says packages can't be made; app picture looked at.
  **Not tried on a console:** left to users' reports, as above.

### 2026-11-07: Hungarian

- **Hungarian** (`i18n.hu.json`, all 871 texts), translated by isty1974,
  who offered it in issue 4. Line endings made Unix; otherwise as sent.
  Listed as Magyar between Français and Русский (page, app, README).
  Two plural forms (nouns after a number stay singular in Hungarian, the
  translation handles that). Numbers as the page's `toLocaleString("hu")`:
  no separator for four digits, a no-break space from five. The console's
  language 24 is "hu" (the public PS4 numbering the other cases follow;
  not checked on a console). Fonts already had ő/ű and „.
- **Fix found on the way:** the app's queue status badges upper-cased
  only a–z ("KéSZ", and French "TERMINé" likewise); they now use the
  same `draw::upper()` as the headings, and their width counts letters
  instead of UTF-8 bytes. Page and app pictures in Hungarian against the
  stand-in engine looked at.

### 2026-11-08: Simplified Chinese

- **Simplified Chinese** (`i18n.zh.json`, all 871 texts), asked for in
  issue 5. The translation is the model's: the issue's author is to be asked to
  review it. Listed as 简体中文 after 日本語 (page, app, README). One plural
  form; numbers as English (`toLocaleString("zh")`). Browsers set to zh-CN
  or zh-TW get it (the page reads the first two letters). The console's
  languages 10 and 11 (Traditional, Simplified; public PS4 numbering, not
  checked on a console) map to zh. Traditional Chinese may come later: it
  needs its own texts and font.
- **App font:** IBM Plex Sans SC (OFL, from IBM/plex at `48e8676`, unhinted
  TrueType, SHA-256 pinned in `app/tools/fonts.sh`), baked with
  `app/tools/chinese_glyphs.py`: CJK punctuation, full-width forms, the
  3,755 GB 2312 level 1 hanzi and what the texts use (3,901 glyphs, 13 MB
  per weight, under the app's 16 MB font limit). Japanese and Chinese draw
  shared characters differently, so `ui/cjk_fonts.hpp` puts the Chinese face
  before the Japanese one only while the language is zh (checked each
  frame). Page and app pictures in Chinese against the stand-in engine
  looked at.

### 2026-11-09: README guide

- README "Guide" (every language): which format when, how-tos, tips. Based
  on a look at ShadowMount+ 1.7beta12 (2026-11-08) and the AMPR emulators:
  FFPKG is still SMP's default; exFAT for titles that misbehave (often
  sound), read fastest directly under /data or /user with 64 KiB clusters;
  PFS/PFSC still experimental there, PFSC decompresses at ~150-250 MB/s
  (big open-world games stutter); ampr-emu-ng reads LZ4 and zstd packs and,
  by its own figures, waits ~8x less per read with ~1/3 the memory of
  ampr_emu; SMP's "update emulators" puts ampr_emu back over it.
  Re-check these before changing the guide's advice.
- theghostonline/PS5-LZ4-Compression is gone (404): the READMEs name it
  without a link; THIRD_PARTY_NOTICES keeps the URL as the record.
- Menu paths in the READMEs are written with "→".

### 2026-11-10: Convert, step by step

- Convert (page `dialogs/ConvertDialog.tsx`, app `actions.cpp` Convert) is a
  wizard (owner): Format, Options, Where, Afterwards, Review; a step with
  nothing to choose is left out. ✕ on a format picks it and moves on, then
  Next has the focus on each step (✕ ✕ ✕ takes the defaults); ○ goes a step
  back (closes on the first: `Overlay.onBack` on the page, `back()` in the
  app); □ goes to Review, and queues from there. L1/R1 were tried and
  dropped (owner: confirm and back instead). Review: a line per step (✕
  goes back to it), the source or the games beside it, then the notes.
  Install… opens on Review.
- Formats: all six, FFPKG first with "Recommended" (and the default, exFAT
  for games already FFPKG); the source's own format is shown as "Current"
  instead of hidden: picking it changes only the asset packs, and when
  nothing changes the footer says so and nothing queues.
- The four pack/unpack switches are one "Asset packs" choice (None / LZ4 /
  zstd, the games' own marked "as now"; none for a mixed selection). The
  AMPR switch shows only when a plan says the game needs it.
- Asset packs sit on the Format step, above the cards (owner, after trying
  it on the console: repacking an exFAT game meant Options, then back to
  Format). Until a format card is picked, changing the packs selects the
  game's own format (back to the first default when set "as now"; not for
  packages). Options keeps the levels, the LZ4 profile and AMPR.
- The stand-in engine (`tools/screenshots/mock.mjs`) now reports packed
  games in `/api/inspect`; `shoot.sh convert` shows the Review step.

### 2026-11-11: PS5 updates, researched and shelved

- **Fake-signed (debug) update packages** (`\x7FLIH`): a 64 KiB header,
  then the patched package's FIH block, the outer blocks the update
  changed, its CNT and a zip of PlayGo/relocation tables. FIH and CNT
  describe the whole patched package (`imagedigs` has a SHA3 per outer
  block); every block not carried is the base package's. So merging needs
  the exact base FPKG, each borrowed block checked against `imagedigs`; a
  dump or image cannot stand in for it. ps5-dump-forge (`lih.rs`, GPL-3.0)
  reads them (layout measured on one real update) but never applied one to
  a base. Blocked: no sample pair (base FPKG + its update FPKG); owner
  knows of no source. Shelved (owner).
- **Official updates** (prosperopatches.com / orbispatches.com list
  them on Sony's servers): per version, numbered pieces (joined in manifest
  order, one package) plus "DP" (the site's code: `packages.delta`), "SC"
  and "CRC" (a CRC-32C per 64 KiB block, for checking only). A PS5 update
  is the whole game at that version. They are retail: encrypted with a key
  the console gets through the game's licence, so a PC cannot open them.
  The sites' file links sit behind a Cloudflare check; not scraped.
- **knutwurst/ps5-patchdl** (GPL-3.0, `.refs/ps5-patchdl`): downloads
  only the pieces from Sony's manifest (`originalFileSize`, `pieces[]`
  with `url`, `fileOffset`, `fileSize`, `hashValue`) and hands the package
  to `libSceAppInstUtil` on the console; no decryption, no keys. Works only
  for games installed from disc or the Store, so not for images: not
  Alembic's job.
- Possible without packages: ShadowMount+ lays a game's backport folder
  over its mounted image, so loose update files could ride on an image.
  Untried.

### 2026-11-12: images from installed games, discussed and shelved

Owner's question; nothing tried on a console. To pick up later (owner).
- **Installed fake packages:** the console keeps the package under
  `/user/app/<TITLE_ID>/` (probably `app.pkg`; not checked). If it is the
  debug FPKG itself, Alembic reads it as any package (zero passcode) and can
  convert it; the work would be an "installed games" list. First step: look
  in that folder with Whole file system on (read only).
- **Retail games:** their packages are encrypted with the console's keys.
  While a game runs, its files are mounted decrypted, likely under
  `/mnt/sandbox/pfsmnt/<TITLE_ID>-app0` and `/mnt/sandbox/<TITLE_ID>_000/app0`
  (what dumpers use; not checked here; the engine sees `/mnt/sandbox`). Data
  files are readable there, but `eboot.bin` and `sce_module/*.prx` stay
  encrypted SELFs: the kernel decrypts them only while loading, so a dump
  needs kernel read/write (etaHEN's and other dumpers). The executables are
  the game's compiled code and cannot be made from its data. Decided: no
  kernel code in Alembic (risk, firmware churn); a dumper dumps, Alembic
  converts what it produces. Possible later: offer to convert a dumper's
  fresh output (owner: not now).

### 2026-11-12: decrypt a running game's executables (self-pager port, phase 1)

Owner chose to port ftpsrv's self-pager into the engine (option 3) so a
conversion can turn a running game's encrypted SELFs into plain ELFs itself.
Built the core + kernel + engine job hook; **page/app UI, i18n and a CLI flag
are deferred until an on-console smoke test**. Not run on a console yet.
- **`src/core/self_extract.{hpp,cpp}`** (portable, GPL-3.0 from ftpsrv `self.c`):
  parse the SELF (0x20 header, 0x20 entries, embedded ELF), copy plaintext
  segments and the PT_SCE_VERSION tail (at the SELF's `file_size`), write the
  program-header table, and get each encrypted segment from a `Decryptor`
  callback; verify the ELF's SHA-256 against the ex-info digest. Entry bitfield
  read explicitly (is_encrypted bit 1, is_compressed bit 3, has_blocks bit 11,
  segment_index bits 20-35). `decrypt_executables(tree, cb)` serves every
  encrypted-SELF executable as its ELF via a replaced-contents wrapper; needs a
  real on-disk path, so folder inputs only. Unit test `test_self_extract.cpp`
  (synthetic SELF + stand-in callback, digest pass/fail, folder walk).
- **`tree::Source::real_path(node)`** added: FolderSource returns
  `paths_[id]`, everything else "" (the only way the step learns a file's path).
- **`src/engine/selfdump_ps5.cpp`** (PS5 only, GPL-3.0 from ftpsrv
  `self-prospero.c`; host stub in platform_host.cpp): the `Decryptor` maps one
  segment with the vnode pager-table entry swapped for the self pager's
  (`kernel_getlong`/`setlong` at the per-firmware offset, full 1.00-14.20
  table), under a global mutex; restores it immediately, mlock + copy out.
  Lazy-resolves the table on first use; refuses unknown firmware. Untested:
  whether `kernel_set_ucred_authid` is needed first (ftpsrv sets it, but for
  /dev/sflash0, not the pager).
- **convert**: `Options::decrypt_self` callback, runs first (before bc1 /
  fake_sign) so the plain ELFs get fake-signed; `Report::decrypted_self`.
  **runner/jobs**: `decrypt_self` job field; the engine sets the callback from
  `platform::self_decryptor` and fails the job (PC / unknown firmware) with the
  reason; verify tolerates the decrypted files differing; in the checkpoint
  fingerprint. Builds clean on Mac (138 tests) and PS5 (`tools/ps5/build.sh`,
  selfdump compiled, no warnings).
- **Smoke test (owner, with go-ahead):** load the new payload, then on the
  running game `POST /api/jobs {kind:convert, input:/mnt/sandbox/pfsmnt/
  <ID>-app0, to:ffpkg|exfat, decrypt_self:true, ...}`; check the executables
  come out plain then fake-signed, and whether the image starts.

### 2026-11-12: a running retail game, looked at on the console

With the owner, a retail game every PS5 ships with running, test engine
(Whole file system on), read only:
- `/mnt/sandbox/pfsmnt/<TITLE_ID>-app0` is the game's tree, mounted
  decrypted: the engine walked it all (33,270 files, 27.2 GB; plan for an
  exFAT accepted); ShadowMount+ (`sm_fakelib.c`) expects it as fstype
  `ppr_pfs`, the system's mount of `/user/app/<TITLE_ID>/app.pkg` (11 GB
  here, compressed). `<TITLE_ID>-app0-patch0-union` is the console's union of
  an update (`patch0`) over app0; this game had no update (no /user/patch),
  so it listed the same as app0. `<TITLE_ID>-app0-nest/pfs_image.dat` (and
  `naps_pkg_layout.dat`, same size) starts with "keystone": still the
  console's container, not a PFS Alembic can open. `/mnt/sandbox/<TITLE_ID>_000`
  is the game's own view (app0, data, dev, ...).
- Through the mount, `eboot.bin` and the modules are SELFs (no plain ELF).
- **ftpsrv v0.22 (ps5-payload-dev/ftpsrv, port 2121) serves `eboot.bin`
  decrypted**: its `SELF` command (on by default, per connection) turns
  SELFs into ELFs on the fly, in listings too. Default: a plain ELF (e_type
  0xFE10), 71,614,632 bytes; with `-Q SELF` (off) the same file is the SELF
  (magic `54 14 F5 EE`, 71,736,750 bytes, what the engine sees). The kernel
  step is that payload's, not Alembic's.
- **How ftpsrv decrypts** (`.refs/ftpsrv` at 92072f2, GPL-3.0, John
  Törnblom; `self-prospero.c`, `self.c`; derived from idlesauce/
  ps5-self-pager): no crypto of its own. With kernel R/W it swaps, for one
  `mmap`, the vnode entry of the kernel's pager table (per-firmware offset,
  table for 1.00-14.20; `PAGER_TABLE + 2*8`) for the self pager's (`+ 7*8`),
  maps one segment (index << 32 in the offset; from 9.00 plus the aligned
  vaddr's low bits) and gets it decrypted as the program loader would, then
  restores it. ELF header, program headers, plain segments and
  PT_SCE_VERSION (after the SELF's end) are copied; the ELF's SHA-256 must
  match the SELF's ex-info digest. A global lock: while swapped, every vnode
  mmap in the system goes through the self pager. Options discussed: (1)
  convert from the console's own ftpsrv; (2) data from the mount, only the
  executables from ftpsrv (recommended); (3) port the pager swap into the
  engine (kernel patching, per-firmware table). Not decided.
- So, untried: the console's own FTP server (127.0.0.1:2121) as a network
  server in Alembic, the app0 (or, with an update, the union) folder
  converted from it while the game runs;
  Alembic fake-signs the plain ELFs and adds the AMPR emulator (the game
  imports libSceAmpr). Open: whether ftpsrv decrypts the .prx too, speed
  over FTP, whether the image starts. A later feature could read data from
  the mount and only the executables from ftpsrv. Still no kernel code in
  Alembic.

### 2026-11-12: smaller sce_sys DDS (issue #1), research

- **Issue #1** (a user): re-encode the `sce_sys` DDS files to save space.
  Our first answer (2026-10-22): they are BC7, the home screen reads them,
  ~25-60 MB of images per game, PNG re-compression ~1%, no bundled tools,
  no Oodle. **The reporter's console test (2026-11-12): BC1 works**, the
  pic0/pic1/pic2 animation included, at half the size; **BC3 showed only
  pic0** (and is BC7's size anyway). Owner's reply on the issue: an opt-in
  image optimisation, probably in the next release.
- Measured on three sample games: `pic0/1/2.dds` 3840x2160 and `icon0.dds`
  512x512, all `DX10` header, DXGI 98 (BC7_UNORM), one mip level; 8,294,548
  and 262,292 bytes. The PNGs are the same size, RGB without alpha (colour
  type 2), so they are the lossless source to encode BC1 from (BC7 -> BC1
  would compress twice). BC1 at 4 bits a pixel: 4,147,348 bytes a picture,
  ~12.4 MB saved per game before compression (less inside an FFPFSC, whose
  zlib already takes 3-27% off BC7).
- Not known yet (ask the reporter): the BC1 files' header (DX10 with DXGI
  71 BC1_UNORM / 72 sRGB, or legacy `DXT1`), mip levels, whether icon0.dds
  was converted too, and which encoder. BC3 failing while BC1 works is odd
  for a format the GPU reads natively, so the header may matter.
- **Built the same day (owner: opt-in, let the reporter experiment):**
  `convert::Options::bc1_pictures`, CLI `--bc1-pictures`, job field
  `bc1_pictures`, Convert's Options step on the page and in the app
  ("Smaller home-screen pictures", off; the step now shows for every PS5
  game). `alembic/png.hpp` (decoder from the PNG spec, vendored zlib, CRCs
  checked, no Adam7) and `alembic/dds.hpp` (BC1 encoder: principal axis,
  least squares, then one-565-step search; all cores, same bytes on any
  thread count; tree step `smaller_pictures`). Only `sce_sys/pic<N>.dds`
  that are single-level BC7 with a DX10 header and a same-size PNG; the
  header is kept with DXGI 71 (72 for sRGB) and the linear size; icon0
  untouched. Runs before fake-signing; `ampr_emu.index` records of those
  files resized (`ampr::resize_records`, now shared with fself). Verify
  allows them to differ; in the checkpoint fingerprint when on.
- Checked on the Mac: package A to a folder, 3 pictures 8,294,548 ->
  4,147,348 bytes in ~1 s; an independent Python BC1 decoder against the
  PNGs: 43-44 dB PSNR. Engine job with delete_source + verify passes.
  Unit tests `test_dds.cpp`. Not yet on a console (the reporter's test
  used their own BC1 files; header differences are the open question).

### 2026-11-12: Whole file system (owner's request)

- **Settings → Whole file system** (page and app, `POST /api/whole_filesystem
  {on}`, kept as `<state>/whole-filesystem.on`): "/" becomes one more root,
  shown in Files as the drive **File system root**; everything under it can
  be read and changed like the drives (owner: writing allowed). The folders
  right under "/" (/data, /system, /mnt, ...) cannot be renamed or deleted.
  Not on Windows (no single "/").
- A first try listed every top-level folder next to the drives; the owner
  asked for one entry instead. So **the list of drives is now path ""** in
  the page (`TOP` in state/view.ts) and the app (`cwd` empty), and "/" can
  be a real folder. `/api/browse`: `path=` is the list; `/` (or no path) is
  still the list unless "/" is a root, so older pages and apps keep working
  while the switch is off.
- `EngineConfig::shared_roots()` (the drives) stays for what must not grow:
  paired engines (`/api/peer/fs/*`, protocol unchanged), the update's payload
  search and the PC's suggested places. `/api/fs/space` includes "/" (the
  drive column shows it). `/api/info` adds `shared_roots` and
  `whole_filesystem {supported, on}`.
- Checked on the Mac: on/off, survives a restart, browsing in and out of "/"
  in the app (pictures) and the page (headless browser), back and crumbs
  land on the right rows. Not tried on a console yet.

## Next session

**Settled (owner, 2026-10-22):**
- Games crashing with the engine idle: not only caused by the payload;
  closed.
- The 1.5 h for the ~90 GB package is not worth chasing: FPKG → exFAT
  without compression takes about 30 min on the console, fine for now.
- LZ4 packs stay as they are.

**Settled (owner, 2026-11-11):**
- ✓ A zstd-packed game starts on the console.
- Installing an Alembic-made FPKG: the owner's console cannot install
  FPKGs, so it stays untested; no issues reported by users either. Left
  as it is.
- Network servers (item 8) set aside: not the main use for users.
- PS5 updates shelved (log entry "PS5 updates, researched and shelved"):
  official updates are retail and need the console's licence; merging
  works only with fake-signed update packages, and we have none.

**Open:**
- **Where we left off (2026-11-11, release 0.6.3):** nothing half-done.
  Still to try on the console: switching a game's emulator, switching
  packs in Convert, Hide / Unhide, an emulator auto-update (needs a new
  upstream release). Next candidate: PS4 updates (item 6).
- **Console app (0.3.0):** ✓ first console run (owner, 2026-10-25): the
  app starts, reaches the engines, ran a PC-engine conversion to the
  console, stopped the engine and (reopened) started it through the ELF
  loader; it feels right. ✓ Restart the engine (2026-10-25). Agreed next
  (owner, 2026-10-25; see the log entry "restarting the engine"):
  1. ✓ the app ships as `alembic-app-<version>.ffpkg` built by our own UFS
     writer in CI (the folder stays for development);
  2. one version and one release for engine and app; the engine's updater
     also replaces the app image (only when its SHA-256 differs), however the
     update was started (page or app); an open app is swapped when it
     closes (staged under a dot name ShadowMount+ skips, then rescan), or
     at the engine's next start;
  2a. ✓ done the same day (log entry "the app's image in the update");
  3. ✓ an app older than the engine says so (a notice) and offers "Update the app";
     the requests it needs for that (`/api/info` version, the update
     requests) must keep working in every later engine;
  4. the console test fakes a lower local version (as with the payload),
     and needs a release after the first one that carries the image.
  ✓ Released as 0.3.0 (2026-10-25): `.ffpkg` and store ZIP in the release;
  the catalog record from `tools/catalog_record.py v0.3.0` is ready for a PR
  to darkxex/ps5-homebrew-catalog (owner opens it from fthdgn-gh).
  Console tests still to do: the app from the `.ffpkg`, Restart the engine,
  an app without its engine downloading it, and (after the next release)
  the app's image being replaced by an update. Then: game icons in the
  preview's stand-in engine (PNG), ✓ Japanese in the app (2026-11-03, IBM
  Plex Sans JP, see the log), local time for dates.
0. ✓ Updating itself, tried on the console through Payload Manager
   (2026-10-24).
1. Owner's console test: install an Alembic-made FPKG.
2. ✓ zlib read-ahead for FFPFSC inputs, hardware SHA-256, CPU instructions
   on every x86-64 PC (2026-10-22).
3. ✓ Windows build (2026-10-22). Owner: try the .exe files on a PC.
4. **Working across engines** (owner): source, converting engine and target
   each any engine on the network. Steps 1-4 (discovery, pairing, peer files
   as inputs and outputs, choosing the converting engine) done 2026-10-22;
   ✓ owner: the console and a PC pair and work together (2026-10-24).
   ✓ owner: an FPKG on the PC converted by the PC engine into an FFPFSC
   written to the console works, started from the console app
   (2026-10-25).
5. Smaller: retail/patch packages, NAPS shuffle layouts, less pass-1
   memory for LZ4. ✓ Kraken-compressed FPKG output (2026-10-31); owner:
   install one on the console. Possible later: LibProsperoPKG's stronger
   encoder (optimal parse) as a "smallest" mode, with no byte oracle.
7. **Payload limits, parked (owner, 2026-10-27).** The engine runs in its
   own process from the ELF loader (spawned via `SceRedisServer`), so the
   app's `param.json` (`kernel.flexibleMemorySize`, the PS5 Pro memory bit
   in `attribute3`) does not reach it. Unknown on the console: the cores the
   process may use (a game reserves most), its priority, its memory budget.
   Idea: a diagnostics readout (CPU affinity, priority, memory used/free, a
   throughput test with 1..N threads), run with and without a game, before
   capping workers or changing priorities.
6. PS4 packages (2026-10-26, reading and writing done, byte-identical to
   LibOrbisPkg; images start on the console with ShadowMount+ 1.7beta8).
   **Next version: PS4 updates** (owner, 2026-10-26: not in 0.3.2). Plan:
   - group by param.sfo TITLE_ID and the content ID's region; an update has
     a patch bit in CNT 0x78 (first/subsequent/cumulative/delta, PatchGo)
     and CATEGORY `gp`; order by APP_VER as numbers;
   - one "game + updates" tree: update files replace the base's at the same
     path (ignoring case), new ones are added, nothing is removed; the
     update's sce_sys wins (param.json made again from the merged SFO);
   - refuse delta updates (binary diffs), title or region mismatches, two
     different updates of one version;
   - outputs: base + updates into one image (updates found next to the
     base, listed in Convert, ticked); an existing image + update into a new
     image; a folder patched in place; one merged PS4 package;
   - first: get a real update package to check the reader opens it;
   - untried: installing the update package on top of an image-run game
     (ShadowMount+ knows /user/patch).
   Not planned for now (owner): passcodes other than all zeros. Later: DLC,
   zlib-compressed PFSC like Sony's packages, hiding the AMPR switch in
   Convert for PS4 games.

8. **Network servers (2026-10-30, 0.3.3):** ✓ SMB from the console
   (owner). Still to try: a Windows share and a NAS over SMB, NFS on a
   NAS (with a portmapper and "secure" exports), WebDAV.
   **Next, as its own piece of work (owner): encryption** - ✓ SMB 3
   encryption (2026-10-31, when required); ✓ TLS (FTPS, HTTPS WebDAV,
   2026-10-31, mbedTLS on every platform instead of libSceSsl, see the log).
   Still possible: an "always encrypt" SMB option, SFTP. Decided
   (owner, 2026-10-31): TLS through the console's libSceSsl on the PS5 and
   vendored mbedTLS (Apache-2.0, 3.6 LTS) on PCs; servers' certificates
   pinned on first use (the fingerprint shown and confirmed once, a change
   refused with a clear message). SMB 3 encryption written from the
   standards like the rest of netcrypto. Also possible:
   outputs written in place on SMB and NFS (both write at any offset, so
   FFPFSC and PKG could go straight there), SFTP (needs SSH crypto).

Older plan items (faster reading, PS5 build, LZ4 packs, FPKG, UFS2 and PFS
writers, the downloader for third-party files) are done; see the log above.

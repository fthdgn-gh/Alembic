# Alembic

Alembic converts PS5 games between the formats homebrew loaders use: debug
packages (FPKG), dump folders, exFAT images, FFPKG (UFS2), FFPFS (PFS) and
FFPFSC (compressed PFS). It runs **on the console** as a payload with a web
page you drive with the controller, and on a PC as a command-line tool.

> Work in progress. Back up anything you convert until you have checked the
> result on your console.

## What it does

| From ↓ / To → | Folder | exFAT | FFPKG (UFS2) | FFPFS (PFS) | FFPFSC | FPKG |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Dump folder | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFSC | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FPKG (debug) | ✓ | ✓ | ✓ | ✓ | ✓ | – |

- **Streams:** every input is read as a file tree and written straight to
  the output, with no temporary copies, so a 100 GB package needs only the
  space of the result.
- **Byte-identical** to the reference tools: exFAT, FFPKG and FFPFSC match
  [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool); FFPFS matches
  [MkPFS](https://github.com/PSBrew/MkPFS) (`pack folder --raw
  --no-compress`); FPKG output matches
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)
  (`img_create`, stored, with the same fixed seed).
- **LZ4 asset packs** for drakmor's [ampr_emu](https://github.com/drakmor/ampr_emu)
  PackedStdio runtime (`--lz4`), byte-identical to its `ampr_pack.py`, with
  game profiles from
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression).
- **AMPR emulator:** games that use the console's APR file reader need
  ampr_emu's `libSceAmpr.sprx` to start from an image or folder. Alembic
  adds it when converting, or to a game you already have.
- **Fast:** compression and package decoding use every core; zlib level 6
  by default (level 9 for exact PS5PKGTool output).

## On the PS5

The engine (`alembic.elf`) runs as a payload and keeps working after you
close the browser. Its page opens in the console's browser, from a shortcut
it adds to the Media tab, or from any browser on your network at
`http://<console-ip>:5940`.

1. Send `alembic-<version>.elf` (from the releases) to your console's ELF loader.
2. Open Alembic from the Media tab (or the address above).
3. In **Files**, pick a game on internal storage, USB or an FTP server, then
   **Convert**, **Install** (packages), **Copy** or **Move**.
4. Jobs run one at a time in **Queue**: pause, resume (long jobs continue
   from a checkpoint), reorder, cancel. You get a notification when one
   finishes.

Also on the page:

- **Controller first:** D-pad, ✕/○, L1/R1, L2/R2 and △/□ all work in the
  console's browser; a controller test is in Settings.
- **FTP servers** (Settings): convert straight from a PC or NAS without
  copying the game to the console first. exFAT, FFPKG, FFPFS and folder
  outputs can be saved back to the server.
- **From this computer:** opened in a PC's browser, the page can share a
  file from that PC for the console to convert.
- **ShadowMount+:** new images can be picked up without a restart, and
  sources can be hidden from it once converted.
- **Components:** third-party files Alembic needs but doesn't ship (ampr_emu's
  runtime, ProsperoPkgTool's FPKG key files) are downloaded on request from
  their official source and kept only if their SHA-256 matches.
- Languages: English, Español, Français, Deutsch, 日本語, Türkçe.

State (queue, settings, downloaded components) is kept in `/data/alembic`.

## On a PC

```sh
alembic convert game.pkg game.ffpfsc              # format from the extension
alembic convert game.exfat game/ --to folder      # unpack to a folder
alembic convert game/ game.exfat --lz4 --lz4-runtime libSceAmpr.sprx
alembic ls game.ffpkg                             # list the files of any input
alembic same game.pkg game.ffpfsc                 # compare two inputs file by file
alembic --help                                    # every command and option
```

`alembic-engine` is the same engine as the payload, with the same web page
(`http://localhost:5940`), for converting on a PC.

## Building

Requirements: CMake 3.20+, a C++20 compiler, zlib (bundled for the PS5).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/alembic_tests
```

The PS5 payload needs LLVM 18 and
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.44:

```sh
# macOS: brew install llvm@18 · Debian/Ubuntu: apt install clang-18 lld-18 llvm-18
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # default: .refs/ps5-payload-sdk
tools/ps5/build.sh                                # → build-ps5/alembic.elf
```

GitHub Actions builds and tests everything on every push (see
[.github/workflows/build.yml](.github/workflows/build.yml)). Each run has the
payload as a download of its own, `alembic-<version>.elf`, and a bundle,
`alembic-<version>.zip`: the payload in `ps5/`, `alembic` and
`alembic-engine` in `linux-x64/` and `macos-arm64/`, with the README and
license files. The version is the tag (`v0.1.0`), else the branch and commit
(`main-a2fab12`). A `v*` tag publishes the payload, the bundle and
`SHA256SUMS` as a release.

### Checking against the reference tools

The scripts in `tools/` compare Alembic's output byte for byte with the tools
it follows. They need the reference projects checked out in `.refs/`
(git-ignored); [docs/NOTES.md](docs/NOTES.md) says how to recreate it.

```sh
tools/oracle/compare.sh        # exFAT, FFPKG, FFPFS, FFPFSC vs PS5PKGTool and MkPFS
tools/pkg-oracle/compare.sh    # FPKG reading vs packages built by ProsperoPkgTool / LibProsperoPKG
tools/lz4-oracle/compare.sh    # LZ4 packs vs ampr_emu's ampr_pack.py
```

## What Alembic does not include

- No Oodle, `libScePubTools`, closed-source package libraries or console keys.
  FPKG output uses ProsperoPkgTool's published key files, which the engine
  downloads on request.
- No third-party binaries: ampr_emu's `libSceAmpr.sprx` is downloaded from its
  official release, pinned by SHA-256.

## License

GPL-3.0-or-later ([LICENSE](LICENSE)). Alembic ports code from several GPL,
MIT and BSD projects; [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists
each one, what was taken from it, and its license.

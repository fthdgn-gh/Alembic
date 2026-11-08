# Alembic

**English** · [Türkçe](README.tr.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Magyar](README.hu.md) · [Русский](README.ru.md) · [日本語](README.ja.md) · [简体中文](README.zh.md)

![Alembic's page in the PS5 browser: a library of games in several formats, with drives, a NAS share, an NFS export, an FTPS server and a paired PC on the left](docs/screenshots/files.png)

Alembic converts PS5 and PS4 games between the formats homebrew loaders use:
packages (PS5 debug FPKG, PS4 fake PKG), dump folders, exFAT images, FFPKG
(UFS2), FFPFS (PFS) and FFPFSC (compressed PFS). It runs **on the console** as an app on the home
screen (from 0.3.0) or as a payload with a web page, both driven with the
controller, and on a PC (Windows, macOS, Linux) as a command-line tool or the
same engine and page. Engines on your network can
pair and work together: convert a package that sits on your PC, on the PC's
processor, straight into the console's storage. Games on a PC or NAS can
also be read from and saved to over SMB, NFS, WebDAV or FTP.

> Work in progress. Back up anything you convert until you have checked the
> result on your console.

**Tried on a console so far:** FFPFSC images made from dumps and from
packages run through ShadowMount+; games that need the AMPR emulator start
once it is added; the console and a PC pair and convert for each other;
**Install** hands a debug package to the console's installer, including
packages Alembic builds from a dump (stored or Kraken, with a generated
debug licence, so no lock on the title); a paused
conversion carries on after the engine is updated; the engine updates
itself from a release (loaded through Payload Manager); the console app
starts from ShadowMount+, starts and restarts the engine, and drives a
conversion on a paired PC; a package converted from an SMB share; a game
in zstd packs (a 160 GB package packed at level 5 by a PC engine into a
96 GB exFAT written to the console) starts with ampr-emu-ng.
PS4 games converted from their packages start from images through
ShadowMount+ 1.7beta8 (it shows "Trophy registration failed" for them: a
ShadowMount+ issue, [#97](https://github.com/drakmor/ShadowMountPlus/issues/97)).
**Not yet:** NFS, WebDAV, SMB encryption, FTPS and HTTPS from a console,
and real NAS boxes and Windows shares (all of them were checked on a PC
against Samba, rclone, WsgiDAV and pyftpdlib); starting a game installed
from an Alembic package (installs were tried, starting it was not); a PS4
package Alembic built; PS4 updates (patch packages) and DLC. LZ4 asset packs work for some games and not for
others.

## What it does

| From ↓ / To → | Folder | exFAT | FFPKG (UFS2) | FFPFS (PFS) | FFPFSC | Package |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Dump folder | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFSC | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| PS5 FPKG (debug) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |
| PS4 PKG (fake) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |

The package a game becomes matches the game: a PS5 game (`sce_sys/param.json`)
a PS5 debug FPKG, a PS4 game (`sce_sys/param.sfo`) a PS4 fake PKG. The page
and the app mark each game with a **PS5** (white) or **PS4** (black) pill.

- **PS4 games** (from 0.3.2): fake PKGs with the usual all-zero passcode
  (from older and newer packaging tools alike) open
  like PS5 packages, as the game's files plus `sce_sys` (`param.sfo`, icons,
  trophies, licence) from the package, and convert to any image. Images and
  folders of a PS4 game get the `sce_sys/param.json` ShadowMount+ needs to
  start it (made from `param.sfo`, as
  [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc) makes
  it); tried on a console with ShadowMount+ 1.7beta8. A PS4 game
  in any format converts back to a PS4 fake PKG, built the way
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)'s `pkg_build` builds
  one: the game's licence is kept (generated only when missing), and the
  PlayGo files are made again. A package rebuilt from any image of it is the
  same, byte for byte. The keys come from LibOrbisPkg (Components, or
  `--ps4-keys FILE` on a PC). The AMPR emulator and LZ4 packs are for PS5
  games only. Which console a game is for comes from its title ID (`CUSA`,
  `PPSA`), else from the SDK its `eboot.bin` was built with (homebrew);
  `alembic game-info` shows both. Not yet: updates and DLC, and packages
  made with a passcode other than all zeros.
- **Streams:** every input is read as a file tree and written straight to
  the output, with no temporary copies, so a 100 GB package needs only the
  space of the result.
- **Byte-identical** to the reference tools: exFAT, FFPKG and FFPFSC match
  [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool); FFPFS matches
  [MkPFS](https://github.com/PSBrew/MkPFS) (`pack folder --raw
  --no-compress`); FPKG output matches
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)
  (`img_create`, stored or `--compression kraken`, with the same fixed
  seed); PS4 packages match
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)'s `pkg_build`, also
  when rebuilding a package from the project its `pkg_makegp4` extracts.
- **LZ4 asset packs** for drakmor's [ampr_emu](https://github.com/drakmor/ampr_emu)
  PackedStdio runtime (`--lz4`), byte-identical to its `ampr_pack.py`, with
  game profiles from
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression).
- **zstd packs** for quer3q's [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng)
  (`--zstd`, or "Pack into zstd packs" when converting), byte-identical to
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge)'s: smaller than
  LZ4 and faster to make (a 26 GiB set of game files became 11.1 GiB in
  33 s on a Mac, against 17.5 GiB in 77 s for LZ4). The game gets
  ampr-emu-ng's `libSceAmpr.sprx` (from Components); turn off ShadowMount+'s
  "update emulators" so it isn't swapped for one that can't read them.
  Levels 1, 5 (default) and 19. Tried on a PS5. A packed game can switch
  between LZ4 and zstd packs in Convert ("Switch to zstd packs"), the old
  packs decoded straight into the new ones.
- **AMPR emulator:** games that use the console's APR file reader need
  an emulator's `libSceAmpr.sprx` to start from an image or folder:
  drakmor's ampr_emu or quer3q's ampr-emu-ng (Components says which is in
  use). Alembic adds it when converting, or to a game you already have;
  on a game that has one, "Add the AMPR emulator" offers to switch it to
  the other.
- **Fake-signing:** a PS5 game whose executables are plain (unsigned) ELF
  files never starts. Conversions turn them into fake-signed SELFs on the
  way, and Convert says so beforehand: for an image or folder
  byte-identical to ampr-emu-ng's `make_fself.py`, for a package to
  ProsperoPkgTool's `img_create`.
- **Fast:** compression and package decoding use every core; AES, CRC32C
  and SHA-256 use the CPU's own instructions where it has them; zlib level 6
  by default (level 9 for exact PS5PKGTool output).

## On the PS5

The work is done by the engine (`alembic.elf`), a payload that keeps running
after you close what you drive it with: the **Alembic app** on the home
screen, or its web page in the console's browser (from a shortcut it adds to
the Media tab) or in any browser on your network at `http://<console-ip>:5940`.
The app and the page show the same things and work the same way.

![The Alembic app on the PS5: a homebrew folder with games in several formats, drives, a NAS share, an NFS export and a paired PC on the left](docs/screenshots/app.png)

**The app** (`PPSA99594`, from 0.3.0), installed either way:

1. **From ProsperoStore** (easiest): Alembic is listed in
   [ProsperoStore](https://github.com/darkxex/ProsperoStore), the console
   store for native homebrew ([Alembic's page](https://prosperostore.net/app/PPSA99594/)).
   Install it there; the store also updates it.
2. **As an image or folder**, through
   [ShadowMount+](https://github.com/drakmor/ShadowMountPlus): copy
   `alembic-app-<version>.ffpkg` from the releases to `/data/homebrew/` (or
   unzip `alembic-app-<version>.zip` there: the `PPSA99594` folder). Its tile
   shows up on the home screen after ShadowMount+'s next scan.

Start it. When no engine answers, it sends its own to the ELF loader on port
9021 (an app without one downloads the engine of its own version from the
GitHub release first, checked against `SHA256SUMS`).

**Without the app:**

1. Send `alembic-<version>.elf` (from the releases) to your console's ELF loader.
2. Open Alembic from the Media tab (or the address above).
3. In **Files**, pick a game on internal storage, USB, a network server (SMB,
   NFS, WebDAV or FTP) or a paired engine's drive (a pill tells PS5 and PS4
   games apart), then
   **Convert**, **Install** (packages), **Copy** or **Move**. Converting a
   PS5 game to a package can hand it to the console's installer when done
   (**Install when done**); the package is then deleted once the installer
   has read it, unless **Keep the package** is on, so it needs room until
   then.
4. Jobs run one at a time in **Queue**: pause, resume (long jobs continue
   from a checkpoint), reorder, cancel. You get a notification when one
   finishes.

![The Convert dialog: target formats, compression level, the LZ4 and zstd pack switches, the AMPR emulator switch, and the output path on a paired PC](docs/screenshots/convert.png)

![The queue: a running conversion with speed and time left, a paused one that continues from its checkpoint, a copy to a NAS share, and a job running on the paired PC](docs/screenshots/queue.png)

Also on the page:

- **Controller first:** the D-pad moves, ✕ chooses, ○ goes back, □ opens a
  file's options, L1/R1 switch sections and L2/R2 page through lists. To
  leave, press L2 + R2 together, then ○ (a page cannot close the console's
  browser itself). Settings has a controller test. On a PC: arrow keys,
  Enter, Esc, S, Q/E and Page Up/Down.
- **Places:** add any folder to Places from its Options; they show in the
  rail, in Save to, and on paired engines.
- **Rename** (from 0.4.1): renaming a game offers names made from its title,
  title ID, version and format (`Title [ABCD12345].ffpfsc`, …) next to the
  current one; the one picked can still be edited. The formats are yours to
  change in **Settings → Rename formats**: fields `{title}` `{titleid}`
  `{contentid}` `{version}` `{format}` `{platform}` `{name}` (or
  `[titleid]`), modifiers such as `{title:upper}`, `{title:20}` or
  `{version:short}`, `<…>` for a part left out when a value is missing, and
  per format what to do with spaces (keep, remove, `_`, `.`, `-`) and
  letters (as they are, UPPERCASE, lowercase). A name too long for
  ShadowMount+ to mount (58 bytes before `.ffpfsc`, 63 before `.exfat`,
  `.ffpkg` and `.ffpfs`) is flagged and followed by a shortened one, and a
  typed one is asked about. A hidden name (`.name`) stays hidden.
- **Several files at once** (from 0.4.2): □ on a file → **Select…**, then ✕
  ticks rows, □ opens the actions for the ticked ones and ○ ends selecting.
  Convert (one job per game, a game already in the target format left
  out), Install, Add the AMPR emulator, Hide from or Show to ShadowMount+,
  Rename with a format (old → new shown first), Send to another engine,
  Copy, Move and Delete; each action takes the files it applies to.
- **Packs and hiding** (from 0.5.0): games in asset packs carry an LZ4 or
  ZSTD tag after their format. **Hide** / **Unhide** (renaming to `.name`
  and back) work on any file or folder, packages included; images keep
  Hide from / Show to ShadowMount+, which also rescan.
- **Network servers** (Settings, from 0.3.3): games on a PC or NAS can be
  converted without copying them to the console first, over
  - **SMB** (Windows shared folders, most NAS boxes): SMB 2 and 3 with
    signing, as Windows 11 requires, and SMB 3 encryption (AES-128-GCM or
    -CCM) when the server or share requires it; a user and password, or a
    guest login. Leave the share empty to see all of the server's shares.
  - **NFS** (NAS and Linux exports): NFSv3, with the uid:gid to use
    instead of a password. Leave the export empty to see all of them.
  - **WebDAV** (NAS, Nextcloud, rclone): HTTP or HTTPS, Basic or Digest
    login.
  - **FTP**: FTP or FTPS (explicit, or implicit on port 990).

  The server only stores the files: the engine reads the game over the
  network and does the converting itself. exFAT, FFPKG, FFPFS and folder
  outputs can be saved back to the server; FFPFSC and PKG outputs go to the
  console first. With **Encrypt with TLS** on, FTP and WebDAV go over
  TLS 1.2 or 1.3 (mbedTLS). Home servers mostly have self-signed
  certificates, so Alembic trusts a server's certificate on first use, as
  SSH does: the first connection shows its SHA-256 fingerprint to compare
  with the server's and asks; later connections must show the same one,
  and a changed certificate is refused with a warning. To have a PC do the
  work, run Alembic's engine on it instead (see
  [Working across engines](#working-across-engines)).
- **From this computer:** opened in a PC's browser, the page can share a
  file from that PC for the console to convert.
- **ShadowMount+:** new images can be picked up without a restart, and
  sources can be hidden from it once converted.
- **Images:** every image, game folder and package in the folders
  ShadowMount+ scans (hidden ones too), grouped by folder, as a grid of game
  icons or a list (the switch above it; Files has the same switch and starts
  as a list). It offers what Files does except moving and copying to
  folders: Convert, Install, Rename, the AMPR emulator, Hide from
  ShadowMount+, Send to another engine, Delete, Select for several at once,
  and **Show in Files**, which opens the folder it is in. For games that use
  the APR it shows the AMPR emulator each one runs with (in the game or its
  backport) and whether a newer build is out; **Update AMPR emulators** puts
  the newest build of each game's own emulator in place, or the one in use
  where a game has none. Every official build of both emulators is
  recognised by the SHA-256 GitHub publishes for it.
  Packages apps download to `/data/downloads` (in any folder below it)
  are listed too; on a PC, those in the system's Downloads folder.

  ![Images: the games in ShadowMount+'s folders as a grid, two AMPR emulators to update, one game without one and one with a build Alembic doesn't know](docs/screenshots/images.png)
- **Components:** third-party files Alembic needs but doesn't ship (the AMPR
  emulators ampr_emu and ampr-emu-ng, ProsperoPkgTool's FPKG key files,
  LibOrbisPkg's licence and PS4 package keys) are downloaded on request from
  their official source and kept only if their SHA-256 matches.
  The two AMPR emulators follow their projects' latest releases: the engine
  checks GitHub at start (or on **Check for updates**) and, with **Update
  automatically** on (the default), downloads a newer build, checked against
  the SHA-256 GitHub and the release's `SHA256SUMS` give for it. One emulator
  is **in use**: the one conversions and "Add the AMPR emulator" put in a
  game. A game keeps the build it got; "Add the AMPR emulator" on it offers
  to replace it with the one in use. ampr-emu-ng also reads its own zstd packs.
  **Offline:** download them on a PC from the links below (for the
  emulators, the builds Alembic was last tested with; any later release of
  the same project works too) and copy them to the console under
  these exact names; the engine picks them up without a restart:

  | Component | Download | Put it at | SHA-256 |
  |---|---|---|---|
  | AMPR emulator (ampr_emu 0.4.2.1) | [libSceAmpr.sprx](https://github.com/drakmor/ampr_emu/releases/download/0.4.2.1/libSceAmpr.sprx) | `/data/alembic/runtime/libSceAmpr.sprx` | `69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a` |
  | AMPR emulator (ampr-emu-ng 0.2.0) | [libSceAmpr.sprx](https://github.com/quer3q/ampr-emu-ng/releases/download/v0.2.0/libSceAmpr.sprx) | `/data/alembic/runtime/ampr-emu-ng/libSceAmpr.sprx` | `7acb2173d9691b5dfb94ea4007660920b7250111c5fd7a86c6eb91b763ad3d76` |
  | FPKG key files | [passcode.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/passcode.bin) | `/data/alembic/keys/passcode.bin` | `81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e` |
  | | [mount_image.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/mount_image.bin) | `/data/alembic/keys/mount_image.bin` | `4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0` |
  | | [metadata_modulus.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/metadata_modulus.bin) | `/data/alembic/keys/metadata_modulus.bin` | `4a585ee47fea88d6b121d5e8afc9829438a27ee325cebec56a2ff537129a37c5` |
  | Licence and PS4 package keys | [Keys.cs](https://raw.githubusercontent.com/maxton/LibOrbisPkg/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/Util/Keys.cs) | `/data/alembic/keys/ps4/Keys.cs` | `48b44dd03e653a5c0991b0a2975477802ea2639c4e6ea3270b2ff20a1856f8a7` |

  Key files must be the pinned ones (same SHA-256), or package output
  refuses them: they are constants, so they don't follow anything. An
  emulator build copied by hand still works but shows as "changed" in
  Components until the engine downloads one itself.
- **Engine** (Settings): **Restart the engine** starts the same version again
  through the ELF loader; the app also offers **Start the engine** once it
  has stopped.
- **Updates** (from 0.2.3): the engine looks for a newer release on GitHub
  when it starts and says so; **Settings → Updates** shows its notes.
  **Update** downloads the payload, checks it against the release's
  `SHA256SUMS` and the version written inside it, and replaces the copies
  your loader starts it from: Payload Manager's (renamed to the new version,
  with the name fixed in that payload's `meta.json`) and `ps5_autoloader`
  folders (same name); **Search the drives** finds copies elsewhere. Then
  the new engine starts through the ELF loader (port 9021) and takes over;
  a running job that keeps its progress continues. Sending a newer
  `alembic.elf` by hand also replaces a running engine. From 0.3.0 the same
  update replaces the app's `.ffpkg` too when the release's differs; while
  the app is open, that waits until it is closed (an app installed from
  ProsperoStore is updated by the store). An app older than its engine says
  so and offers **Update the app**.
- **Other engines:** a paired engine that is switched off stays listed as
  offline instead of disappearing.
- **Busy drives:** the app keeps the queue up to date while game titles
  and icons load from a drive that a running job keeps busy.
- Languages: English, Türkçe, Deutsch, Español, Français, Magyar, Русский, 日本語, 简体中文
  (Magyar translated by isty1974).

State (queue, settings, downloaded components) is kept in `/data/alembic`.

## Working across engines

Run `alembic-engine` on a PC as well as the payload on the console. Once
paired, each engine can read and write the other's files and hand it jobs.

1. In **Settings → Other engines**, switch on **Find engines on this
   network** on both (off by default), or add one by its address.
2. Press **Pair** next to the other engine. It shows a six-digit code
   (on the PS5 as a notification, on a PC in its window); type it in.
   Paired engines sign every request with the key they agreed on.
3. The other engine's drives and Places appear in **Files** under
   *Other engines*, as inputs and as places to save to.
4. In **Convert**, **Convert on** picks the engine that does the work (of
   those online right now). The
   source, the converting engine and the output can each be on a different
   engine; data streams between them, with no copy made first.

A job shows in the queue of every engine whose files it uses ("on Living
room PC"), and can be paused, resumed, cancelled or cleared from the history
on any of them. When a job deletes its source after checking the output,
engines compare SHA-256 digests instead of sending the files back. Files can also be sent between
engines without converting (Options → Send to another engine).

![Settings: a paired PC and a Mac found on the network, and two network servers: a NAS share over SMB and an NFS export](docs/screenshots/engines.png)

Engines of different Alembic versions work together as long as they talk to
each other the same way; when that changes in a release, Settings shows which
engine to update, and they refuse each other until it is.

The page itself has no password: anyone on your network can open it. Start
a PC engine with `--listen 127.0.0.1` to keep it to that computer; other
engines can't reach it then.

## On a PC

Each release has a package per system: `alembic-<version>-linux-x64.tar.gz`,
`-macos-arm64.tar.gz` (Apple silicon) and `-windows-x64.zip`, each with
`alembic` and `alembic-engine`. The macOS builds are not signed: after a
download from a browser, run `xattr -d com.apple.quarantine alembic
alembic-engine` once in their folder, or macOS refuses to start them.

```sh
alembic convert game.pkg game.ffpfsc              # format from the extension (PS5 or PS4 package)
alembic convert ps4game.exfat game.pkg --ps4-keys Keys.cs   # a PS4 game back to a PS4 fake PKG
alembic convert game.exfat game/ --to folder      # unpack to a folder
alembic convert game/ game.exfat --lz4 --lz4-runtime libSceAmpr.sprx
alembic convert game/ game.exfat --zstd --zstd-runtime libSceAmpr.sprx   # ampr-emu-ng's
alembic ls game.ffpkg                             # list the files of any input
alembic same game.pkg game.ffpfsc                 # compare two inputs file by file
alembic game-info game.exfat                      # title ID, SDK and PS4 or PS5
alembic --help                                    # every command and option
```

`alembic-engine` is the same engine as the payload, with the same web page
(`http://localhost:5940`), for converting on a PC or pairing with the
console. `--root DIR` (repeatable) chooses the folders it shares (your
home folder by default); `alembic-engine --help` lists the other options.
Settings → Updates says when a newer release is out; on a PC, download it
and replace the files yourself.

## Building

Requirements: CMake 3.20+, a C++20 compiler, zlib (bundled for the PS5), and
Node.js 20+ with npm for the engine's web page (`src/engine/web`, Preact and
TypeScript bundled by esbuild; the first build runs `npm ci` there).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/alembic_tests
```

Windows (`alembic.exe`, `alembic-engine.exe`) is cross-built with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) from macOS or Linux; the
executables are statically linked and run on Windows 10 and later as they
are. `tools/windows/smoke.sh` checks them on Windows (Git Bash).

```sh
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=/path/to/llvm-mingw
cmake --build build-win -j
```

The PS5 payload needs LLVM 18 and
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.44:

```sh
# macOS: brew install llvm@18 · Debian/Ubuntu: apt install clang-18 lld-18 llvm-18
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # default: .refs/ps5-payload-sdk
tools/ps5/build.sh                                # → build-ps5/alembic.elf
```

GitHub Actions builds and tests everything on every push that changes more
than the docs (see [.github/workflows/build.yml](.github/workflows/build.yml)).
Each run has the payload as a download of its own, `alembic-<version>.elf`,
and a bundle, `alembic-<version>.zip`: the payload in `ps5/`, `alembic` and
`alembic-engine` in `linux-x64/`, `macos-arm64/` and `windows-x64/` (`.exe`),
with the README and license files. The version is the tag (`v0.1.0`), else the branch and commit
(`main-a2fab12`). A `v*` tag publishes the payload, the bundle, a package per
PC system, the console app (`.ffpkg` and `.zip`) and `SHA256SUMS` as a release.
From 0.3.1 every release file carries a build attestation: `gh attestation
verify <file> --repo fthdgn-gh/Alembic` shows that this repository's workflow
built it, and from which commit.

### The web page

The engine's page is `src/engine/web`: Preact components in TypeScript
(`src/pages/` has one folder per section; `src/main.tsx` maps the rest),
bundled for the PS5's older browser into one HTML file that the engine
embeds. To work on it, run an engine and let the page rebuild on every
change:

```sh
build/alembic-engine --listen 127.0.0.1 &   # its API, on port 5940
cd src/engine/web
node build.mjs --serve                      # the page at http://127.0.0.1:5950
npx tsc --noEmit                            # type check (CI runs it too)
python3 ../../../tools/i18n_keys.py --check # every text translated
```

`tools/screenshots/shoot.sh` makes the README's screenshots against a
stand-in engine with made-up games.

### Checking against the reference tools

The scripts in `tools/` compare Alembic's output byte for byte with the tools
it follows. They need the reference projects checked out in `.refs/`
(git-ignored); [docs/NOTES.md](docs/NOTES.md) says how to recreate it.

```sh
tools/oracle/compare.sh        # exFAT, FFPKG, FFPFS, FFPFSC vs PS5PKGTool and MkPFS
tools/pkg-oracle/compare.sh    # FPKG reading and writing vs ProsperoPkgTool / LibProsperoPKG
tools/pkg-oracle/write_samples.sh  # FPKG writing vs ProsperoPkgTool on your own packages
tools/lz4-oracle/compare.sh    # LZ4 packs vs ampr_emu's ampr_pack.py
tools/zstd-oracle/compare.sh   # zstd packs vs ps5-dump-forge (builds it with cargo)
tools/fself-oracle/compare.sh  # fake-signed SELFs vs ampr-emu-ng's make_fself.py
tools/ps4-oracle/compare.sh    # PS4 packages vs LibOrbisPkg's pkg_build, pkg_extract and pkg_validate
```

## What Alembic does not include

- No Oodle, `libScePubTools`, closed-source package libraries or console keys
  in this repository or its binaries. FPKG output uses ProsperoPkgTool's
  published key files; PS4 package output uses LibOrbisPkg's `Keys.cs` (it
  also holds the debug licence key, used only to generate a licence for a
  game that has none, PS5 or PS4). The engine downloads both on request.
- No third-party binaries: the AMPR emulators' `libSceAmpr.sprx` is
  downloaded from each project's official GitHub release, checked by the
  SHA-256 published with it.

## Credits

Alembic stands on the work of the PS5 homebrew scene. Thank you to:

- **pearlxcore** for [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool)
  (exFAT, PFSC and FFPFSC; the reference Alembic matches byte for byte) and
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool) (reading
  and writing debug packages, and its key files).
- **SadykovIV** for [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc)
  (how a PS4 game's image has to look for ShadowMount+, its `param.json`).
- **SvenGDK** for [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) (the FFPKG
  reader and writer) and [LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG)
  (test packages).
- **drakmor** for [ampr_emu](https://github.com/drakmor/ampr_emu) (the AMPR
  index, LZ4 asset packs and the runtime that reads them) and
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (the loader
  Alembic's images are made for, and its HTTPS download code).
- **quer3q** for [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng) (the
  second AMPR emulator, and its zstd packs) and
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge) (the zstd pack
  writer Alembic follows, and checks against).
- **PSBrew** for [MkPFS](https://github.com/PSBrew/MkPFS) (the PFS writer).
- **maxton** for [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) (PS4
  packages: the writer Alembic follows, its keys, and the tool it is checked
  against).
- **theghostonline** for
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)
  (LZ4 pack profiles and the engine-and-page idea).
- **danthespal** for the LZ4 game profiles posted with
  [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp).
- **ps5-payload-dev** for the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk),
  [shsrv](https://github.com/ps5-payload-dev/shsrv) (package installs) and
  [websrv](https://github.com/ps5-payload-dev/websrv) (the Media-tab shortcut),
  with [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
  and [itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager).
- **blackbearreloaded** for [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
  the design the page follows.
- Microsoft's [open specifications](https://learn.microsoft.com/openspecs/)
  (SMB2/3, NTLM, SPNEGO, DCE/RPC and the share list) and the IETF RFCs for
  NFSv3, ONC RPC and WebDAV, which the network clients are written from.
- [zlib](https://zlib.net), [LZ4](https://github.com/lz4/lz4) and
  [Zstandard](https://github.com/facebook/zstd), bundled
  with the engine, and [Preact](https://github.com/preactjs/preact) with its
  [signals](https://github.com/preactjs/signals), bundled into its page.

The games, engines and addresses in the screenshots are made up
(`tools/screenshots/shoot.sh` renders them against a stand-in engine).

## License

GPL-3.0-or-later ([LICENSE](LICENSE)). Alembic ports code from several GPL,
LGPL, MIT and BSD projects; [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists
each one, what was taken from it, and its license.

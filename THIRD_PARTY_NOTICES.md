# Third-party notices

Alembic is licensed under the GNU General Public License v3.0 or later
(`LICENSE`). Parts of it are ported from, or follow, the projects below.
Ported files say where they come from in their header comment.

## Code ported from other projects

| Project | License | What Alembic uses |
| --- | --- | --- |
| [pearlxcore/PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool) | GPL-3.0 | exFAT image writer and reader, PFSC codec, FFPFSC image, AMPR index (`src/core/exfat_*.cpp`, `pfsc.cpp`, `ffpfsc.cpp`, `ampr.cpp`); the exFAT up-case table in `src/core/data/exfat_upcase_table.inc` |
| [pearlxcore/ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool) | MIT (notice below) | FPKG reading: package header/CNT parsing, key derivation, outer PFS reading |
| [SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool), as shipped in PS5PKGTool | BSD-2-Clause (notice below) | UFS2 reader (`src/core/ufs2.cpp`): superblock field offsets and validation rules; UFS2 writer (`src/core/ufs2_image.cpp`), ported from `Ufs2ImageCreator.cs` and `Ufs2Operations.CreateFromDirectoryAsync` |
| [drakmor/ampr_emu](https://github.com/drakmor/ampr_emu) | GPL-3.0 | AMPRIDX3 hashing, sorting and exclusion rules (`ampr::Scheme::ampr_emu`), following `tools/build_ampr_index.py` and `src/ampr_emu_index.cpp`; LZ4 asset packs (AMPRPAK4/AMPRDAT3/AMPRCRC1/AMPRCFG1 format, packer, verifier) in `src/core/lz4pack_*.cpp`, ported from `tools/ampr_pack.py` and `tools/ampr_pack_format.py` |
| [theghostonline/PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression) | GPL-3.0-or-later | the automatic LZ4 pack profile, its safety list, the keep-on-disk list and the runtime cache budget (`lz4pack::automatic_profile`, `src/core/lz4pack_build.cpp`), from `src/ampr_pack_profile.c` and `src/ampr_pack.c` only (not its `src/pfs_*` code); how bundled profiles are matched to a game and how a TOML profile is applied (`src/core/lz4pack_profile.cpp`, from `src/lzc_api.c` and `src/ampr_pack_profile.c`); the TOML subset `src/core/toml.cpp` reads (from `src/ampr_pack_toml.c`) |
| [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) | GPL-3.0 | the PFS (.ffpfs) writer `src/core/pfs_image.cpp`, ported from `mkpfs/pfs.py` (`build_pfs` unsigned/uncompressed path, `scan_source_tree`, `make_fpt_and_collision_blob`, `_pack_pfs_header_block`, `build_inode_block_sig_s64`, `Inode.to_bytes`, `Dirent.to_bytes`); the reader follows the same layout |
| [TweetNaCl](https://tweetnacl.cr.yp.to) (Bernstein, Janssen, Lange, Schwabe et al.) | public domain | X25519 for pairing engines (`src/engine/keys.cpp`): the field arithmetic and ladder of `crypto_scalarmult` |
| .NET runtime (`string.ToLowerInvariant`) | MIT | the lower-case mapping in `src/core/data/lowercase_table.inc`, generated from .NET 10 |

## Bundled source code

| Project | License | Where |
| --- | --- | --- |
| [zlib](https://zlib.net) 1.3.1 (release tarball, SHA-256 `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`) | zlib (`third_party/zlib/LICENSE`) | `third_party/zlib/`: the library sources needed for deflate/inflate, unchanged; built for the PS5 (the payload SDK has no zlib), or on a PC with `-DALEMBIC_BUNDLED_ZLIB=ON` |
| [lz4/lz4](https://github.com/lz4/lz4) v1.10.0 (tag commit `ebb370ca`) | BSD-2-Clause (notice below) | `third_party/lz4/`: `lz4.c`, `lz4.h`, `lz4hc.c`, `lz4hc.h`, `xxhash.c`, `xxhash.h`, unchanged |

## Bundled into the engine's web page

The page (`src/engine/web`) is bundled by esbuild into one HTML file that is
embedded in the engine, so these libraries ship inside `alembic.elf` and
`alembic-engine`. Versions are pinned in `src/engine/web/package-lock.json`.

| Project | License | What Alembic uses |
| --- | --- | --- |
| [Preact](https://github.com/preactjs/preact) 10.29.8 | MIT (notice below) | the page's components and rendering |
| [@preact/signals](https://github.com/preactjs/signals) 2.11.3 and @preact/signals-core 1.14.4 | MIT (notice below) | the page's shared state |

## PS5 build

| Project | License | What Alembic uses |
| --- | --- | --- |
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.44 | GPL-3.0-or-later | the toolchain, libc and libc++ the engine payload (`alembic.elf`) is built and linked with; the notification call follows its `samples/notify` |
| [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | GPL-3.0 | HTTPS downloads with `libSceHttp`/`libSceSsl` (from `src/sm_ampr_updater.c`) and its public CA certificates for GitHub (`src/engine/github_ca.inc`, from `src/sm_ampr_ca.inc`) |
| [ps5-payload-dev/shsrv](https://github.com/ps5-payload-dev/shsrv) | GPL-3.0-or-later | installing a `.pkg` with `sceAppInstUtilInstallByPackage` (from `bundles/pkg_install`) |
| [ps5-payload-dev/websrv](https://github.com/ps5-payload-dev/websrv), [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) | GPL-3.0-or-later | the Media-tab shortcut: a stub title in `/user/app` with a `deeplinkUri` param.json, registered with `sceAppInstUtilAppInstallTitleDir` (websrv `src/ps5/sys.c`, ps5-web-file-manager `src/app_installer.c`); category 65536 as in ps5-web-file-manager and [itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager) |
| [blackbearreloaded/ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) | GPL-3.0-or-later | design reference only (no code): the web page's Files layout, focus ring and button-hint bar follow its Files and Settings screens |
| theghostonline/PS5-LZ4-Compression | GPL-3.0-or-later | keeping the console awake during a job (`sceSystemServicePowerTick`), from `src/lzc_power_guard.c`; the engine/app split and the job/web API idea |

## Bundled data

| Project | License | Where |
| --- | --- | --- |
| LZ4 pack profiles by danthespal, posted with [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp), as bundled unchanged by PS5-LZ4-Compression (`profiles/`) | GPL-3.0 | `profiles/lz4/*.toml`, unchanged, built into the binary |

## Downloaded on request (not part of Alembic)

The engine's Components page downloads these from their sources, checks each
file's SHA-256 and stores it on the console; they are never in this repository
or in Alembic's binaries.

| File | Source | Note |
| --- | --- | --- |
| `libSceAmpr.sprx` (ampr_emu 0.4.2.1 PackedStdio) | drakmor/ampr_emu release asset | GPL-3.0 |
| `passcode.bin`, `mount_image.bin`, `metadata_modulus.bin` | pearlxcore/ProsperoPkgTool `ProsperoPkgTool/Keys/` at `f2c91cd7a5d8` | public RSA moduli that project ships; it describes them as constants from Sony's publishing tools |

## Used for testing only (not part of Alembic builds)

| Project | License | Use |
| --- | --- | --- |
| [SvenGDK/LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG) | GPL-3.0 | `tools/pkg-oracle` builds and extracts test packages with it |
| pearlxcore/PS5PKGTool, pearlxcore/ProsperoPkgTool, drakmor/ampr_emu | as above | `tools/oracle/compare.sh` compares Alembic's output with theirs |
| [zlib](https://zlib.net) | zlib | linked for PFSC compression and CRC-32 |
| drakmor/ampr_emu `tools/ampr_pack.py` | GPL-3.0 | `tools/lz4-oracle/compare.sh` packs and verifies with it |

## Preact license

Preact is Copyright (c) 2015-present Jason Miller; @preact/signals and
@preact/signals-core are Copyright (c) 2022-present Preact Team. Both under:

```
The MIT License (MIT)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## LZ4 license

`third_party/lz4/LICENSE` (xxHash in the same folder is under the same BSD-2-Clause terms, Copyright (C) 2012-2016, Yann Collet):

```
LZ4 Library
Copyright (c) 2011-2020, Yann Collet
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## UFS2Tool license

```
BSD 2-Clause License

Copyright (c) 2026, SvenGDK
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## ProsperoPkgTool license

```
MIT License

Copyright (c) 2026 pearlxcore

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

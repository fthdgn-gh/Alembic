# Third-party notices

Alembic is licensed under the GNU General Public License v3.0 or later
(`LICENSE`). Parts of it are ported from, or follow, the projects below.
Ported files say where they come from in their header comment.

## Code ported from other projects

| Project | License | What Alembic uses |
| --- | --- | --- |
| [pearlxcore/PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool) | GPL-3.0 | exFAT image writer and reader, PFSC codec, FFPFSC image, AMPR index (`src/core/exfat_*.cpp`, `pfsc.cpp`, `ffpfsc.cpp`, `ampr.cpp`); the exFAT up-case table in `src/core/data/exfat_upcase_table.inc` |
| [pearlxcore/ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool) | MIT (notice below) | FPKG reading: package header/CNT parsing, key derivation, outer PFS reading |
| [SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool), as shipped in PS5PKGTool | BSD-2-Clause (notice below) | UFS2 reader (`src/core/ufs2.cpp`): superblock field offsets and validation rules |
| [drakmor/ampr_emu](https://github.com/drakmor/ampr_emu) | GPL-3.0 | AMPRIDX3 hashing, sorting and exclusion rules (`ampr::Scheme::ampr_emu`), following `tools/build_ampr_index.py` and `src/ampr_emu_index.cpp`; LZ4 asset packs (AMPRPAK4/AMPRDAT3/AMPRCRC1/AMPRCFG1 format, packer, verifier) in `src/core/lz4pack_*.cpp`, ported from `tools/ampr_pack.py` and `tools/ampr_pack_format.py` |
| [theghostonline/PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression) | GPL-3.0-or-later | the automatic LZ4 pack profile, its safety list, the keep-on-disk list and the runtime cache budget (`lz4pack::automatic_profile`, `src/core/lz4pack_build.cpp`), from `src/ampr_pack_profile.c` and `src/ampr_pack.c` only (not its `src/pfs_*` code); how bundled profiles are matched to a game and how a TOML profile is applied (`src/core/lz4pack_profile.cpp`, from `src/lzc_api.c` and `src/ampr_pack_profile.c`); the TOML subset `src/core/toml.cpp` reads (from `src/ampr_pack_toml.c`) |
| .NET runtime (`string.ToLowerInvariant`) | MIT | the lower-case mapping in `src/core/data/lowercase_table.inc`, generated from .NET 10 |

## Bundled source code

| Project | License | Where |
| --- | --- | --- |
| [lz4/lz4](https://github.com/lz4/lz4) v1.10.0 (tag commit `ebb370ca`) | BSD-2-Clause (notice below) | `third_party/lz4/`: `lz4.c`, `lz4.h`, `lz4hc.c`, `lz4hc.h`, `xxhash.c`, `xxhash.h`, unchanged |

## Bundled data

| Project | License | Where |
| --- | --- | --- |
| LZ4 pack profiles by danthespal, posted with [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp), as bundled unchanged by PS5-LZ4-Compression (`profiles/`) | GPL-3.0 | `profiles/lz4/*.toml`, unchanged, built into the binary |

## Used for testing only (not part of Alembic builds)

| Project | License | Use |
| --- | --- | --- |
| [SvenGDK/LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG) | GPL-3.0 | `tools/pkg-oracle` builds and extracts test packages with it |
| pearlxcore/PS5PKGTool, pearlxcore/ProsperoPkgTool, drakmor/ampr_emu | as above | `tools/oracle/compare.sh` compares Alembic's output with theirs |
| [zlib](https://zlib.net) | zlib | linked for PFSC compression and CRC-32 |
| drakmor/ampr_emu `tools/ampr_pack.py` | GPL-3.0 | `tools/lz4-oracle/compare.sh` packs and verifies with it |

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

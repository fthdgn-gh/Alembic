# Third-party notices

Alembic is licensed under the GNU General Public License v3.0 or later
(`LICENSE`). Parts of it are ported from, or follow, the projects below.
Ported files say where they come from in their header comment.

## Code ported from other projects

| Project | License | What Alembic uses |
| --- | --- | --- |
| [pearlxcore/PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool) | GPL-3.0 | exFAT image writer and reader, PFSC codec, FFPFSC image, AMPR index (`src/core/exfat_*.cpp`, `pfsc.cpp`, `ffpfsc.cpp`, `ampr.cpp`); the exFAT up-case table in `src/core/data/exfat_upcase_table.inc` |
| [pearlxcore/ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool) | MIT (notice below) | FPKG reading: package header/CNT parsing, key derivation, outer PFS reading |
| [drakmor/ampr_emu](https://github.com/drakmor/ampr_emu) | GPL-3.0 | AMPRIDX3 hashing, sorting and exclusion rules (`ampr::Scheme::ampr_emu`), following `tools/build_ampr_index.py` and `src/ampr_emu_index.cpp` |
| .NET runtime (`string.ToLowerInvariant`) | MIT | the lower-case mapping in `src/core/data/lowercase_table.inc`, generated from .NET 10 |

## Used for testing only (not part of Alembic builds)

| Project | License | Use |
| --- | --- | --- |
| [SvenGDK/LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG) | GPL-3.0 | `tools/pkg-oracle` builds and extracts test packages with it |
| pearlxcore/PS5PKGTool, pearlxcore/ProsperoPkgTool, drakmor/ampr_emu | as above | `tools/oracle/compare.sh` compares Alembic's output with theirs |
| [zlib](https://zlib.net) | zlib | linked for PFSC compression |

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

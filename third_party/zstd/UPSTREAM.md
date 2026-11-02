# Zstandard 1.5.7

- Source: https://github.com/facebook/zstd/releases/tag/v1.5.7
  (`zstd-1.5.7.tar.gz`, SHA-256
  `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`,
  as the release's `zstd-1.5.7.tar.gz.sha256` gives it).
- Kept: `LICENSE`, and from `lib/`: `zstd.h`, `zstd_errors.h`, `common/`,
  `compress/`, `decompress/`, unchanged, without
  `decompress/huf_decompress_amd64.S` (built with `ZSTD_DISABLE_ASM`; the C
  decoder gives the same bytes). No `legacy/`, `dictBuilder/`,
  `deprecated/`.
- License: BSD-3-Clause OR GPL-2.0-only; Alembic uses it under the BSD
  license, which GPL-3.0 accepts.
- 1.5.7 is the libzstd quer3q/ps5-dump-forge compresses its zstd packs with
  (Rust `zstd` 0.13.3 -> `zstd-sys` 2.1.2+zstd.1.5.7): the same library and
  parameters give the same frames.
- Built single-threaded (no `ZSTD_MULTITHREAD`, as `zstd-sys` without its
  `zstdmt` feature), with `XXH_NAMESPACE=ZSTD_` (its xxHash next to LZ4's)
  and `ZSTD_LEGACY_SUPPORT=0`.
- Updating: replace these folders from a newer release, update this file,
  and run `tools/zstd-oracle/compare.sh` (a different libzstd may give
  different frames than ps5-dump-forge).

# Alembic

A PS5 homebrew app that converts FPKG (debug PS5 packages) to and from other
formats: dump folders, exFAT, FFPKG (UFS2) and FFPFSC.

Read [docs/NOTES.md](docs/NOTES.md) first: it holds the research on the
reference repos, the decisions made so far, progress, and the next step.

Build and test:

```sh
cmake -S . -B build && cmake --build build && ./build/alembic_tests
tools/oracle/compare.sh /path/to/PS5PKGTool   # byte-for-byte check vs PS5PKGTool
```

Output must stay byte-identical to PS5PKGTool (the AMPR index with
`--ampr-scheme ps5pkgtool`) and to ampr_emu's index builder (default scheme).
Run the oracle after changing any format code. Reference checkouts live in
`.refs/` (git-ignored): PS5PKGTool, ampr_emu, LibProsperoPKG-sven, fpkg-cli.

## Decisions so far

- **Name:** Alembic. "ProsperoPKGTool" was dropped because it clashes with
  pearlxcore/ProsperoPkgTool. The folder may still be called `ProsperoPKGTool`
  until it is renamed.
- **Architecture:** a portable C++20 core library and a PC command-line tool
  first, checked byte-for-byte against PS5PKGTool on a PC. A PS5 front end
  built on blackbearreloaded/ps5-homebrew-ui comes after that.
- **First conversion:** dump folder → FFPFSC (exFAT + PFSC + zlib, no crypto).
  FPKG reading (crypto, Kraken, package reader) comes next.
- **Language rules:** C++20 with no exceptions and no RTTI (the PS5 build
  forbids them), so errors are returned as values.
- **License:** GPL-3.0-or-later, compatible with every source we port from.

## Never

- Ship or depend on Oodle (`oo2core_*.dll`), `libScePubTools.dll`, or the
  closed binary LibProsperoPkg releases (the "Drakmor 0.6.x" line).
- Copy binaries or key material out of reference repos (for example
  LibProsperoPKG's `right.sprx`, `Keys/Data/*`, `pkg_meta_rsa_key.pem`)
  without first establishing where they come from.

Porting from GPL-3.0 source is fine, including SvenGDK/LibProsperoPKG: keep
its NOTICE attributions (LibOrbisPkg, ooz). Never copy code from repos with
no license, such as rdmrocha/fpkg-cli.
- Launch anything on a console without asking the owner first.

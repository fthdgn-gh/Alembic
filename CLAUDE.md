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

Output must stay byte-identical to PS5PKGTool; run the oracle after changing
any format code.

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

- Ship or depend on Oodle (`oo2core_*.dll`), LibProsperoPkg or
  `libScePubTools.dll`.
- Launch anything on a console without asking the owner first.

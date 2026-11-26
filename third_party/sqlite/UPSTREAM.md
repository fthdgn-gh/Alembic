# SQLite 3.54.1

- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
  (released 2026-10-16; 2,990,966 bytes, SHA3-256
  `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` as
  https://www.sqlite.org/download.html gives it, SHA-256
  `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`).
- Kept: `sqlite3.c` and `sqlite3.h`, unchanged (not `shell.c`,
  `sqlite3ext.h`).
- License: public domain (https://www.sqlite.org/copyright.html).
- Built with `SQLITE_THREADSAFE=1`, `SQLITE_OMIT_LOAD_EXTENSION` (no
  dlopen), `SQLITE_DQS=0`. Its own unix/win32 file layer opens and locks the
  database files itself: the one place outside `alembic/io.hpp` that does,
  since it has to share the console's locks with the system's own SQLite.
- Used by: the experiments build only (`-DALEMBIC_EXPERIMENTS=ON`,
  `src/engine/experiment.cpp`), which writes rows into the console's
  application databases. Release builds link nothing from it.
- Updating: replace both files from a newer amalgamation, update this file.

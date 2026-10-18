# Alembic icon

Drawn for Alembic (GPL-3.0-or-later, like the rest of the repo).

- `alembic.svg`: rounded square, for the engine's web page and favicon
  (served at `/icon.svg`, built into the engine).
- `alembic-square.svg`: full-bleed square for the PS5 app tile.
- `icon0.png`: `alembic-square.svg` at 512×512, the PS5 app's `sce_sys/icon0.png`.
  Regenerate on macOS with
  `qlmanage -t -s 512 -o /tmp assets/icon/alembic-square.svg` and copy the result here.

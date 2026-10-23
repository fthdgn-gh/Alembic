#!/bin/sh
# README screenshots: the real page against a stand-in engine (mock.mjs) with
# made-up games, a paired PC and live jobs, captured as the PS5 browser shows
# it (1600x900 at 2x) by a headless Chromium-based browser.
#
#   tools/screenshots/shoot.sh [name]     # files, convert, queue, engines; default: all
#
# Needs Node 22+, a build (build/alembic-engine) and Chrome, or another
# Chromium browser named by BROWSER=/path/to/binary. Writes docs/screenshots/.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
engine="$repo/build/alembic-engine"
[ -x "$engine" ] || { echo "build alembic-engine first (cmake --build build)" >&2; exit 1; }
work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT

# The engine's own info and components, so the mock stays current.
mkdir "$work/state" "$work/root"
"$engine" --port 5961 --listen 127.0.0.1 --state "$work/state" --root "$work/root" >/dev/null 2>&1 &
engine_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -sf http://127.0.0.1:5961/api/info >"$work/info.json" && break; sleep 0.5; done
curl -sf http://127.0.0.1:5961/api/deps >"$work/deps.json"
kill $engine_pid

node "$here/mock.mjs" "$repo" "$work" &
sleep 1
mkdir -p "$repo/docs/screenshots"
node "$here/shoot.mjs" "$repo/docs/screenshots" "${1:-}" "$work/profile"

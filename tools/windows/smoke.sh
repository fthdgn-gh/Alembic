#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks the Windows build on Windows (Git Bash on a GitHub runner, or a PC):
# the unit tests, conversions of a folder with non-ASCII names and a path
# longer than 260 characters, and the engine answering over HTTP.
#   tools/windows/smoke.sh DIR   (DIR holds alembic.exe, alembic-engine.exe, alembic_tests.exe)
set -euo pipefail
bin="$(cd "${1:?usage: smoke.sh DIR}" && pwd)"
work="$(mktemp -d)"
engine=""
trap '[[ -n $engine ]] && kill "$engine" 2>/dev/null; rm -rf "$work"' EXIT
# Windows paths with '/' ("C:/Users/..."), which Alembic takes as they are.
win() { cygpath -m "$1"; }

echo "== unit tests"
# The tests read a few files from the checkout (translations, reference keys).
ALEMBIC_SOURCE_DIR="$(win "$(cd "$(dirname "$0")/../.." && pwd)")" "$bin/alembic_tests.exe"

echo "== a game folder with non-ASCII names and a long path"
in="$work/in"
mkdir -p "$in/sce_sys" "$in/Ünïcode dir"
echo '{}' > "$in/sce_sys/param.json"
head -c 3000000 /dev/urandom > "$in/Ünïcode dir/Doğan ☃.bin"
deep="$in"
for i in 1 2 3 4 5 6; do deep="$deep/a-folder-name-that-is-fifty-characters-long-$i-xxxxx"; done
mkdir -p "$deep"
printf 'deep file\n%.0s' {1..2000} > "$deep/deep.txt"
(( ${#deep} > 300 )) || { echo "the long path is too short"; exit 1; }

a="$bin/alembic.exe"
"$a" convert "$(win "$in")" "$(win "$work/out.exfat")"
"$a" convert "$(win "$work/out.exfat")" "$(win "$work/out.ffpfsc")"
"$a" ffpfsc-verify "$(win "$work/out.ffpfsc")"
"$a" convert "$(win "$work/out.ffpfsc")" "$(win "$work/back")/"
"$a" same "$(win "$in")" "$(win "$work/back")"
# Backslashes work too.
"$a" ls "$(cygpath -w "$work/out.ffpfsc")" | grep -q "deep.txt"

echo "== the engine"
state="$work/state"
"$bin/alembic-engine.exe" --listen 127.0.0.1 --port 5941 --state "$(win "$state")" --root "$(win "$work")" &
engine=$!
for _ in $(seq 50); do curl -sf http://127.0.0.1:5941/api/info >/dev/null && break; sleep 0.2; done
info=$(curl -sf http://127.0.0.1:5941/api/info)
echo "$info" | grep -q '"os":"Windows 1[01] (build '
echo "$info" | grep -q '"host_name":"[^"]'
echo "$info" | grep -q '"local_url":"http://localhost:5941/"'
root="$(win "$work")"
curl -sf --get --data-urlencode "path=$root" http://127.0.0.1:5941/api/browse | grep -q 'out.ffpfsc'
curl -sf --get --data-urlencode "path=$root/in" http://127.0.0.1:5941/api/browse | grep -q 'Ünïcode dir'
# Discovery: off by default; switched on, it opens its UDP port and announces.
curl -sf http://127.0.0.1:5941/api/peers | grep -q '"enabled":false'
curl -sf -X POST http://127.0.0.1:5941/api/peers/discovery -d '{"enabled":true}' | grep -q '"listening":true'
curl -sf -X POST http://127.0.0.1:5941/api/peers/discovery -d '{"enabled":false}' | grep -q '"enabled":false'
# Pairing: requests from other engines must be signed.
code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:5941/api/pair/check)
[[ $code == 401 ]] || { echo "an unsigned request gave $code"; exit 1; }
curl -sf http://127.0.0.1:5941/api/pair/incoming | grep -q '"local":true'
# Outside the root: refused.
code=$(curl -s -o /dev/null -w '%{http_code}' --get --data-urlencode "path=C:/Windows" http://127.0.0.1:5941/api/browse)
[[ $code == 403 ]] || { echo "browsing outside the root gave $code"; exit 1; }
# A job through the queue: the FFPFSC back to exFAT.
curl -sf -X POST http://127.0.0.1:5941/api/jobs \
    -d "{\"kind\":\"convert\",\"input\":\"$root/out.ffpfsc\",\"output_dir\":\"$root\",\"output_name\":\"queued.exfat\",\"to\":\"exfat\",\"ampr_emu\":\"off\"}" >/dev/null
for _ in $(seq 100); do
    s=$(curl -sf http://127.0.0.1:5941/api/jobs)
    [[ $s == *'"state":"done"'* ]] && break
    [[ $s == *'"state":"failed"'* ]] && { echo "$s"; exit 1; }
    sleep 0.2
done
[[ $s == *'"state":"done"'* ]] || { echo "the job did not finish: $s"; exit 1; }
"$a" same "$(win "$work/out.exfat")" "$(win "$work/queued.exfat")"
curl -sf -X POST http://127.0.0.1:5941/api/quit >/dev/null || true
wait "$engine" || true
engine=""
echo "all Windows checks passed"

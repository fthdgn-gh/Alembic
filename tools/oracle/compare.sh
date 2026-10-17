#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds fixtures, runs Alembic and PS5PKGTool on each and compares the output.
#   tools/oracle/compare.sh [path/to/PS5PKGTool]
# Needs: a configured build in ./build, dotnet 10, python3.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
ps5pkgtool="${1:-$repo/.refs/PS5PKGTool}"
alembic="$repo/build/alembic"
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-oracle-XXXXXX")"
trap 'rm -rf "$work"' EXIT

dotnet build "$here/Oracle.csproj" -c Release -nologo -v q -p:PS5PKGToolDir="$ps5pkgtool" \
    -o "$work/oracle" >/dev/null
oracle() { dotnet "$work/oracle/Oracle.dll" "$@"; }
python3 -I "$here/make_fixtures.py" "$work/fixtures"

fail=0
check() { # name, file a, file b
    if cmp -s "$2" "$3"; then echo "same  $1"; else echo "DIFF  $1"; cmp "$2" "$3" | head -1; fail=1; fi
}

for fixture in "$work"/fixtures/*; do
    name="$(basename "$fixture")"
    for cluster in default 4096 131072; do
        extra=(); [[ $cluster != default ]] && extra=(--cluster-size "$cluster")
        oextra=(); [[ $cluster != default ]] && oextra=("$cluster")
        "$alembic" exfat "$fixture" "$work/$name-$cluster.alembic.img" ${extra[@]+"${extra[@]}"} 2>/dev/null
        oracle exfat "$fixture" "$work/$name-$cluster.oracle.img" ${oextra[@]+"${oextra[@]}"}
        check "exfat $name cluster=$cluster" "$work/$name-$cluster.alembic.img" "$work/$name-$cluster.oracle.img"
    done
done

# PFSC: compressed bytes depend on the zlib build on each side (they matched on
# macOS with .NET 10 and system zlib, 2026-10-17). The hard requirement is that
# each side decodes the other's output to the same bytes.
img="$work/many-default.oracle.img"
"$alembic" pfsc-encode "$img" "$work/a.pfsc" >/dev/null 2>&1
oracle pfsc-encode "$img" "$work/o.pfsc" >/dev/null
oracle pfsc-decode "$work/a.pfsc" "$work/a.by-oracle"
"$alembic" pfsc-decode "$work/o.pfsc" "$work/o.by-alembic" 2>/dev/null
"$alembic" pfsc-decode "$work/a.pfsc" "$work/a.by-alembic" 2>/dev/null
check "pfsc alembic->oracle decode" "$work/a.by-oracle" "$work/a.by-alembic"
check "pfsc oracle->alembic decode" "$work/o.by-alembic" "$work/a.by-alembic"
head -c "$(stat -f%z "$img" 2>/dev/null || stat -c%s "$img")" "$work/a.by-alembic" > "$work/a.trimmed"
check "pfsc round trip" "$img" "$work/a.trimmed"
if cmp -s "$work/a.pfsc" "$work/o.pfsc"; then echo "same  pfsc container bytes"; else echo "note  pfsc container bytes differ (zlib builds differ; decode checks above still apply)"; fi

exit $fail

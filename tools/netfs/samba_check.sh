#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Alembic's SMB client against Samba in several configurations: SMB 3 with
# encryption required (AES-128-GCM and -CCM, dialects 3.0 to 3.1.1, by the
# server or by one share), no encryption, and a dialect that cannot encrypt.
# Each runs the live test (tests/test_netfs.cpp smb_live_session: upload,
# list, download whole and from an offset, rename, remove). Samba refuses
# unencrypted requests where it requires encryption, so a pass there means
# the traffic was encrypted.
#   tools/netfs/samba_check.sh
# Needs Samba (macOS: brew install samba) and a build (build/alembic_tests).
# Samba runs as this user on 127.0.0.1:4450 with its own folders.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
tests="$repo/build/alembic_tests"
smbd=""
for c in /opt/homebrew/opt/samba/sbin/samba-dot-org-smbd /opt/homebrew/opt/samba/sbin/smbd /usr/local/opt/samba/sbin/smbd \
         "$(command -v smbd 2>/dev/null)"; do
    [[ -n $c && -x $c ]] && "$c" --version 2>/dev/null | grep -q '^Version' && { smbd="$c"; break; }
done
[[ -n $smbd ]] || { echo "Samba's smbd not found (brew install samba)"; exit 2; }
pdbedit="$(dirname "$smbd")/../bin/pdbedit"
[[ -x $pdbedit ]] || pdbedit="$(command -v pdbedit)"
# Samba's runtime folders need a short path.
# (resolved: on macOS /tmp is a link, and Samba refuses paths through links).
work="$(cd "$(mktemp -d /tmp/alsmb.XXXX)" && pwd -P)"
pid=""
stop() { [[ -n $pid ]] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null; pid=""; }
trap 'stop; [[ -n ${ALEMBIC_KEEP_SAMBA:-} ]] || rm -rf "$work"' EXIT
user="$(id -un)" password="alembic-test-1"
port=4450

# config <max protocol> <server encrypt> <share encrypt> <algorithms>
config() {
    mkdir -p "$work"/{p,l,s,c,r,share}
    cat >"$work/smb.conf" <<CONF
[global]
  server role = standalone server
  smb ports = $port
  interfaces = 127.0.0.1
  bind interfaces only = yes
  disable netbios = yes
  load printers = no
  private dir = $work/p
  lock directory = $work/l
  state directory = $work/s
  cache directory = $work/c
  pid directory = $work/r
  ncalrpc dir = $work/r/ncalrpc
  log file = $work/log
  log level = 3
  passdb backend = tdbsam:$work/p/passdb.tdb
  server signing = mandatory
  server min protocol = SMB2_02
  server max protocol = $1
  server smb encrypt = $2
  server smb3 encryption algorithms = $4
[games]
  path = $work/share
  read only = no
  # Unprivileged on macOS, Samba's ACL mapping makes folders mode 000.
  nt acl support = no
  create mask = 0644
  force create mode = 0644
  directory mask = 0755
  force directory mode = 0755
  server smb encrypt = $3
CONF
}

start() {
    # In a session of its own: smbd ends its process group when it exits.
    python3 -I -c 'import os, sys; os.setsid(); os.execv(sys.argv[1], sys.argv[1:])' \
        "$smbd" --foreground --no-process-group --debug-stdout -s "$work/smb.conf" >"$work/smbd.out" 2>&1 &
    pid=$!
    for _ in $(seq 50); do
        nc -z 127.0.0.1 $port 2>/dev/null && return 0
        sleep 0.1
    done
    echo "Samba did not start:"; tail -5 "$work/smbd.out"; return 1
}

fail=0
# label, expect (pass|refused:<text>), max protocol, server encrypt, share encrypt, algorithms
run() {
    local label="$1" expect="$2"
    shift 2
    config "$@"
    # pdbedit writes the user database directly (smbpasswd -a needs root).
    printf '%s\n%s\n' "$password" "$password" | "$pdbedit" -s "$work/smb.conf" -a -u "$user" -t >/dev/null 2>&1
    start || { fail=1; return; }
    out="$(ALEMBIC_SMB_TEST="127.0.0.1:$port:games:$user:$password" "$tests" smb_live 2>&1)"
    stop
    if [[ $expect == pass ]]; then
        if grep -q ', 0 failed checks' <<<"$out" && ! grep -q skipped <<<"$out"; then echo "ok    $label"
        else echo "FAIL  $label"; echo "$out" | tail -5; fail=1; fi
    else
        if grep -q "${expect#refused:}" <<<"$out"; then echo "ok    $label: refused (${expect#refused:})"
        else echo "FAIL  $label: not refused as expected"; echo "$out" | tail -5; fail=1; fi
    fi
}

all="AES-128-GCM, AES-128-CCM"
run "SMB 3.1.1, server requires encryption (GCM)" pass SMB3_11 required required "$all"
run "SMB 3.1.1, server requires encryption (CCM only)" pass SMB3_11 required required "AES-128-CCM"
run "SMB 3.0.2, server requires encryption (CCM)" pass SMB3_02 required required "$all"
run "SMB 3.0, server requires encryption (CCM)" pass SMB3_00 required required "$all"
run "SMB 3.1.1, only the share requires encryption" pass SMB3_11 if_required required "$all"
run "SMB 3.1.1, no encryption" pass SMB3_11 off off "$all"
# Samba itself refuses the share to a dialect that cannot encrypt.
run "SMB 2.1, share requires encryption" "refused:cannot open the share" SMB2_10 if_required required "$all"
exit $fail

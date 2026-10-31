#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Alembic's TLS clients against local servers with a self-signed
# certificate: FTPS (pyftpdlib, explicit AUTH TLS, TLS required on control
# and data connections) and HTTPS WebDAV (WsgiDAV on cheroot), plus their
# plain forms. Each runs the live test (tests/test_netfs.cpp
# netfs_live_session): first the trust steps (no pin: refused with this
# certificate's fingerprint; a wrong pin: "changed"), then upload, list,
# download, stop part way, rename, remove.
#   tools/netfs/tls_check.sh
# Needs .refs/netfs-venv (pyftpdlib, pyOpenSSL, WsgiDAV, cheroot), openssl
# and a build (build/alembic_tests).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
tests="$repo/build/alembic_tests"
py="$repo/.refs/netfs-venv/bin/python"
[[ -x $py ]] || { echo "needs .refs/netfs-venv (see docs/NOTES.md)"; exit 2; }
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-tls-XXXXXX")"
scripts="$(mktemp -d "${TMPDIR:-/tmp}/alembic-tls-scripts-XXXXXX")"
pid=""
stop() { [[ -n $pid ]] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null; pid=""; }
trap 'stop; rm -rf "$work" "$scripts"' EXIT
user=alembic password='p&ss wörd'

openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=127.0.0.1" \
    -keyout "$work/key.pem" -out "$work/cert.pem" >/dev/null 2>&1 || { echo "openssl failed"; exit 2; }
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj "/CN=127.0.0.1" \
    -keyout "$work/ec-key.pem" -out "$work/ec-cert.pem" >/dev/null 2>&1 || { echo "openssl failed (EC)"; exit 2; }
fp() { openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/.*=//'; }

cat >"$scripts/ftp_server.py" <<'PY'
import sys
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
from pyftpdlib.servers import FTPServer
root, port, user, password, tls, cert, key = sys.argv[1:8]
auth = DummyAuthorizer()
auth.add_user(user, password, root, perm="elradfmwMT")
if tls in ("1", "12"):
    handler = TLS_FTPHandler
    handler.certfile, handler.keyfile = cert, key
    if tls == "12":  # TLS 1.2 only
        from OpenSSL import SSL
        ctx = SSL.Context(SSL.TLS_SERVER_METHOD)
        ctx.set_max_proto_version(SSL.TLS1_2_VERSION)
        ctx.use_certificate_chain_file(cert)
        ctx.use_privatekey_file(key)
        handler.ssl_context = ctx
    handler.tls_control_required = True
    handler.tls_data_required = True
    # Like vsftpd's require_ssl_reuse: a data connection must resume the
    # control connection's TLS session, or it is dropped.
    from OpenSSL._util import lib
    from pyftpdlib.handlers import TLS_DTPHandler
    class ReuseRequired(TLS_DTPHandler):
        def handle_ssl_established(self):
            if not lib.SSL_session_reused(self.socket._ssl):
                with open(root + "/../reuse.log", "a") as f:
                    f.write("data connection without session reuse\n")
                self.close()
                return
            super().handle_ssl_established()
    handler.dtp_handler = ReuseRequired
else:
    handler = FTPHandler
handler.authorizer = auth
handler.passive_ports = range(int(port) + 1, int(port) + 50)
FTPServer(("127.0.0.1", int(port)), handler).serve_forever()
PY

cat >"$scripts/dav_server.py" <<'PY'
import sys
from cheroot import wsgi
from cheroot.ssl.builtin import BuiltinSSLAdapter
from wsgidav.wsgidav_app import WsgiDAVApp
root, port, user, password, tls, cert, key = sys.argv[1:8]
app = WsgiDAVApp({
    "provider_mapping": {"/dav": root},
    "simple_dc": {"user_mapping": {"*": {user: {"password": password}}}},
    "http_authenticator": {"domain_controller": None, "accept_basic": True, "accept_digest": True,
                           "default_to_digest": False},
    "verbose": 0, "logging": {"enable": False},
})
server = wsgi.Server(("127.0.0.1", int(port)), app)
if tls in ("1", "12"):
    server.ssl_adapter = BuiltinSSLAdapter(cert, key)
    if tls == "12":  # TLS 1.2 only
        import ssl
        server.ssl_adapter.context.maximum_version = ssl.TLSVersion.TLSv1_2
server.start()
PY

started() {
    for _ in $(seq 60); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

fail=0
# label, protocol, server script, port, root (as the client gives it), tls, [cert prefix]
run() {
    local label="$1" protocol="$2" script="$3" port="$4" root="$5" tls="$6" prefix="${7:-}"
    local cert="$work/${prefix}cert.pem" key="$work/${prefix}key.pem"
    local fingerprint
    fingerprint="$(fp "$cert")"
    rm -rf "$work/files" && mkdir -p "$work/files"
    "$py" -I "$scripts/$script" "$work/files" "$port" "$user" "$password" "$tls" "$cert" "$key" \
        >"$work/server.log" 2>&1 &
    pid=$!
    if ! started "$port"; then echo "FAIL  $label: server did not start"; tail -5 "$work/server.log"; fail=1; stop; return; fi
    local client_tls=$tls
    [[ $tls == 12 ]] && client_tls=1
    out="$(ALEMBIC_NETFS_TEST="$protocol|127.0.0.1|$port|$root|$user|$password|$client_tls|$fingerprint" "$tests" netfs_live 2>&1)"
    stop
    if grep -q ', 0 failed checks' <<<"$out" && ! grep -q skipped <<<"$out"; then echo "ok    $label"
    else echo "FAIL  $label"; echo "$out" | tail -8; fail=1; fi
}

run "FTPS (explicit AUTH TLS, TLS required for data), TLS 1.3" ftp ftp_server.py 2121 "" 1
run "FTPS, TLS 1.2 only" ftp ftp_server.py 2321 "" 12
run "FTP (plain)" ftp ftp_server.py 2221 "" 0
run "WebDAV over HTTPS, TLS 1.3" webdav dav_server.py 8443 /dav 1
run "WebDAV over HTTPS, TLS 1.2 only" webdav dav_server.py 8444 /dav 12
run "WebDAV over HTTP" webdav dav_server.py 8080 /dav 0
run "FTPS, ECDSA P-256 certificate" ftp ftp_server.py 2521 "" 1 ec-
run "WebDAV over HTTPS, ECDSA P-256 certificate" webdav dav_server.py 8445 /dav 1 ec-
exit $fail

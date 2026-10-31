// SPDX-License-Identifier: GPL-3.0-or-later
// Network servers games can be read from and saved to: SMB shares, NFS exports, WebDAV and FTP.
import { useState } from "preact/hooks";
import type { NetServer, Protocol } from "../../api/types";
import { api, ApiError } from "../../api/client";
import { servers, loadInfo } from "../../state/engine";
import { serverAddress, defaultPort } from "../../state/paths";
import { t, tMsg } from "../../lib/i18n";
import { ICONS } from "../../components/Icons";
import { Sheet, openOverlay, closeOverlay } from "../../components/Overlay";
import { ask } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { SwitchRow } from "../../components/Rows";
import { Head, SmallBtn, Field, useForm, formHints } from "./parts";

const PROTOCOLS: { value: Protocol; label: string; hint: string; port: number }[] = [
  { value: "smb", label: "SMB", hint: "Windows shared folders, NAS", port: 445 },
  { value: "nfs", label: "NFS", hint: "NAS and Linux exports", port: 2049 },
  { value: "webdav", label: "WebDAV", hint: "NAS, Nextcloud, rclone", port: 80 },
  { value: "ftp", label: "FTP", hint: "FileZilla and other FTP servers", port: 21 },
];

export function NetServers() {
  return (
    <>
      <Head text={t("Network servers")} />
      <div class="card">
        <p class="muted" style="margin:0 0 .5rem">{t("Games on a PC or NAS can be converted from there over SMB (shared folders), NFS, WebDAV or FTP, and exFAT, FFPKG, FFPFS and folder outputs saved there. FFPFSC and PKG outputs must be saved on the console first. SMB is encrypted when the server or share requires it, FTP and WebDAV when TLS is on (the server's certificate is trusted on first use); otherwise files and FTP and WebDAV passwords cross the network as they are. Passwords are kept on the console.")}</p>
        {servers.value.map((s) => (
          <div class="ftprow">
            <div class="nm">{ICONS.server()}</div>
            <div style="min-width:0;flex:1"><div class="ellip">{s.name}</div><div class="muted mono ellip" style="font-size:.85rem">{serverAddress(s, true)}</div></div>
            <SmallBtn k={"srv:ed:" + s.path} text={t("Edit")} onClick={() => serverDialog(s)} />
            <SmallBtn k={"srv:rm:" + s.path} cls="danger" text={t("Remove")} onClick={() => removeServer(s)} />
          </div>
        ))}
        <div class="btns"><SmallBtn k="srv:add" text={t("Add a server…")} onClick={() => serverDialog(null)} /></div>
      </div>
    </>
  );
}

// Adds a server, or changes `server`; the engine logs in once before keeping it.
function serverDialog(server: NetServer | null) { openOverlay(() => <ServerForm server={server} />, formHints()); }
function ServerForm(props: { server: NetServer | null }) {
  const s = props.server;
  const [protocol, setProtocol] = useState<Protocol>(s ? s.protocol : "smb");
  const [name, setName] = useState(s ? s.name : ""), [host, setHost] = useState(s ? s.host : "");
  const [port, setPort] = useState(s ? String(s.port) : "");
  const [root, setRoot] = useState(s ? (s.protocol === "webdav" && s.root === "/" ? "" : s.root) : "");
  const [user, setUser] = useState(s ? s.user : ""), [pass, setPass] = useState("");
  const [tls, setTls] = useState(!!(s && s.tls));
  const f = useForm();
  const canTls = protocol === "ftp" || protocol === "webdav";
  async function save(fingerprint?: string) {
    if (!host.trim()) { f.setMsg({ text: t("Enter the server's address"), err: true }); return; }
    f.setBusy(true); f.setMsg({ text: t("Connecting…"), err: false });
    const body: Record<string, unknown> = { protocol, name: name.trim(), host: host.trim(), port: parseInt(port, 10) || 0, user: user.trim(), root: root.trim() };
    if (canTls && tls) {
      body.tls = true;
      // The certificate trusted before, or the one just confirmed.
      const pin = fingerprint || (s && s.fingerprint);
      if (pin) body.fingerprint = pin;
    }
    // Editing: an empty password keeps the saved one.
    if (!s || pass) body.password = pass;
    try {
      if (s) await api("PUT", "/api/servers/" + encodeURIComponent(s.id), body);
      else await api("POST", "/api/servers", body);
      toast(s ? t("Server saved") : t("Server added"), "ok");
      closeOverlay(true);
      loadInfo().catch(() => {});
    } catch (e: any) {
      // A certificate seen for the first time, or a changed one: ask, then try again with it.
      const cert = e instanceof ApiError && typeof e.data.certificate === "string" ? e.data.certificate as string : "";
      if (cert && !fingerprint) {
        f.setMsg({ text: "", err: false });
        if (await trustCertificate(name.trim() || host.trim(), cert, !!(s && s.fingerprint && s.fingerprint !== cert))) return save(cert);
        f.setBusy(false);
        return;
      }
      f.setMsg({ text: tMsg(e.message), err: true }); f.setBusy(false);
    }
  }
  return (
    <Sheet title={s ? t("Edit {name}", { name: s.name }) : t("Add a server")} cls="narrow" foot={<><f.Msg /><button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={f.busy} onClick={() => save()}>{s ? t("Save") : t("Add")}</button></>}>
      <div>
        {/* The protocol is chosen once: a server keeps it (and its paths) when edited. */}
        {s ? null : <>
          <div class="lbl">{t("Protocol")}</div>
          <div class="opts two">
            {PROTOCOLS.map((p) => (
              <button key={p.value} class={"opt" + (protocol === p.value ? " sel" : "")} data-key={"srv:p:" + p.value} data-autofocus={protocol === p.value ? "" : undefined} onClick={() => setProtocol(p.value)}>
                <b>{p.label}</b><span>{t(p.hint)}</span>
              </button>
            ))}
          </div>
        </>}
        <Field label={t("Name")} placeholder={t("My PC")} value={name} onInput={setName} />
        <Field label={t("Address")} placeholder={protocol === "webdav" ? "http://192.168.1.10:5005/dav" : "192.168.1.10"} value={host} onInput={setHost} />
        {protocol === "smb" ? <Field label={t("Share")} placeholder={t("Games or Games/PS5; empty: all")} value={root} onInput={setRoot} /> : null}
        {protocol === "webdav" ? <Field label={t("Path")} placeholder="/" value={root} onInput={setRoot} /> : null}
        {protocol === "nfs" ? <Field label={t("Export")} placeholder={t("/volume1/games; empty: all")} value={root} onInput={setRoot} /> : null}
        <SwitchRow label={t("Encrypt with TLS")} hint={protocol === "ftp" ? t("FTPS: AUTH TLS, or implicit on port 990") : t("HTTPS")} on={tls} onChange={setTls} hidden={!canTls} dataKey="srv:tls" />
        <Field label={t("Port")} inputMode="numeric" placeholder={String(defaultPort(protocol, canTls && tls))} value={port} onInput={setPort} />
        {protocol === "nfs"
          ? <Field label={t("User and group ID")} placeholder={t("uid:gid like 1000:1000; empty: 0:0")} value={user} onInput={setUser} />
          : <Field label={t("User")} placeholder={protocol === "ftp" ? "anonymous" : protocol === "smb" ? t("Empty for a guest login") : ""} value={user} onInput={setUser} />}
        {protocol === "nfs" ? null : <Field label={t("Password")} type="password" placeholder={s ? t("Unchanged") : undefined} value={pass} onInput={setPass} />}
      </div>
    </Sheet>
  );
}
// Trust on first use: the fingerprint to compare with what the server shows.
function trustCertificate(server: string, fingerprint: string, changed: boolean): Promise<string | true | null> {
  return ask({
    title: changed ? t("{name}'s certificate changed", { name: server }) : t("Trust {name}'s certificate?", { name: server }),
    body: <>
      <p style="margin-top:0">{changed
        ? t("This is not the certificate trusted before. If the server's certificate was not renewed or replaced, someone may be between this engine and the server.")
        : t("This is the first time this certificate is seen. Check that its SHA-256 fingerprint matches the one the server shows before trusting it.")}</p>
      <p class="mono">{fingerprint}</p>
    </>,
    ok: changed ? t("Trust the new certificate") : t("Trust"),
    danger: changed,
  });
}
async function removeServer(s: NetServer) {
  if (!await ask({ title: t("Remove {name}?", { name: s.name }), body: <p style="margin-top:0">{t("Its files stay on the server; jobs that use it will fail.")}</p>, ok: t("Remove"), danger: true })) return;
  try { await api("DELETE", "/api/servers/" + encodeURIComponent(s.id)); toast(t("Server removed"), "ok"); } catch (e: any) { toast(tMsg(e.message), "bad"); }
  loadInfo().catch(() => {});
}

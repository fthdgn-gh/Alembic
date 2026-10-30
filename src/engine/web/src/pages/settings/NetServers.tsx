// SPDX-License-Identifier: GPL-3.0-or-later
// Network servers games can be read from and saved to: SMB shares, NFS exports, WebDAV and FTP.
import { useState } from "preact/hooks";
import type { NetServer, Protocol } from "../../api/types";
import { api } from "../../api/client";
import { servers, loadInfo } from "../../state/engine";
import { serverAddress } from "../../state/paths";
import { t, tMsg } from "../../lib/i18n";
import { ICONS } from "../../components/Icons";
import { Sheet, openOverlay, closeOverlay } from "../../components/Overlay";
import { ask } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
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
        <p class="muted" style="margin:0 0 .5rem">{t("Games on a PC or NAS can be converted from there over SMB (shared folders), NFS, WebDAV or FTP, and exFAT, FFPKG, FFPFS and folder outputs saved there. FFPFSC and PKG outputs must be saved on the console first. Nothing is encrypted: files cross the network as they are, FTP and WebDAV send the password unencrypted too, and passwords are kept on the console.")}</p>
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
  const f = useForm();
  const def = PROTOCOLS.find((p) => p.value === protocol)!;
  async function save() {
    if (!host.trim()) { f.setMsg({ text: t("Enter the server's address"), err: true }); return; }
    f.setBusy(true); f.setMsg({ text: t("Connecting…"), err: false });
    try {
      const body: Record<string, unknown> = { protocol, name: name.trim(), host: host.trim(), port: parseInt(port, 10) || 0, user: user.trim(), root: root.trim() };
      // Editing: an empty password keeps the saved one.
      if (!s || pass) body.password = pass;
      if (s) await api("PUT", "/api/servers/" + encodeURIComponent(s.id), body);
      else await api("POST", "/api/servers", body);
      toast(s ? t("Server saved") : t("Server added"), "ok");
      closeOverlay(true);
      loadInfo().catch(() => {});
    } catch (e: any) { f.setMsg({ text: tMsg(e.message), err: true }); f.setBusy(false); }
  }
  return (
    <Sheet title={s ? t("Edit {name}", { name: s.name }) : t("Add a server")} cls="narrow" foot={<><f.Msg /><button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={f.busy} onClick={save}>{s ? t("Save") : t("Add")}</button></>}>
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
        <Field label={t("Port")} inputMode="numeric" placeholder={String(def.port)} value={port} onInput={setPort} />
        {protocol === "nfs"
          ? <Field label={t("User and group ID")} placeholder={t("uid:gid like 1000:1000; empty: 0:0")} value={user} onInput={setUser} />
          : <Field label={t("User")} placeholder={protocol === "ftp" ? "anonymous" : protocol === "smb" ? t("Empty for a guest login") : ""} value={user} onInput={setUser} />}
        {protocol === "nfs" ? null : <Field label={t("Password")} type="password" placeholder={s ? t("Unchanged") : undefined} value={pass} onInput={setPass} />}
      </div>
    </Sheet>
  );
}
async function removeServer(s: NetServer) {
  if (!await ask({ title: t("Remove {name}?", { name: s.name }), body: <p style="margin-top:0">{t("Its files stay on the server; jobs that use it will fail.")}</p>, ok: t("Remove"), danger: true })) return;
  try { await api("DELETE", "/api/servers/" + encodeURIComponent(s.id)); toast(t("Server removed"), "ok"); } catch (e: any) { toast(tMsg(e.message), "bad"); }
  loadInfo().catch(() => {});
}

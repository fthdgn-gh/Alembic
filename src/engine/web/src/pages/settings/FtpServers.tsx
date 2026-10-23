// SPDX-License-Identifier: GPL-3.0-or-later
// FTP servers games can be read from and saved to.
import { useState } from "preact/hooks";
import type { FtpServer } from "../../api/types";
import { api } from "../../api/client";
import { ftp, loadInfo } from "../../state/engine";
import { t, tMsg } from "../../lib/i18n";
import { ICONS } from "../../components/Icons";
import { Sheet, openOverlay, closeOverlay } from "../../components/Overlay";
import { ask } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { Head, SmallBtn, Field, useForm, formHints } from "./parts";

export function FtpServers() {
  return (
    <>
      <Head text={t("FTP servers")} />
      <div class="card">
        <p class="muted" style="margin:0 0 .5rem">{t("Games on another computer's FTP server can be converted from there, and exFAT, FFPKG, FFPFS and folder outputs saved there. FFPFSC and PKG outputs must be saved on the console first. Plain FTP: the password crosses the network unencrypted and is kept on the console.")}</p>
        {ftp.value.map((s) => (
          <div class="ftprow">
            <div class="nm">{ICONS.server()}</div>
            <div style="min-width:0;flex:1"><div class="ellip">{s.name}</div><div class="muted mono ellip" style="font-size:.85rem">{s.user + "@" + s.host + ":" + s.port}</div></div>
            <SmallBtn k={"ftp:ed:" + s.id} text={t("Edit")} onClick={() => ftpDialog(s)} />
            <SmallBtn k={"ftp:rm:" + s.id} cls="danger" text={t("Remove")} onClick={() => removeFtp(s)} />
          </div>
        ))}
        <div class="btns"><SmallBtn k="ftp:add" text={t("Add an FTP server…")} onClick={() => ftpDialog(null)} /></div>
      </div>
    </>
  );
}

// Adds an FTP server, or changes `server`; the engine logs in once before keeping it.
function ftpDialog(server: FtpServer | null) { openOverlay(() => <FtpForm server={server} />, formHints()); }
function FtpForm(props: { server: FtpServer | null }) {
  const s = props.server;
  const [name, setName] = useState(s ? s.name : ""), [host, setHost] = useState(s ? s.host : ""), [port, setPort] = useState(String(s ? s.port : 21));
  const [user, setUser] = useState(s ? s.user : "anonymous"), [pass, setPass] = useState("");
  const f = useForm();
  async function save() {
    if (!host.trim()) { f.setMsg({ text: t("Enter the server's address"), err: true }); return; }
    f.setBusy(true); f.setMsg({ text: t("Connecting…"), err: false });
    try {
      const body: Record<string, unknown> = { name: name.trim() || host.trim(), host: host.trim(), port: parseInt(port, 10) || 21, user: user.trim() || "anonymous" };
      // Editing: an empty password keeps the saved one.
      if (!s || pass) body.password = pass;
      if (s) await api("PUT", "/api/ftp/" + encodeURIComponent(s.id), body);
      else await api("POST", "/api/ftp", body);
      toast(s ? t("FTP server saved") : t("FTP server added"), "ok");
      closeOverlay(true);
      loadInfo().catch(() => {});
    } catch (e: any) { f.setMsg({ text: tMsg(e.message), err: true }); f.setBusy(false); }
  }
  return (
    <Sheet title={s ? t("Edit {name}", { name: s.name }) : t("Add an FTP server")} cls="narrow" foot={<><f.Msg /><button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={f.busy} onClick={save}>{s ? t("Save") : t("Add")}</button></>}>
      <div>
        <Field label={t("Name")} placeholder={t("My PC")} value={name} onInput={setName} />
        <Field label={t("Address")} placeholder="192.168.1.10" value={host} onInput={setHost} />
        <Field label={t("Port")} inputMode="numeric" value={port} onInput={setPort} />
        <Field label={t("User")} value={user} onInput={setUser} />
        <Field label={t("Password")} type="password" placeholder={s ? t("Unchanged") : undefined} value={pass} onInput={setPass} />
      </div>
    </Sheet>
  );
}
async function removeFtp(s: FtpServer) {
  if (!await ask({ title: t("Remove {name}?", { name: s.name }), body: <p style="margin-top:0">{t("Its files stay on the server; jobs that use it will fail.")}</p>, ok: t("Remove"), danger: true })) return;
  try { await api("DELETE", "/api/ftp/" + encodeURIComponent(s.id)); toast(t("FTP server removed"), "ok"); } catch (e: any) { toast(tMsg(e.message), "bad"); }
  loadInfo().catch(() => {});
}

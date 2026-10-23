// SPDX-License-Identifier: GPL-3.0-or-later
// Settings: the console shortcut, the page, Places, other engines, FTP
// servers, the engine itself and its cache.
import { useEffect, useState } from "preact/hooks";
import type { FtpServer, Peer, Peers } from "./types";
import { info, locations, peers, ftp, jobs, loadInfo, loadPeers, shownPath, tab } from "./state";
import { api, bytes, store, LOCAL_PAGE } from "./util";
import { t, tMsg, tStatus, plural, lang, LANGS } from "./i18n";
import { ICONS, ChoiceRow, SwitchRow, Facts, clearMeta } from "./ui";
import { Sheet, ask, openOverlay, closeOverlay, overlays, topOverlay } from "./overlay";
import { goBack, inputLog } from "./nav";
import { setPlace } from "./files";
import { toast } from "./toasts";

const SCALES = [{ value: 0.85, label: "Small" }, { value: 1, label: "Normal" }, { value: 1.15, label: "Large" }, { value: 1.3, label: "Larger" }];
export function applyScale() {
  const w = window.innerWidth, s = store.get<number>("scale") || 1;
  const px = w >= 2400 ? 26 : w >= 1700 ? 20 : w >= 1200 ? 17 : w >= 760 ? 16 : 15;
  document.documentElement.style.fontSize = (px * s).toFixed(1) + "px";
}

export function setLang(v: string) {
  lang.value = v;
  store.set("lang", v);
  api("POST", "/api/lang", { lang: v }).catch(() => {});
  document.documentElement.lang = v;
}

const Head = (props: { text: string }) => <div class="head2"><div class="sect">{props.text}</div></div>;
const SmallBtn = (props: { k: string; text: string; cls?: string; onClick: () => void; disabled?: boolean }) =>
  <button class={"btn sm" + (props.cls ? " " + props.cls : "")} data-key={props.k} disabled={props.disabled} onClick={props.onClick}>{props.text}</button>;

export function SettingsView(props: { hidden: boolean }) {
  const i = info.value;
  const [scale, setScale] = useState(store.get<number>("scale") || 1);
  // Other engines come and go: asked again every few seconds while this is open.
  useEffect(() => {
    if (tab.value !== "settings") return;
    const timer = setInterval(() => { const p = peers.value; if (p && (p.enabled || p.engines.length)) loadPeers().catch(() => {}); }, 4000);
    return () => clearInterval(timer);
  }, [tab.value]);
  return (
    <section class="view scroll" id="view-settings" hidden={props.hidden}>
      <div class="page" data-zone="settings" id="settings">
        <Head text={t("Console")} />
        <Launcher />
        <Head text={t("Page")} />
        <ChoiceRow dataKey="s:lang" label={t("Language")} options={LANGS.map((l) => ({ value: l[0], label: l[1] }))} value={lang.value} onChange={setLang} />
        <ChoiceRow dataKey="s:scale" label={t("Text size")} hint={t("For reading from the couch")} options={SCALES.map((x) => ({ value: x.value, label: t(x.label) }))} value={scale}
          onChange={(v) => { setScale(v); store.set("scale", v); applyScale(); }} />
        <button class="frow" data-key="s:test" onClick={openControllerTest}>
          <div class="fl"><b>{t("Controller test")}</b><small>{t("Shows what each button sends to this page")}</small></div><span class="arr">›</span>
        </button>
        <Places />
        <Engines />
        <FtpServers />
        <Head text={t("Engine")} />
        <div class="card">
          <Facts rows={!i ? [[t("Engine"), t("not connected")]] : ([
            [t("Version"), i.version],
            [t("Running on"), i.platform === "ps5" ? t("PS5 (payload)") : i.os || t("PC")],
            i.host_name && i.platform !== "ps5" ? [t("Computer"), i.host_name] : null,
            i.cpu_instructions ? [t("CPU instructions"), i.cpu_instructions === "none" ? "–" : i.cpu_instructions] : null,
            [t("Address"), location.host, "mono"],
            [t("From other devices"), i.network_urls && i.network_urls.length ? i.network_urls.join("\n") : i.open_to_network ? "–" : t("This computer only"), "mono"],
            i.roots && i.roots.length ? [t("Shared folders"), i.roots.join("\n"), "mono"] : null,
            [t("State folder"), i.state_dir, "mono"],
            [t("LZ4 runtime"), i.lz4_runtime && i.lz4_runtime.present ? (i.lz4_runtime.pinned ? "ampr_emu 0.4.2.1" : t("present, not the pinned build")) : t("not downloaded")],
          ].filter(Boolean) as [string, string, string?][])} />
          <div class="btns"><SmallBtn k="s:quit" cls="danger" text={t("Stop the engine")} onClick={quitEngine} /></div>
        </div>
        {i && i.cache ? <Cache /> : null}
      </div>
    </section>
  );
}

function Launcher() {
  const i = info.value, l = i && i.launcher;
  const [busy, setBusy] = useState(false);
  if (!(l && l.supported)) return (
    <div class="frow"><div class="fl"><b>{t("Shortcut in Media")}</b>
      <small>{l && i!.platform === "ps5" ? tStatus(l.status || "not available") : t("Only when the engine runs on a PS5.")}</small></div></div>
  );
  return (
    <>
      <SwitchRow dataKey="s:launcher" label={t("Shortcut in Media")} hint={t("Adds Alembic to the Media tab of the home screen; it opens this page in the console's browser")} on={l.installed}
        onChange={async (on) => {
          try { info.value = await api("POST", "/api/launcher", { enabled: on }); toast(on ? t("Added to Media on the home screen") : t("Shortcut removed"), "ok"); }
          catch (e: any) { toast(tMsg(e.message), "bad"); }
        }} />
      <div class="card">
        <Facts rows={[[t("Status"), tStatus(l.status || ""), /failed|not available/.test(l.status) ? "err" : ""], [t("Title ID"), l.title_id, "mono"]]} />
        <div class="btns"><SmallBtn k="s:again" text={t("Register again")} disabled={busy} onClick={async () => {
          setBusy(true);
          try { info.value = await api("POST", "/api/launcher", { enabled: true, again: true }); toast(t("Shortcut registered again; check Media on the home screen"), "ok"); }
          catch (e: any) { toast(tMsg(e.message), "bad"); }
          loadInfo().catch(() => {}).then(() => setBusy(false));
        }} /></div>
      </div>
    </>
  );
}

// Places the person added, here and on paired engines.
function Places() {
  const mine = locations.value.filter((l) => l.custom);
  return (
    <>
      <Head text={t("Places")} />
      <div class="card">
        {!mine.length ? <p class="muted" style="margin:0">{t("No places yet: in Files, open a folder's Options and choose Add to Places. They show here and on paired engines.")}</p> : null}
        {mine.map((l) => (
          <div class="ftprow">
            <div class="nm">{ICONS.place()}</div>
            <div style="min-width:0;flex:1"><div class="ellip">{l.place || l.label}</div><div class="muted mono ellip" style="font-size:.85rem">{shownPath(l.path)}</div></div>
            <SmallBtn k={"place:rm:" + l.path} cls="danger" text={t("Remove")} onClick={() => setPlace(l.path, false)} />
          </div>
        ))}
      </div>
    </>
  );
}

// Other Alembic engines on the network (discovery is off until switched on here).
function Engines() {
  const p: Peers = peers.value || { enabled: false, engines: [] };
  return (
    <>
      <Head text={t("Other engines")} />
      <SwitchRow dataKey="s:discovery" label={t("Find engines on this network")}
        hint={t("Announces this engine on the local network every few seconds and lists the other Alembic engines that do the same. Off by default.")} on={p.enabled}
        onChange={async (on) => {
          try { peers.value = await api("POST", "/api/peers/discovery", { enabled: on }); setTimeout(() => loadPeers().catch(() => {}), 1500); }
          catch (e: any) { toast(tMsg(e.message), "bad"); }
        }} />
      <div class="card">
        {p.enabled && p.listening === false ? <p class="err" style="margin:0 0 .5rem">{t("The discovery port could not be opened; another program may be using it.")}</p> : null}
        {!p.engines.length ? <p class="muted" style="margin:0 0 .5rem">{p.enabled ? t("No other engines found yet.") : t("Switch on finding engines, or add one by its address.")}</p> : null}
        {p.engines.map((e) => <EngineRow e={e} />)}
        <div class="btns"><SmallBtn k="peer:add" text={t("Add an engine by address…")} onClick={peerDialog} /></div>
      </div>
    </>
  );
}
function EngineRow(props: { e: Peer }) {
  const e = props.e;
  const sub = [e.os || e.platform, e.address + ":" + e.port, e.version ? "v" + e.version : ""].filter(Boolean).join(" · ");
  const btn = (key: string, text: string, fn: () => void, cls?: string) => <SmallBtn k={"peer:" + key + ":" + e.id} text={text} cls={cls} onClick={fn} />;
  const actions = e.paired ? [btn("check", t("Check"), () => checkPeer(e)), btn("unpair", t("Unpair"), () => unpairPeer(e), "danger")]
    : [e.waiting_for_code ? btn("code", t("Enter the code"), () => codeDialog(e)) : btn("pair", t("Pair"), () => pairPeer(e), "primary"),
      e.manual ? btn("rm", t("Remove"), () => removePeer(e), "danger") : null];
  const note = e.manual && !e.reachable ? <div class="err ellip" style="font-size:.85rem">{t("Not reachable: {why}", { why: tMsg(e.problem || "") })}</div>
    : e.paired ? <div class="good" style="font-size:.85rem">{t("Paired")}</div>
    : e.manual ? <div class="muted" style="font-size:.85rem">{t("Added by address")}</div> : null;
  return (
    <div class="ftprow">
      <div class="nm">{ICONS.server()}</div>
      <div style="min-width:0;flex:1"><div class="ellip">{e.name}</div><div class="muted mono ellip" style="font-size:.85rem">{sub}</div>{note}</div>
      <a class="btn sm" data-key={"peer:open:" + e.id} href={e.url} target="_blank" rel="noopener">{t("Open its page")}</a>
      {actions}
    </div>
  );
}

function FtpServers() {
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

// Titles and icons the engine read from games; read again when needed.
function Cache() {
  const c = info.value!.cache;
  const [busy, setBusy] = useState(false);
  return (
    <>
      <Head text={t("Cache")} />
      <div class="card">
        <Facts rows={[[t("Game info"), plural(c.games, "{n} game", "{n} games") + " · " + bytes(c.bytes)]]} />
        <p class="muted" style="margin:.5rem 0 0">{t("Titles and icons, so lists open quickly. Entries unused for 30 days are removed at start, and the cache is kept under 100 MB.")}</p>
        <div class="btns"><SmallBtn k="s:cache" text={t("Clear cache")} disabled={busy || !(c.games || c.bytes)} onClick={async () => {
          setBusy(true);
          try {
            const cache = await api("POST", "/api/cache/clear");
            info.value = Object.assign({}, info.value!, { cache });
            clearMeta();
            toast(t("Cache cleared"), "ok");
          } catch (e: any) { toast(tMsg(e.message), "bad"); }
          setBusy(false);
        }} /></div>
      </div>
    </>
  );
}

/* ---------- dialogs ---------- */
// A labelled text field of a dialog form.
function Field(props: { label: string; value: string; onInput: (v: string) => void; placeholder?: string; type?: string; inputMode?: string }) {
  return [
    <div class="lbl">{props.label}</div>,
    <input class="input mono" spellcheck={false} autocapitalize="off" type={props.type} placeholder={props.placeholder} inputMode={props.inputMode as any}
      value={props.value} onInput={(e) => props.onInput((e.target as HTMLInputElement).value)} />,
  ];
}
// A dialog form's message and buttons.
function useForm() {
  const [msg, setMsg] = useState<{ text: string; err: boolean }>({ text: "", err: false });
  const [busy, setBusy] = useState(false);
  return { msg, setMsg, busy, setBusy, Msg: () => <span class={"msg" + (msg.err ? " err" : "")}>{msg.text}</span> };
}
const formHints = () => ({ hints: [["x", t("Choose")], ["o", t("Cancel"), goBack]] as [string, string, (() => void)?][] });

function peerDialog() { openOverlay(() => <AddEngine />, formHints()); }
function AddEngine() {
  const [host, setHost] = useState(""), [port, setPort] = useState("5940");
  const f = useForm();
  async function add() {
    if (!host.trim()) { f.setMsg({ text: t("Enter the engine's address"), err: true }); return; }
    f.setBusy(true); f.setMsg({ text: t("Connecting…"), err: false });
    try {
      peers.value = await api("POST", "/api/peers", { address: host.trim(), port: parseInt(port, 10) || 5940 });
      toast(t("Engine added"), "ok");
      closeOverlay(true);
    } catch (e: any) { f.setMsg({ text: tMsg(e.message), err: true }); f.setBusy(false); }
  }
  return (
    <Sheet title={t("Add an engine")} cls="narrow" foot={<><f.Msg /><button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={f.busy} onClick={add}>{t("Add")}</button></>}>
      <div>
        <Field label={t("Address")} placeholder="192.168.1.10" value={host} onInput={setHost} />
        <Field label={t("Port")} inputMode="numeric" value={port} onInput={setPort} />
      </div>
    </Sheet>
  );
}

async function removePeer(e: Peer) {
  if (!await ask({ title: t("Remove {name}?", { name: e.name }), body: <p style="margin-top:0">{t("Nothing changes on that engine; it can be added again.")}</p>, ok: t("Remove"), danger: true })) return;
  try { peers.value = await api("DELETE", "/api/peers/" + encodeURIComponent(e.id)); toast(t("Engine removed"), "ok"); } catch (err: any) { toast(tMsg(err.message), "bad"); }
}
// Pairing (pairing.hpp): started here, the code is typed here.
async function pairPeer(e: Peer) {
  toast(t("Connecting…"), "ok");
  try { peers.value = await api("POST", "/api/peers/" + encodeURIComponent(e.id) + "/pair", {}); codeDialog(e); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}
function codeDialog(e: Peer) {
  const state = { done: false };
  openOverlay(() => <PairCode e={e} state={state} />, formHints())
    .then(() => {
      if (!state.done) api<Peers>("DELETE", "/api/peers/" + encodeURIComponent(e.id) + "/pair/session").then((p) => { peers.value = p; }).catch(() => {});
    });
}
function PairCode(props: { e: Peer; state: { done: boolean } }) {
  const e = props.e;
  const [code, setCode] = useState("");
  const f = useForm();
  async function pair() {
    if (code.replace(/\D/g, "").length !== 6) { f.setMsg({ text: t("Enter the six-digit code"), err: true }); return; }
    f.setBusy(true); f.setMsg({ text: t("Connecting…"), err: false });
    try {
      peers.value = await api("POST", "/api/peers/" + encodeURIComponent(e.id) + "/pair/confirm", { code });
      props.state.done = true; closeOverlay(true); toast(t("Paired with {name}", { name: e.name }), "ok");
    } catch (err: any) {
      f.setMsg({ text: tMsg(err.message), err: true }); f.setBusy(false);
      // Too many wrong codes end the attempt there: close this too.
      loadPeers().then(() => { if (!peers.value!.engines.some((x) => x.id === e.id && x.waiting_for_code)) { props.state.done = true; closeOverlay(null); } }).catch(() => {});
    }
  }
  return (
    <Sheet title={t("Pair with {name}", { name: e.name })} cls="narrow" foot={<><f.Msg /><button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={f.busy} onClick={pair}>{t("Pair")}</button></>}>
      <div>
        <p style="margin-top:0">{t("Enter the code {name} shows. On a PS5 it is a notification on the screen; on a computer, it is in the engine's window, or in its page opened on that computer.", { name: e.name })}</p>
        <input class="input mono" inputMode="numeric" autocomplete="off" maxLength={7} placeholder="123 456" style="font-size:1.6rem;letter-spacing:.2em;text-align:center"
          value={code} onInput={(ev) => setCode((ev.target as HTMLInputElement).value)} />
      </div>
    </Sheet>
  );
}
async function checkPeer(e: Peer) {
  try { await api("GET", "/api/peers/" + encodeURIComponent(e.id) + "/pair/check"); toast(t("The pairing works"), "ok"); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}
async function unpairPeer(e: Peer) {
  if (!await ask({ title: t("Unpair {name}?", { name: e.name }), body: <p style="margin-top:0">{t("Both engines forget their shared key; pair them again to use them together.")}</p>, ok: t("Unpair"), danger: true })) return;
  try { peers.value = await api("DELETE", "/api/peers/" + encodeURIComponent(e.id) + "/pair"); toast(t("Unpaired"), "ok"); } catch (err: any) { toast(tMsg(err.message), "bad"); }
}

// Started there, the code is shown here: only in a browser on this computer
// (the engine shows it to nobody else).
let incoming: { from: string; id: number } | null = null;
export async function watchIncoming() {
  if (!LOCAL_PAGE) return;
  const r = await api<{ pairing?: { code: string; from: string } }>("GET", "/api/pair/incoming").catch(() => null);
  const p = r && r.pairing;
  if (p && p.code && !incoming) {
    const code = p.code, from = p.from;
    const open = openOverlay(() => (
      <Sheet title={t("{name} wants to pair with this engine", { name: from })} cls="narrow"
        foot={<button class="btn" onClick={() => { api("POST", "/api/pair/incoming/cancel", {}).catch(() => {}); closeOverlay(null); }}>{t("Cancel")}</button>}>
        <div>
          <p style="margin-top:0">{t("Enter this code on {name}:", { name: from })}</p>
          <div class="mono" style="font-size:2.6rem;letter-spacing:.15em;text-align:center;margin:.6rem 0">{code}</div>
        </div>
      </Sheet>
    ), { hints: [["o", t("Cancel"), goBack]] });
    const top = topOverlay();
    incoming = { from, id: top ? top.id : 0 };
    open.then(() => { incoming = null; });
  } else if (!p && incoming) {
    const from = incoming.from, top = topOverlay();
    if (top && top.id === incoming.id) closeOverlay(true); // paired or expired: only our dialog
    incoming = null;
    await loadPeers().catch(() => {});
    if (peers.value && peers.value.engines.some((x) => x.name === from && x.paired)) toast(t("Paired with {name}", { name: from }), "ok");
  }
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

async function quitEngine() {
  const running = jobs.value.some((j) => j.state === "running");
  const body = <div><p style="margin-top:0">{t("This page stops working until the engine is loaded again.")}</p>
    {running ? <div class="warnbox">{t("A job is running; it will be cancelled.")}</div> : null}</div>;
  if (!await ask({ title: t("Stop the engine?"), body, ok: t("Stop"), danger: true })) return;
  try { await api("POST", "/api/quit"); toast(t("Engine stopped"), "ok"); } catch (e: any) { toast(tMsg(e.message), "bad"); }
}

// Shows what each button sends; nothing acts while it is open, except ✕ on Close.
function openControllerTest() {
  openOverlay(() => <ControllerTest />, { hints: [["x", t("Close (press it last)")]] });
}
function ControllerTest() {
  const [start] = useState(Date.now());
  const [, redraw] = useState(0);
  useEffect(() => {
    inputLog.test = () => redraw((n) => n + 1);
    return () => { inputLog.test = null; };
  }, []);
  const items = inputLog.items.filter((x) => x.t >= start);
  return (
    <Sheet title={t("Controller test")} cls="wide" foot={
      <button class="btn primary" data-test-done data-autofocus onClick={() => { inputLog.test = null; inputLog.closing = true; closeOverlay(null); }}>{t("Close")}</button>}>
      <div>
        <p style="margin-top:0">{t("Press each button once: D-pad, ○, △, □, L1, R1, L2, R2, Options. Each one shows below what the browser sends. Press ✕ last: it closes this.")}</p>
        <div class="card" style="min-height:16rem">
          <div class="mono" style="font-size:.85rem;line-height:1.7">
            {!items.length ? <div class="muted">{t("Waiting for a button…")}</div> : null}
            {items.slice(0, 16).map((x) => <div>{((x.t - start) / 1000).toFixed(1) + " s  " + x.text}</div>)}
          </div>
        </div>
      </div>
    </Sheet>
  );
}

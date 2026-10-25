// SPDX-License-Identifier: GPL-3.0-or-later
// Other engines: finding them, adding one by address, pairing (pairing.hpp
// in the engine), and the code shown here when another engine pairs.
import { useState } from "preact/hooks";
import type { Peer, Peers } from "../../api/types";
import { api } from "../../api/client";
import { info, peers, loadPeers } from "../../state/engine";
import { LOCAL_PAGE } from "../../lib/env";
import { t, tMsg } from "../../lib/i18n";
import { ICONS } from "../../components/Icons";
import { SwitchRow } from "../../components/Rows";
import { Sheet, openOverlay, closeOverlay, topOverlay } from "../../components/Overlay";
import { ask } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { goBack } from "../../input/history";
import { Head, SmallBtn, Field, useForm, formHints } from "./parts";

// Other Alembic engines on the network (discovery is off until switched on here).
export function Engines() {
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
  const sub = [e.os || e.platform, e.address ? e.address + ":" + e.port : "", e.version ? "v" + e.version : ""].filter(Boolean).join(" · ");
  const btn = (key: string, text: string, fn: () => void, cls?: string) => <SmallBtn k={"peer:" + key + ":" + e.id} text={text} cls={cls} onClick={fn} />;
  // Engines that talk another way (another protocol) cannot work with this one.
  const ours = (info.value && info.value.protocol) || 1, theirs = e.protocol || 1;
  const mismatch = theirs !== ours;
  const actions = e.paired ? [btn("check", t("Check"), () => checkPeer(e)), btn("unpair", t("Unpair"), () => unpairPeer(e), "danger")]
    : [mismatch ? null : e.waiting_for_code ? btn("code", t("Enter the code"), () => codeDialog(e)) : btn("pair", t("Pair"), () => pairPeer(e), "primary"),
      e.manual ? btn("rm", t("Remove"), () => removePeer(e), "danger") : null];
  const note = mismatch ? <div class="err" style="font-size:.85rem">{theirs < ours ? t("Older Alembic: update it to use it with this engine") : t("Newer Alembic: update this engine to use them together")}</div>
    : e.manual && !e.reachable ? <div class="err ellip" style="font-size:.85rem">{t("Not reachable: {why}", { why: tMsg(e.problem || "") })}</div>
    : e.paired && e.offline ? <div class="muted" style="font-size:.85rem">{t("Paired · offline")}</div>
    : e.paired ? <div class="good" style="font-size:.85rem">{t("Paired")}</div>
    : e.manual ? <div class="muted" style="font-size:.85rem">{t("Added by address")}</div> : null;
  return (
    <div class="ftprow">
      <div class="nm">{ICONS.server()}</div>
      <div style="min-width:0;flex:1"><div class="ellip">{e.name}</div><div class="muted mono ellip" style="font-size:.85rem">{sub}</div>{note}</div>
      {e.url ? <a class="btn sm" data-key={"peer:open:" + e.id} href={e.url} target="_blank" rel="noopener">{t("Open its page")}</a> : null}
      {actions}
    </div>
  );
}

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

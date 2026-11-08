// SPDX-License-Identifier: GPL-3.0-or-later
// What can be done to the ticked entries at once (Files or Images > □ > Select, then □).
// Each action takes the entries it applies to and says so when that is not all.
import { useEffect, useState } from "preact/hooks";
import type { Entry, NameFormat, Plan, Dep, ImageItem } from "../../api/types";
import { api } from "../../api/client";
import { cwd, listing, tab } from "../../state/view";
import { loadImages } from "../../state/images";
import { amprUpdatable, updateAmpr } from "../images/ampr";
import { info, locations, peerDrives, loadInfo } from "../../state/engine";
import { refreshJobs } from "../../state/jobs";
import { selectedEntries, selectAll, selectableCount, clearSelected, stopSelecting } from "../../state/selection";
import { isConvertible, isHiddenName } from "../../lib/kinds";
import { bytes, dir } from "../../lib/format";
import { t, tMsg, plural } from "../../lib/i18n";
import { ask, choose } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { Sheet, openOverlay, closeOverlay } from "../../components/Overlay";
import { ChoiceRow, SwitchRow } from "../../components/Rows";
import { goBack } from "../../input/history";
import { openConvert } from "../../dialogs/ConvertDialog";
import { openDestMany } from "../../dialogs/FolderPicker";
import { getNeeds } from "../../dialogs/getNeeds";
import { browse } from "./browse";

type Item = { label: string; sub?: string; danger?: boolean; value: () => void };

// In Images: the same, less Copy, Move, renaming and plain Hide, plus the
// update of the AMPR emulator of the ticked ones that have a newer build.
export async function openBulkActions() {
  const all = selectedEntries.value, n = all.length;
  const lib = tab.value === "images";
  const total = selectableCount();
  const items: Item[] = [];
  // "3 of 5": an action that applies to only some of the ticked entries.
  const some = (k: number, sub?: string) => (k < n ? t("{n} of {total} selected", { n: k, total: n }) + (sub ? " · " + sub : "") : sub);
  const games = all.filter((e) => isConvertible(e) && !e.shared);
  const images = games.filter((e) => e.kind !== "fpkg"), packages = games.filter((e) => e.kind === "fpkg");
  const onPs5 = !!info.value && info.value.platform === "ps5";
  if (games.length) items.push({ label: t("Convert…"), sub: some(games.length, t("To FFPFSC, FFPKG, exFAT or a folder")), value: () => openConvert(games) });
  if (images.length && onPs5) items.push({ label: t("Install…"), sub: some(images.length, t("Converts them to packages, installs them, then deletes the packages")), value: () => openConvert(images, true) });
  if (packages.length) items.push({ label: t("Install"), sub: some(packages.length, t("Hand them to the console's installer")), value: () => installMany(packages) });
  const updatable = lib ? (all as ImageItem[]).filter(amprUpdatable) : [];
  if (updatable.length) items.push({ label: t("Update the AMPR emulator"), sub: some(updatable.length, t("The newest build of each one's emulator")), value: () => updateAmpr(updatable).then(done) });
  if (images.length) items.push({ label: t("Add the AMPR emulator…"), sub: some(images.length, t("For games that stop at start from an image or folder")), value: () => amprMany(images) });
  const visible = images.filter((e) => !isHiddenName(e.name)), hidden = images.filter((e) => isHiddenName(e.name));
  if (visible.length) items.push({ label: t("Hide from ShadowMount+"), sub: some(visible.length, t("Renames them to .name, so ShadowMount+ uses other copies of these games")), value: () => hideMany(visible, true) });
  if (hidden.length) items.push({ label: t("Show to ShadowMount+ again"), sub: some(hidden.length, t("Renames them back without the leading dot")), value: () => hideMany(hidden, false) });
  // Any other file or folder (packages too): hidden by the leading dot alone.
  const plain = all.filter((e) => !e.shared && images.indexOf(e) < 0);
  const plainShown = plain.filter((e) => !isHiddenName(e.name)), plainHidden = plain.filter((e) => isHiddenName(e.name));
  if (plainShown.length && !lib) items.push({ label: t("Hide"), sub: some(plainShown.length, t("Renames them to .name")), value: () => hideMany(plainShown, true, false) });
  if (plainHidden.length && !lib) items.push({ label: t("Unhide"), sub: some(plainHidden.length, t("Renames them back without the leading dot")), value: () => hideMany(plainHidden, false, false) });
  if (games.length && !lib) items.push({ label: t("Rename with a format…"), sub: some(games.length, t("Names from each game's title, title ID, version and format")), value: () => renameMany(games) });
  const local = all.filter((e) => !e.shared);
  const peerPlace = locations.value.find((l) => !!l.peer) || peerDrives.value.map((d) => ({ path: d.root, label: d.engine }))[0];
  if (n) {
    if (peerPlace && local.length) items.push({ label: t("Send to another engine…"), sub: some(local.length, t("Copies as they are, without converting")), value: () => copyMany(local, "copy", peerPlace.path) });
    if (!lib) items.push({ label: t("Copy to…"), sub: some(local.length), value: () => copyMany(local, "copy") },
      { label: t("Move to…"), sub: some(local.length), value: () => copyMany(local, "move") });
    items.push({ label: t("Delete…"), danger: true, sub: some(local.length), value: () => deleteMany(local) });
  }
  if (n < total) items.push({ label: t("Select all"), value: selectAll });
  if (n) items.push({ label: t("Clear the selection"), value: clearSelected });
  const fn = await choose(n ? plural(n, "{n} selected", "{n} selected") : t("Nothing selected"), items);
  if (fn) fn();
}

// The names, for a dialog: the first few, then how many more.
function names(list: Entry[], max = 6) {
  const shown = list.slice(0, max).map((e) => e.name).join("\n");
  return list.length > max ? shown + "\n" + t("and {n} more", { n: list.length - max }) : shown;
}
function done() { stopSelecting(); if (tab.value === "images") loadImages(true); else if (cwd.value) browse(cwd.value, false); }
function failures(failed: string[]) {
  if (failed.length) toast(failed.length > 1 ? t("{first} (and {n} more)", { first: failed[0], n: failed.length - 1 }) : failed[0], "bad");
}

async function installMany(list: Entry[]) {
  if (!onConsole()) return;
  const body = <div><div class="mono" style="white-space:pre-line">{names(list)}</div>
    <p class="muted">{t("The console installs them in the background, one after another; follow them in Downloads. Keep the files until the installs have finished.")}</p></div>;
  if (!await ask({ title: plural(list.length, "Install {n} package?", "Install {n} packages?"), body, ok: t("Install") })) return;
  const failed: string[] = [];
  let queued = 0;
  for (const e of list) {
    try { await api("POST", "/api/jobs", { kind: "install", input: e.path }); queued++; } catch (err: any) { failed.push(e.name + ": " + tMsg(err.message)); }
  }
  if (queued) { toast(plural(queued, "{n} install queued", "{n} installs queued"), "ok"); refreshJobs(); done(); }
  failures(failed);
}
function onConsole() {
  if (info.value && info.value.platform === "ps5") return true;
  toast(t("Installing needs the engine running on a PS5"), "bad");
  return false;
}

// ampr_emu for each game that has none yet.
async function amprMany(list: Entry[]) {
  const plans: { e: Entry; p: Plan }[] = [];
  const failed: string[] = [];
  toast(t("Checking {n} games…", { n: list.length }));
  for (const e of list) {
    try { plans.push({ e, p: await api<Plan>("POST", "/api/plan", { kind: "ampr_emu", input: e.path }) }); }
    catch (err: any) { failed.push(e.name + ": " + tMsg(err.message)); }
  }
  const todo = plans.filter((x) => !x.p.ships_emulator), have = plans.length - todo.length;
  if (!todo.length) { failures(failed); if (have) toast(t("These games already have the AMPR emulator"), "ok"); return; }
  const body = <div><div class="mono" style="white-space:pre-line">{names(todo.map((x) => x.e))}</div>
    <p class="muted">{t("Game folders get it in their fakelib; images are not changed: ShadowMount+ gets a backport folder for each.")}</p>
    {have ? <p class="muted">{plural(have, "{n} game already has it.", "{n} games already have it.")}</p> : null}</div>;
  if (!await ask({ title: plural(todo.length, "Add the AMPR emulator to {n} game?", "Add the AMPR emulator to {n} games?"), body, ok: t("Add") })) return;
  const needs: Dep[] = [];
  for (const x of todo) for (const d of x.p.needs || []) if (!needs.some((y) => y.id === d.id)) needs.push(d);
  try { if (!await getNeeds({ output: "", needs }, "This needs {name}, which isn't downloaded yet.")) return; }
  catch (err: any) { toast(tMsg(err.message), "bad"); return; }
  let queued = 0;
  for (const x of todo) {
    try { await api("POST", "/api/jobs", { kind: "ampr_emu", input: x.e.path }); queued++; } catch (err: any) { failed.push(x.e.name + ": " + tMsg(err.message)); }
  }
  if (queued) { toast(plural(queued, "{n} job queued", "{n} jobs queued"), "ok"); refreshJobs(); done(); }
  failures(failed);
}

// ShadowMount+ skips names that start with ".": one rename each, one rescan.
// `smp`: images ShadowMount+ uses (it rescans and the toast says so).
async function hideMany(list: Entry[], hide: boolean, smp = true) {
  const failed: string[] = [];
  let changed = 0;
  for (const e of list) {
    const name = hide ? "." + e.name : e.name.slice(1);
    try { await api("POST", "/api/fs/rename", { path: e.path, name }); changed++; } catch (err: any) { failed.push(e.name + ": " + tMsg(err.message)); }
  }
  if (changed && !smp) {
    toast(hide ? plural(changed, "{n} hidden", "{n} hidden") : plural(changed, "{n} unhidden", "{n} unhidden"), "ok");
    done();
  }
  let scanned = true;
  if (changed && smp) { try { await api("POST", "/api/smp/scan"); } catch (err) { scanned = false; } }
  if (changed && smp) {
    toast((hide ? plural(changed, "{n} hidden from ShadowMount+", "{n} hidden from ShadowMount+") : plural(changed, "{n} visible to ShadowMount+ again", "{n} visible to ShadowMount+ again"))
      + (scanned ? " · " + t("it is rescanning") : ""), "ok");
    done();
  }
  failures(failed);
}

async function copyMany(list: Entry[], kind: "copy" | "move", start?: string) {
  if (await openDestMany(list, kind, start || dir(list[0].path))) done();
}

async function deleteMany(list: Entry[]) {
  const size = list.reduce((n, e) => n + (e.size || 0), 0), folders = list.some((e) => e.kind === "folder");
  const body = <div>
    <div class="warnbox">
      <b>{folders ? plural(list.length, "Delete {n} item, folders with everything in them?", "Delete {n} items, folders with everything in them?")
        : plural(list.length, "Delete {n} file?", "Delete {n} files?")}</b>
      <div class="mono" style="margin-top:.4rem;white-space:pre-line">{names(list)}</div>
      {size ? <div class="muted" style="margin-top:.2rem">{bytes(size)}</div> : null}
    </div>
    <p class="muted">{t("This can't be undone.")}</p>
  </div>;
  if (!await ask({ title: t("Delete"), body, ok: t("Delete"), danger: true })) return;
  const failed: string[] = [];
  let deleted = 0;
  for (const e of list) {
    try { await api("POST", "/api/fs/delete", { path: e.path }); deleted++; } catch (err: any) { failed.push(e.name + ": " + tMsg(err.message)); }
  }
  if (deleted) { toast(plural(deleted, "Deleted {n} item", "Deleted {n} items"), "ok"); done(); loadInfo().catch(() => {}); }
  failures(failed);
}

// ---- Rename with a format ----

interface Applied { path: string; name: string; warning: string; fitted: string }

async function renameMany(list: Entry[]) {
  let formats: NameFormat[];
  try { formats = (await api<{ formats: NameFormat[] }>("GET", "/api/names")).formats; }
  catch (err: any) { toast(tMsg(err.message), "bad"); return; }
  if (!formats.length) { toast(t("No rename formats: add one in Settings > Rename formats"), "bad"); return; }
  const go = { run: () => {} };
  const ok = await openOverlay<boolean>(() => <RenameMany list={list} formats={formats} go={go} />,
    { hints: [["x", t("Choose")], ["o", t("Cancel"), goBack], ["s", t("Rename"), () => go.run()]], onSquare: () => go.run() });
  if (ok) done();
}

function RenameMany(props: { list: Entry[]; formats: NameFormat[]; go: { run: () => void } }) {
  const { list, formats } = props;
  const [which, setWhich] = useState(0);
  const [shorten, setShorten] = useState(true);
  const [applied, setApplied] = useState<Record<string, Applied> | null>(null);
  const [msg, setMsg] = useState<{ text: string; err: boolean }>({ text: "", err: false });
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    let live = true;
    setApplied(null);
    const f = formats[which];
    api<{ results: Applied[] }>("POST", "/api/names/apply", { paths: list.map((e) => e.path), pattern: f.pattern, spaces: f.spaces, letters: f.letters })
      .then((r) => { if (!live) return; const by: Record<string, Applied> = {}; for (const a of r.results) by[a.path] = a; setApplied(by); },
        (err) => { if (live) setMsg({ text: tMsg(err.message), err: true }); });
    return () => { live = false; };
  }, [which]);

  // What happens to each: its new name, or why it is left as it is.
  const others = (listing.value ? listing.value.entries : []).filter((e) => list.indexOf(e) < 0).map((e) => e.name);
  const rows = list.map((e) => {
    const a = applied ? applied[e.path] : null;
    const name = !a ? "" : shorten && a.fitted ? a.fitted : a.name;
    return { e, a, name, why: "", note: a && a.warning && !(shorten && a.fitted) ? tMsg(a.warning) : a && shorten && a.fitted ? t("Shortened to fit ShadowMount+") : "" };
  });
  for (const r of rows) {
    if (!applied) continue;
    if (!r.name) r.why = t("This format has no value for it");
    else if (r.name === r.e.name) r.why = t("Already named so");
    else if (others.indexOf(r.name) >= 0 || rows.filter((x) => x.name === r.name).length > 1) r.why = t("Another file would have this name");
  }
  const todo = rows.filter((r) => applied && !r.why);

  async function run() {
    if (busy || !todo.length) return;
    setBusy(true);
    const failed: string[] = [];
    let renamed = 0;
    for (const r of todo) {
      try { await api("POST", "/api/fs/rename", { path: r.e.path, name: r.name }); renamed++; } catch (err: any) { failed.push(r.e.name + ": " + tMsg(err.message)); }
    }
    setBusy(false);
    if (renamed) toast(plural(renamed, "Renamed {n} item", "Renamed {n} items"), "ok");
    failures(failed);
    closeOverlay(renamed > 0);
  }
  props.go.run = run;

  return (
    <Sheet title={plural(list.length, "Rename {n} game", "Rename {n} games")} cls="narrow" foot={<>
      <span class={"msg" + (msg.err ? " err" : "")}>{msg.text}</span>
      <button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={busy || !todo.length} onClick={run}>{todo.length ? plural(todo.length, "Rename {n}", "Rename {n}") : t("Rename")}</button>
    </>}>
      <div>
        <ChoiceRow dataKey="rn:format" label={t("Format")} options={formats.map((f, i) => ({ value: i, label: f.pattern }))} value={which} onChange={setWhich} />
        <SwitchRow dataKey="rn:shorten" label={t("Shorten names too long for ShadowMount+")} hint={t("Cuts the title until the name fits")} on={shorten} onChange={setShorten} />
        <div class="many" style="margin-top:.6rem">
          {rows.map((r) => (
            <div class="item">
              <div class="ellip mono muted" style="font-size:.85rem">{r.e.name}</div>
              <div class={"ellip" + (r.why ? " muted" : "")}>{!applied ? "…" : r.why ? r.why : "→ " + r.name}</div>
              {!r.why && r.note ? <div class="note" style="margin-top:.15rem">{r.note}</div> : null}
            </div>
          ))}
        </div>
      </div>
    </Sheet>
  );
}

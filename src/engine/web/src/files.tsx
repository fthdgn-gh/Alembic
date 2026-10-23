// SPDX-License-Identifier: GPL-3.0-or-later
// Files: drives and places on the left, the folder's contents on the right.
import { useRef } from "preact/hooks";
import { signal } from "@preact/signals";
import type { Entry, Listing, Plan } from "./types";
import {
  cwd, listing, focusEntry, info, space, ftp, peerDrives, locations, tab, jobs, isConvertible, isActive, KIND_LABEL,
  rootOf, peerDriveOf, ftpOf, driveName, shownPath, hereName, loadInfo,
} from "./state";
import { api, base, bytes, date, dir, store, IS_PS5 } from "./util";
import { t, plural, placeLabel, tMsg } from "./i18n";
import { ICONS, EntryIco, useMeta } from "./ui";
import { ask, choose } from "./overlay";
import { focusEl, pushNav, setZoneLast, visible, zoneOf } from "./nav";
import { overlays } from "./overlay";
import { toast } from "./toasts";
import { openConvert } from "./convert";
import { openDest } from "./dest";
import { refreshJobs } from "./queue";

/* ---------- moving around ---------- */
export async function browse(path: string, push?: boolean, focusKey?: string | null) {
  let data: Listing;
  try { data = await api<Listing>("GET", "/api/browse?path=" + encodeURIComponent(path || "/")); }
  catch (e: any) { toast(tMsg(e.message), "bad"); return; }
  const from = cwd.value;
  if (push !== false && data.path !== from) pushNav({ view: "files", path: data.path, from });
  else if (history.state && !history.state.overlay) history.replaceState(Object.assign({}, history.state, { view: "files", path: data.path }), "");
  const same = data.path === from;
  // Back from a folder: land on that folder's row.
  const a = document.activeElement;
  const key = same ? (a && a.getAttribute("data-key")) : (focusKey || (from && dir(from) === data.path ? from : null));
  cwd.value = data.path;
  listing.value = data;
  setTimeout(() => placeFocus(data, key, same), 0); // once the list is drawn
}

// keepPlace (a refresh): leave the focus where it is when it's outside the list.
function placeFocus(data: Listing, focusKey: string | null, keepPlace: boolean) {
  if (listing.value !== data) return;
  const box = document.getElementById("list");
  if (!box) return;
  box.scrollTop = 0;
  if (!data.entries.length) { focusEntry.value = null; return; }
  const rows = Array.prototype.slice.call(box.querySelectorAll(".row")) as HTMLElement[];
  const toFocus = focusKey ? rows.filter((r) => r.getAttribute("data-key") === focusKey)[0] : null;
  const first = toFocus || rows[0];
  setZoneLast("list", first);
  const a = document.activeElement as HTMLElement | null, elsewhere = !!a && a !== document.body && a.isConnected && visible(a) && zoneOf(a) !== "list";
  if (overlays.value.length || tab.value !== "files" || (keepPlace && elsewhere)) focusEntry.value = data.entries[toFocus ? data.entries.findIndex((e) => e.path === focusKey) : 0];
  else focusEl(first);
}

export function refresh() { if (cwd.value) browse(cwd.value, false); loadInfo().catch(() => {}); }

function openEntry(e: Entry) {
  if (cwd.value === "/" || (e.kind === "folder" && !e.game)) return browse(e.path);
  if (isConvertible(e)) return openConvert(e);
  openActions(e);
}

/* ---------- the rail ---------- */
function RailItem(props: { k: string; on: boolean; title: string; icon: keyof typeof ICONS; name: string; onClick: () => void; children?: any }) {
  return (
    <button class={"ritem" + (props.on ? " on" : "")} data-key={props.k} title={props.title} onClick={props.onClick}>
      <div class="nm">{ICONS[props.icon]()}<span class="ellip">{props.name}</span></div>
      {props.children}
    </button>
  );
}
function Meter(props: { free: number; total: number }) {
  const used = props.total ? 1 - props.free / props.total : 0;
  return <div class={"meter" + (props.free < 20e9 ? " low" : "")}><div style={"width:" + (used * 100).toFixed(1) + "%"} /></div>;
}
const within = (root: string) => !!cwd.value && (cwd.value === root || cwd.value.indexOf(root + "/") === 0);

export function Rail() {
  const c = cwd.value;
  // Places: this engine's, then paired engines' (the ones added by hand and
  // the ones they suggest, like a PS5's homebrew folder; not their drives,
  // listed above), each with the engine's name under it.
  const peerRoots = new Set(peerDrives.value.map((d) => d.root));
  const short = (label: string) => placeLabel(label.replace(/ · homebrew.*$/, " · homebrew").replace(/^Internal storage$/, "Internal"));
  const places = locations.value.filter((l) => l.exists && !l.ftp && !l.peer && !space.value.some((s) => s.root === l.path));
  const theirs = locations.value.filter((l) => l.peer && l.exists && !peerRoots.has(l.path));
  const recent = (store.get<string[]>("recent") || []).filter((p) => rootOf(p) && !peerDriveOf(p) && !places.some((l) => l.path === p)).slice(0, 4);
  const engines: string[] = [];
  for (const l of theirs) if (engines.indexOf(l.peer!) < 0) engines.push(l.peer!);
  return (
    <nav class="rail scroll" id="rail" data-zone="rail" data-column>
      <h3>{t("Drives")}</h3>
      {space.value.map((s) => (
        <RailItem k={"r:" + s.root} on={within(s.root)} title={s.root} icon={/usb|ext/.test(s.root) ? "usb" : "drive"} name={driveName(s.root)} onClick={() => browse(s.root)}>
          <Meter free={s.free} total={s.total} />
          <div class="free">{t("{size} free", { size: bytes(s.free) })}</div>
        </RailItem>
      ))}
      {ftp.value.map((s) => (
        <RailItem k={"r:" + s.path} on={!!c && ftpOf(c) === s} title={s.user + "@" + s.host + ":" + s.port} icon="server" name={s.name} onClick={() => browse(s.path)}>
          <div class="free ellip">{"FTP · " + s.host + (s.port === 21 ? "" : ":" + s.port)}</div>
        </RailItem>
      ))}
      {/* Paired engines' drives (Settings > Other engines). */}
      {peerDrives.value.length ? <h3>{t("Other engines")}</h3> : null}
      {peerDrives.value.map((s) => (
        <RailItem k={"r:" + s.root} on={within(s.root)} title={s.engine} icon="server" name={driveName(s.root)} onClick={() => browse(s.root)}>
          {s.total ? <Meter free={s.free} total={s.total} /> : null}
          {s.total ? <div class="free">{t("{size} free", { size: bytes(s.free) })}</div> : null}
        </RailItem>
      ))}
      {places.length || theirs.length || recent.length ? <h3>{t("Places")}</h3> : null}
      {/* With other engines' places listed too, this engine's get its name as well. */}
      {theirs.length && (places.length || recent.length) ? <h4>{hereName()}</h4> : null}
      {places.map((l) => <RailItem k={"p:" + l.path} on={c === l.path} title={l.path} icon="place" name={short(l.label)} onClick={() => browse(l.path)} />)}
      {recent.map((p) => <RailItem k={"c:" + p} on={c === p} title={p} icon="clock" name={base(p)} onClick={() => browse(p)} />)}
      {/* Each paired engine's under its name. */}
      {engines.map((id) => {
        const list = theirs.filter((l) => l.peer === id);
        return [<h4>{list[0].engine}</h4>, ...list.map((l) => (
          <RailItem k={"p:" + l.path} on={c === l.path} title={shownPath(l.path)} icon="place" name={short(l.place || "")} onClick={() => browse(l.path)} />
        ))];
      })}
    </nav>
  );
}

/* ---------- breadcrumbs (also in the folder picker) ---------- */
export function Crumbs(props: { path: string | null; onPick: (p: string) => void; id?: string; style?: string; zone?: string }) {
  const path = props.path;
  const parts: string[] = [];
  let rootPath = "";
  if (path && path !== "/") {
    const r = rootOf(path), f = r ? null : ftpOf(path);
    rootPath = r ? r.root : f ? f.path : path;
    parts.push(rootPath);
    let acc = rootPath;
    for (const p of path.slice(rootPath.length).split("/").filter(Boolean)) { acc += "/" + p; parts.push(acc); }
  }
  return (
    <div class="crumbs" id={props.id} data-zone={props.zone} style={props.style}>
      <button data-key="c:/" onClick={() => props.onPick("/")}>{t("Drives")}</button>
      {parts.map((p) => [
        <span class="sep">›</span>,
        <button data-key={"c:" + p} onClick={() => props.onPick(p)} title={shownPath(p)}>{p === rootPath ? driveName(p) : base(p)}</button>,
      ])}
    </div>
  );
}

/* ---------- the list ---------- */
function Row(props: { e: Entry; top: boolean }) {
  const { e, top } = props;
  const ref = useRef<HTMLDivElement>(null);
  const m = useMeta(isConvertible(e) && !top ? e : null, ref);
  const fs = top ? ftpOf(e.path) : null;
  const r = top ? rootOf(e.path) : null;
  const plain = top ? (r ? t("{free} free of {total}", { free: bytes(r.free), total: bytes(r.total as number) }) : fs ? "FTP · " + fs.user + "@" + fs.host + ":" + fs.port : e.path)
    : [e.kind === "folder" ? "" : bytes(e.size), date(e.mtime)].filter(Boolean).join("  ·  ");
  const title = !top && m && m.title ? m.title : null;
  return (
    <div ref={ref} class="row" tabIndex={0} data-key={e.path} onClick={() => openEntry(e)} onFocus={() => { focusEntry.value = e; }}
      onContextMenu={top ? undefined : (ev) => { ev.preventDefault(); openActions(e); }}>
      {top ? <div class="ico folder">{fs ? ICONS.server() : /usb|ext/.test(e.path) ? ICONS.usb() : ICONS.drive()}</div> : <EntryIco entry={e} meta={m} />}
      <div style="min-width:0">
        <div class="t ellip" title={title || e.name}>{top ? driveName(e.path) : title || e.name}</div>
        <div class="s ellip">
          {m ? [
            <span class={"fmt " + (e.game ? "folder" : e.kind)}>{e.game ? t("FOLDER") : KIND_LABEL[e.kind] || ""}</span>,
            [m.title ? e.name : "", m.version ? "v" + m.version.replace(/^0+(?=\d)/, "") : "", m.title_id].filter(Boolean).join("  ·  "),
          ] : plain}
        </div>
      </div>
      <div class="sz">{!top && e.kind !== "folder" ? bytes(e.size) : ""}</div>
      {top ? <span /> : <button class="more" tabIndex={-1} data-nonav title={t("Options")} onClick={(ev) => { ev.stopPropagation(); openActions(e); }}>⋯</button>}
    </div>
  );
}

function List() {
  const data = listing.value;
  return (
    <div class="list scroll" id="list" data-zone="list" data-column>
      {!data ? null : !data.entries.length ? <div class="empty">{t("This folder is empty.")}</div>
        : data.entries.map((e) => <Row key={e.path} e={e} top={data.path === "/"} />)}
    </div>
  );
}

function Summary() {
  const data = listing.value;
  if (!data) return <div class="summary" id="summary" />;
  const files = data.entries.filter((e) => e.kind !== "folder"), folders = data.entries.length - files.length;
  const total = files.reduce((n, e) => n + (e.size || 0), 0);
  const r: any = data.path === "/" ? null : rootOf(data.path);
  const text = data.path === "/" ? t("Pick a drive") :
    [folders ? plural(folders, "{n} folder", "{n} folders") : "", files.length ? plural(files.length, "{n} file", "{n} files") + " · " + bytes(total) : "",
      r && (r.total || !r.engine) ? t("{size} free on {drive}", { size: bytes(r.free), drive: driveName(r.root) }) : ftpOf(data.path) ? t("On an FTP server") : ""].filter(Boolean).join("  ·  ") || t("Empty");
  return <div class="summary" id="summary">{text}</div>;
}

export function FilesView(props: { hidden: boolean }) {
  const pick = useRef<HTMLInputElement>(null);
  const atTop = cwd.value === "/";
  const shown = shares.value;
  return (
    <section class="view files" id="view-files" hidden={props.hidden}>
      <Rail />
      <div class="main">
        <div class="bar1">
          <div class="where">
            <Crumbs id="crumbs" zone="crumbs" path={cwd.value} onPick={(p) => browse(p)} />
            <Summary />
          </div>
          <div class="tools" data-zone="tools">
            <button class="btn sm" id="btn-share" hidden={IS_PS5} onClick={() => pick.current && pick.current.click()}>{t("From this computer…")}</button>
            <input ref={pick} type="file" id="share-pick" accept=".pkg,.exfat,.ffpfsc,.ffpkg,.ffpfs" hidden onChange={onPick} />
            {/* The folder you are in: Add to Places, Copy to..., and the rest. */}
            <button class="btn sm" id="btn-here" title={t("Options for this folder")} disabled={atTop}
              onClick={() => { if (cwd.value && cwd.value !== "/") openActions({ path: cwd.value, name: base(cwd.value), kind: "folder" }); }}>⋯</button>
            <button class="btn sm" id="btn-mkdir" disabled={atTop} onClick={newFolder}>{t("New folder")}</button>
            <button class="btn sm ghost" id="btn-refresh" title={t("Refresh")} onClick={refresh}>{t("Refresh")}</button>
          </div>
        </div>
        <div class="note sharebar" id="sharebar" hidden={!shown.length}>
          {shown.map((s) => t("Sharing {name} with the console ({sent} sent). Keep this page open until its job is done.", { name: s.file.name, sent: bytes(s.sent) })).join(" ")}
        </div>
        <List />
      </div>
    </section>
  );
}

/* ---------- a file's options ---------- */
export async function openActions(e: Entry) {
  if (cwd.value === "/") return;
  const conv = isConvertible(e);
  const items: { label: string; sub?: string; danger?: boolean; value: () => void }[] = [];
  if (e.kind === "folder" && e.path !== cwd.value) items.push({ label: t("Open"), value: () => browse(e.path) });
  if (conv) items.push({ label: t("Convert…"), sub: t("To FFPFSC, FFPKG, exFAT or a folder"), value: () => openConvert(e) });
  if (e.kind === "fpkg") items.push({ label: t("Install"), sub: t("Hand it to the console's installer"), value: () => installPkg(e) });
  if (conv && e.kind !== "fpkg") items.push({ label: t("Add the AMPR emulator…"), sub: t("For games that stop at start from an image or folder"), value: () => addAmpr(e) });
  if (conv && e.kind !== "fpkg") items.push(isHiddenName(e.name)
    ? { label: t("Show to ShadowMount+ again"), sub: t("Renames it back to {name}", { name: e.name.slice(1) }), value: () => toggleHidden(e.path) }
    : { label: t("Hide from ShadowMount+"), sub: t("Renames it to .{name}, so ShadowMount+ uses another copy of this game", { name: e.name }), value: () => toggleHidden(e.path) });
  // Places (this engine's or a paired one's): shared with every paired engine.
  if (e.kind === "folder" && cwd.value !== "/") {
    const isPlace = locations.value.some((l) => l.custom && l.path === e.path);
    items.push(isPlace ? { label: t("Remove from Places"), value: () => setPlace(e.path, false) }
      : { label: t("Add to Places"), sub: t("Shows it in Places and Save to, here and on paired engines"), value: () => setPlace(e.path, true) });
  }
  // A paired engine's usual destination (Settings > Other engines), for a copy without converting.
  const peerPlace = locations.value.find((l) => !!l.peer) || peerDrives.value.map((d) => ({ path: d.root, label: d.engine }))[0];
  if (peerPlace && !e.shared) items.push({ label: t("Send to another engine…"), sub: t("A copy as it is, without converting"), value: () => openDest(e, "copy", peerPlace.path) });
  items.push({ label: t("Copy to…"), value: () => openDest(e, "copy") }, { label: t("Move to…"), value: () => openDest(e, "move") },
    { label: t("Rename…"), value: () => renameEntry(e) }, { label: t("Delete…"), danger: true, value: () => deleteEntry(e) });
  const fn = await choose(e.name, items);
  if (fn) fn();
}

async function newFolder() {
  const name = await ask({ title: t("New folder"), input: t("New folder"), ok: t("Create") });
  if (!name || name === true) return;
  try { await api("POST", "/api/fs/mkdir", { path: cwd.value + "/" + name }); browse(cwd.value!, false, cwd.value + "/" + name); }
  catch (e: any) { toast(tMsg(e.message), "bad"); }
}

export async function setPlace(path: string, add: boolean) {
  try {
    locations.value = await api("POST", "/api/places", { path, remove: !add });
    toast(add ? t("Added to Places") : t("Removed from Places"), "ok");
  } catch (e: any) { toast(tMsg(e.message), "bad"); }
}

async function installPkg(e: Entry) {
  if (info.value && info.value.platform !== "ps5") { toast(t("Installing needs the engine running on a PS5"), "bad"); return; }
  const body = <div><div class="mono">{shownPath(e.path)}</div>
    <p class="muted">{t("The console installs it in the background; follow it in Downloads. Keep the file until the install has finished.")}</p></div>;
  if (!await ask({ title: t("Install this package?"), body, ok: t("Install") })) return;
  try { await api("POST", "/api/jobs", { kind: "install", input: e.path }); toast(t("Install queued"), "ok"); refreshJobs(); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}

// ShadowMount+ skips names that start with "." and keeps one source per
// game while that source exists: hiding the old one makes it switch.
export function isHiddenName(name: string) { return name.length > 1 && name[0] === "."; }
export async function toggleHidden(path: string, quiet?: boolean) {
  const n = base(path), name = isHiddenName(n) ? n.slice(1) : "." + n;
  try {
    await api("POST", "/api/fs/rename", { path, name });
    let scanned = true;
    try { await api("POST", "/api/smp/scan"); } catch (e) { scanned = false; }
    if (!quiet) toast(isHiddenName(name) ? (scanned ? t("Hidden from ShadowMount+; it is rescanning") : t("Hidden from ShadowMount+"))
      : (scanned ? t("Visible to ShadowMount+ again; it is rescanning") : t("Visible to ShadowMount+ again")), "ok");
    if (tab.value === "files" && cwd.value === dir(path)) browse(cwd.value, false, dir(path) + "/" + name);
    return true;
  } catch (err: any) { toast(tMsg(err.message), "bad"); return false; }
}

// Downloads the components a planned job needs, after asking. False when declined.
export async function getNeeds(plan: Plan, intro: string) {
  for (const dep of plan.needs || []) {
    const body = <div>
      <p style="margin-top:0">{t(intro, { name: t(dep.label) })}</p>
      <div class="facts"><div>{t("Why")}</div><div>{t(dep.purpose)}</div><div>{t("Source")}</div><div>{dep.source}</div><div>{t("Size")}</div><div>{bytes(dep.bytes)}</div></div>
      <p class="muted">{t("It's checked against a pinned SHA-256 before it's saved.")}</p>
    </div>;
    if (!await ask({ title: t("Download {name}?", { name: t(dep.label) }), body, ok: t("Download and continue") })) return false;
    toast(t("Downloading {name}…", { name: t(dep.label) }));
    await api("POST", "/api/deps/" + dep.id + "/install");
    toast(t("{name} downloaded", { name: t(dep.label) }), "ok");
    loadInfo().catch(() => {});
  }
  return true;
}

// ampr_emu into an existing game: a folder gets it in place, an image a
// ShadowMount+ backport folder (the image is not changed).
async function addAmpr(e: Entry) {
  const isFolder = e.kind === "folder";
  let plan: Plan;
  try { plan = await api<Plan>("POST", "/api/plan", { kind: "ampr_emu", input: e.path }); }
  catch (err: any) { toast(tMsg(err.message), "bad"); return; }
  if (plan.ships_emulator) { toast(t("This game already has the AMPR emulator"), "ok"); return; }
  const body = <div>
    <div class="mono">{shownPath(e.path)}</div>
    <p>{plan.imports_ampr === false ? t("This game does not seem to use the console's APR, so it may not need the emulator. It does no harm to add it.")
      : t("This game reads its files through the console's APR, which doesn't work when it runs from an image or folder. ampr_emu's libSceAmpr.sprx stands in for it.")}</p>
    <p class="muted">{isFolder ? t("It goes into the game folder's fakelib (and ampr_emu.index, if the game has none).")
      : t("The image is not changed: ShadowMount+ gets a backport folder for this title (the game's fakelib files plus the emulator), which it lays over the game when it starts.")}</p>
  </div>;
  if (!await ask({ title: t("Add the AMPR emulator?"), body, ok: t("Add") })) return;
  try {
    if (!await getNeeds(plan, "This needs {name}, which isn't downloaded yet.")) return;
    await api("POST", "/api/jobs", { kind: "ampr_emu", input: e.path });
    toast(t("Queued: {name}", { name: e.name }), "ok"); refreshJobs();
  } catch (err: any) { toast(tMsg(err.message), "bad"); }
}

async function renameEntry(e: Entry) {
  const name = await ask({ title: t("Rename"), input: e.name, ok: t("Rename") });
  if (!name || name === true || name === e.name) return;
  try { await api("POST", "/api/fs/rename", { path: e.path, name }); toast(t("Renamed to {name}", { name }), "ok"); browse(cwd.value!, false, dir(e.path) + "/" + name); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}

async function deleteEntry(e: Entry) {
  const body = <div>
    <div class="warnbox">
      <b>{e.kind === "folder" ? t("Delete this folder and everything in it?") : t("Delete this file?")}</b>
      <div class="mono" style="margin-top:.4rem">{shownPath(e.path)}</div>
      {e.size != null ? <div class="muted" style="margin-top:.2rem">{bytes(e.size)}</div> : null}
    </div>
    <p class="muted">{t("This can't be undone.")}</p>
  </div>;
  if (!await ask({ title: t("Delete"), body, ok: t("Delete"), danger: true })) return;
  try { await api("POST", "/api/fs/delete", { path: e.path }); toast(t("Deleted {name}", { name: e.name }), "ok"); browse(cwd.value!, false); loadInfo().catch(() => {}); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}

/* ---------- files shared from this computer (not on the console) ---------- */
// The console reads a picked file through this page: it asks for pieces
// (want, a long poll) and the page posts them. The page must stay open.
interface Share { path: string; file: File; sent: number }
const shares = signal<Share[]>([]);
export const isShared = (path: string) => typeof path === "string" && path.indexOf("/remote/") === 0;
export const isSharedHere = (path: string) => shares.value.some((s) => s.path === path);

function kindOfName(name: string): Entry["kind"] {
  const n = name.toLowerCase();
  for (const [ext, kind] of [[".pkg", "fpkg"], [".exfat", "exfat"], [".ffpfsc", "ffpfsc"], [".ffpkg", "ffpkg"], [".ffpfs", "ffpfs"]] as [string, Entry["kind"]][])
    if (n.slice(-ext.length) === ext) return kind;
  return "file";
}
async function relay(share: Share, id: string) {
  for (;;) {
    let want: { pieces?: { offset: number; length: number }[] };
    try { want = await api("POST", "/api/remote/" + id + "/want"); }
    catch (e) { await new Promise((r) => setTimeout(r, 2000)); continue; }
    for (const p of want.pieces || []) {
      const blob = share.file.slice(p.offset, p.offset + p.length);
      for (let attempt = 0; attempt < 3; attempt++) {
        try {
          const r = await fetch("/api/remote/" + id + "/data?offset=" + p.offset, { method: "POST", body: blob });
          if (r.ok) { share.sent += p.length; break; }
        } catch (e) { await new Promise((res) => setTimeout(res, 1000)); }
      }
    }
    shares.value = shares.value.slice(); // the bar shows what was sent
  }
}
async function shareFile(file: File) {
  const r = await api<{ path: string }>("POST", "/api/remote", { name: file.name, size: file.size, last_modified: file.lastModified });
  const known = shares.value.find((s) => s.path === r.path);
  if (!known) {
    const share = { path: r.path, file, sent: 0 };
    shares.value = shares.value.concat(share);
    const id = r.path.split("/")[2];
    for (let i = 0; i < 3; i++) relay(share, id); // three pieces in flight
  } else {
    known.file = file;
    shares.value = shares.value.slice();
  }
  return r.path;
}
async function onPick(ev: Event) {
  const input = ev.target as HTMLInputElement;
  const file = input.files && input.files[0];
  input.value = "";
  if (!file) return;
  try {
    const path = await shareFile(file);
    openConvert({ name: file.name, path, kind: kindOfName(file.name), size: file.size, shared: true });
  } catch (err: any) { toast(tMsg(err.message), "bad"); }
}
if (!IS_PS5) window.addEventListener("beforeunload", (ev) => {
  const busy = jobs.value.some((j) => isActive(j) && isShared(j.spec.input) && isSharedHere(j.spec.input));
  if (busy) { ev.preventDefault(); ev.returnValue = ""; }
});

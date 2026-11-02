// SPDX-License-Identifier: GPL-3.0-or-later
// The Convert dialog: the target format and options on the left; what the
// source is and where the output goes on the right. Several games (a
// selection in Files) share the choices and get a job each.
import { useEffect, useState } from "preact/hooks";
import type { Dep, Entry, Inspect, Plan, Job } from "../api/types";
import { api } from "../api/client";
import { cachedMeta } from "../api/meta";
import { PlatformPill } from "../components/PlatformPill";
import { info, peers, locations } from "../state/engine";
import { rootOf, serverOf, onServerText, driveName, shownPath, peerDriveOf, hereName } from "../state/paths";
import { refreshJobs } from "../state/jobs";
import { isHiddenName } from "../lib/kinds";
import { base, bytes, dir } from "../lib/format";
import { store } from "../lib/storage";
import { t, tMsg, plural, placeLabel } from "../lib/i18n";
import { ChoiceRow, SwitchRow } from "../components/Rows";
import { EntryIco } from "../components/Icons";
import { Sheet, openOverlay, closeOverlay } from "../components/Overlay";
import { choose, type ChoiceItem } from "../components/Dialogs";
import { toast } from "../components/Toasts";
import { goBack, showTab } from "../input/history";
import { stopSelecting } from "../state/selection";
import { openDest } from "./FolderPicker";
import { getNeeds } from "./getNeeds";

export const FORMATS = [
  { value: "ffpfsc", label: "FFPFSC", hint: "Compressed image, smallest" },
  { value: "ffpkg", label: "FFPKG", hint: "UFS2 image, ShadowMount+'s pick" },
  { value: "ffpfs", label: "FFPFS", hint: "PFS image, uncompressed (experimental in ShadowMount+)" },
  { value: "exfat", label: "exFAT", hint: "Plain image, fastest to load" },
  { value: "folder", label: "Folder", hint: "Loose files" },
  { value: "fpkg", label: "FPKG", hint: "Debug package to install" },
];

export function rememberFolder(path: string) {
  if (locations.value.some((l) => l.path === path)) return;
  store.set("recent", [path].concat((store.get<string[]>("recent") || []).filter((p) => p !== path)).slice(0, 5));
}
function destLabel(v: string) {
  if (!v) return t("Next to the source");
  const l = locations.value.filter((x) => x.path === v)[0];
  return l ? placeLabel(l.label) : shownPath(v);
}

// What the dialog's □ and Add to queue do, set by the open dialog.
interface Control { go: () => void }

// `install`: opened as "Install…" (FPKG, installed when done).
export function openConvert(entry: Entry | Entry[], install?: boolean) {
  const ctl: Control = { go: () => {} };
  const entries = Array.isArray(entry) ? entry : [entry];
  openOverlay<string>(() => <Convert entries={entries} ctl={ctl} install={!!install} />,
    { hints: [["dpad", t("Move, change")], ["x", t("Choose")], ["o", t("Cancel"), goBack], ["s", t("Add to queue"), () => ctl.go()]], onSquare: () => ctl.go() })
    .then((v) => { if (v === "deps") showTab("deps"); });
}

// The plan usually comes back at once; past these many seconds the footer says why the button waits.
const PLAN_SLOW = 2, PLAN_LATE = 60;
function waitText(waited: number, ready: boolean): string {
  if (ready || waited < PLAN_SLOW) return "";
  if (waited < PLAN_LATE) return t("Reading the source… ({n} s)", { n: waited });
  return t("The engine hasn't answered for {n} s. It may be busy with the disk; this keeps waiting, or cancel and try again.", { n: waited });
}

function initialDest(entry: Entry): string {
  let d = store.get<string>("dest") || "";
  // A shared file has no folder on the console: it goes to a place here.
  if (entry.shared && !d) d = (locations.value[0] || { path: "" }).path || "";
  if (d && !rootOf(d) && !serverOf(d)) d = "";
  return d;
}

// A game's own format: converting to it is no conversion (unless LZ4 changes).
const formatOf = (e: Entry) => (e.game ? "folder" : e.kind);

// Asks for each path, two at a time; resolves with the answers (or errors) by path.
function eachPath<T>(paths: string[], ask: (p: string) => Promise<T>): Promise<{ ok: Record<string, T>; err: Record<string, string> }> {
  const ok: Record<string, T> = {}, err: Record<string, string> = {};
  let next = 0;
  const worker = (): Promise<void> => {
    if (next >= paths.length) return Promise.resolve();
    const p = paths[next++];
    return ask(p).then((v) => { ok[p] = v; }, (e) => { err[p] = tMsg(e.message); }).then(worker);
  };
  return Promise.all([worker(), worker()]).then(() => ({ ok, err }));
}

function Convert(props: { entries: Entry[]; ctl: Control; install: boolean }) {
  const entries = props.entries, entry = entries[0], many = entries.length > 1;
  const m = cachedMeta(entry);
  const metas = entries.map(cachedMeta);
  const anyPs4 = metas.some((x) => !!x && x.platform === "ps4");
  const anyShared = entries.some((e) => !!e.shared);
  const rt = info.value ? info.value.lz4_runtime : null;
  const [to, setTo] = useState(props.install ? "fpkg" : entries.every((e) => e.kind === "ffpfsc") ? "ffpkg" : "ffpfsc");
  const [installWant, setInstall] = useState(props.install);
  const [keepPkg, setKeepPkg] = useState(false);
  const [dest, setDest] = useState(() => initialDest(entry));
  // What each source is (format, size, LZ4 packs), and the ones that could not be read.
  const [inspects, setInspects] = useState<{ ok: Record<string, Inspect>; err: Record<string, string> } | null>(null);
  const inspect = inspects ? inspects.ok[entry.path] || null : null;
  const problem = !inspects ? null : many ? (Object.keys(inspects.ok).length ? null : inspects.err[entry.path] || null) : inspects.err[entry.path] || null;
  const [level, setLevel] = useState(6);
  const [pkgComp, setPkgComp] = useState("stored");
  const [profile, setProfile] = useState("auto");
  const [lz4Level, setLz4Level] = useState(9);
  const [lz4Want, setLz4] = useState(false);
  const [unpackWant, setUnpack] = useState(false);
  const [zstdWant, setZstd] = useState(false);
  const [zstdLevel, setZstdLevel] = useState(5);
  const [zunpackWant, setZunpack] = useState(false);
  const [ampr, setAmpr] = useState(true);
  const [verify, setVerify] = useState(true);
  const [delWant, setDel] = useState(false);
  const [hideWant, setHide] = useState(!!store.get("hideSource"));
  const [name, setName] = useState("");
  const [over, setOver] = useState(false);
  // Which engine does the work (Settings > Other engines); the source and the output can be on any.
  // Only engines that answer right now: an offline one can't take the job.
  const paired = ((peers.value && peers.value.engines) || []).filter((e) => e.paired && e.reachable);
  const onChoices = [{ value: "", label: t("This engine") }].concat(paired.map((e) => ({ value: e.id, label: e.name })));
  const [whereWant, setWhere] = useState(() => store.get<string>("convertOn") || "");
  // An engine that went offline (or was the last choice) falls back to this one.
  const where = onChoices.some((c) => c.value === whereWant) ? whereWant : "";
  const whereHidden = !paired.length || anyShared;
  // The engine's plans and the choices they were made for: queueing uses them as they are.
  const [planned, setPlanned] = useState<{ key: string; plans: Record<string, Plan>; errs: Record<string, string> } | null>(null);
  const [waited, setWaited] = useState(0); // seconds without an answer to the latest plan request
  const [msg, setMsg] = useState<{ text: string; err: boolean }>({ text: "", err: false });
  const [busy, setBusy] = useState(false);

  // An LZ4-packed game can't be packed again, only unpacked: several games
  // can be packed when none is, unpacked when all are.
  const packed = (e: Entry) => !!(inspects && inspects.ok[e.path] && inspects.ok[e.path].lz4_packed);
  const packedAny = entries.some(packed), packedAll = entries.every(packed);
  // zstd packs (ampr-emu-ng's) the same way; a game has one kind or none.
  const zpacked = (e: Entry) => !!(inspects && inspects.ok[e.path] && inspects.ok[e.path].zstd_packed);
  const zpackedAny = entries.some(zpacked), zpackedAll = entries.every(zpacked);
  const packOffered = !packedAny && !zpackedAny;
  // zstd packs are for PS5 games in a folder or an image.
  // A packed game can switch to the other kind of packs in the same conversion.
  const zstdOffered = (packOffered || packedAll) && !anyPs4 && to !== "fpkg";
  const lz4Offered = packOffered || zpackedAll;
  const zstd = zstdWant && zstdOffered && !(unpackWant && packedAll);
  const lz4 = lz4Want && lz4Offered && !zstd && !(zunpackWant && zpackedAll);
  const unpack = unpackWant && packedAll && !zstd, zunpack = zunpackWant && zpackedAll && !lz4;
  // A PS5 package can go straight to the console's installer (then it is deleted).
  const installable = to === "fpkg" && !!info.value && info.value.platform === "ps5" && !anyPs4;
  const del = !anyShared && delWant && !(installable && installWant);
  // The source's own format is no conversion, except when packing or unpacking
  // LZ4 (an exFAT game into an LZ4-packed exFAT, say). Several games: a game
  // already in the target format is left out.
  const formats = entries.map(formatOf), oneFormat = formats.every((f) => f === formats[0]) ? formats[0] : null;
  const offered = FORMATS.filter((f) => f.value !== oneFormat || lz4 || unpack || zstd || zunpack);
  const target = offered.some((f) => f.value === to) ? to : offered[0].value;
  const work = entries.filter((e) => formatOf(e) !== target || lz4 || unpack || zstd || zunpack);
  const skipped = entries.filter((e) => work.indexOf(e) < 0);
  const hideHiddenFor = (e: Entry) => e.kind === "fpkg" || isHiddenName(e.name) || del || target === "fpkg" || !!e.shared;
  const hideHidden = entries.every(hideHiddenFor);
  const amprHidden = target === "fpkg" || lz4 || zstd; // packing adds its runtime anyway
  const specFor = (e: Entry) => ({
    input: e.path, to: target, output_dir: dest, output_name: many ? "" : name.trim(), level, pkg_compression: anyPs4 ? "stored" : pkgComp,
    install_after: installable && target === "fpkg" && installWant, keep_package: keepPkg, lz4, lz4_unpack: unpack,
    lz4_profile: profile, lz4_level: lz4Level, delete_source: del,
    ...(zstd || zunpack ? { zstd, zstd_level: zstdLevel, zstd_unpack: zunpack } : {}), verify, overwrite: over,
    ampr_emu: ampr ? "auto" : "off", hide_source: !hideHiddenFor(e) && hideWant && !del,
  });
  const specs = work.map(specFor);
  const specKey = JSON.stringify(specs);
  // Only plans for the current choices count; older ones still name the outputs while the new ones load.
  const plans = planned ? planned.plans : {}, planErrs = planned ? planned.errs : {};
  const plan = plans[entry.path] || null;
  const planErr = !planned || planned.key !== specKey ? null
    : many ? (Object.keys(planned.plans).length ? null : t("None of these can be converted this way.")) : planErrs[entry.path] || null;
  const planReady = !!planned && planned.key === specKey && !planErr && work.length > 0;

  useEffect(() => {
    eachPath(entries.map((e) => e.path), (p) => api<Inspect>("GET", "/api/inspect?path=" + encodeURIComponent(p))).then(setInspects);
  }, []);
  // The engine's plan for these choices: the output's name, what it needs, warnings.
  // A slow engine (busy disk, a big package read for the first time) must not
  // look like a dead dialog: count the wait and say so.
  useEffect(() => {
    let live = true, ticker = 0;
    const key = specKey, started = Date.now();
    setWaited(0);
    const timer = setTimeout(() => {
      ticker = setInterval(() => { if (live) setWaited(Math.floor((Date.now() - started) / 1000)); }, 1000) as unknown as number;
      const bySource: Record<string, unknown> = {};
      for (const sp of specs) bySource[sp.input] = sp;
      eachPath(Object.keys(bySource), (p) => api<Plan>("POST", "/api/plan", bySource[p])).then((r) => {
        if (!live) return; // the choices changed meanwhile: a newer request is on its way
        setPlanned({ key, plans: r.ok, errs: r.err }); setMsg({ text: "", err: false });
      }).then(() => { clearInterval(ticker); if (live) setWaited(0); });
    }, 150);
    return () => { live = false; clearTimeout(timer); clearInterval(ticker); };
  }, [specKey]);

  async function go() {
    if (busy || !planReady || problem) return;
    // The games the engine has a plan for (the others say why in the list).
    const ready = specs.filter((sp) => !!planned!.plans[sp.input]);
    setBusy(true);
    try {
      const on = whereHidden ? "" : where, engine = paired.find((e) => e.id === on);
      // Components are the converting engine's own: only this engine's are fetched from here.
      if (!on) {
        const needs: Dep[] = [];
        for (const sp of ready) for (const d of planned!.plans[sp.input].needs || []) if (!needs.some((x) => x.id === d.id)) needs.push(d);
        if (!await getNeeds({ output: "", needs }, "This conversion needs {name}, which isn't downloaded yet.")) {
          setMsg({ text: t("Not queued: a component is needed."), err: false }); return;
        }
      }
      const jobs: Job[] = [];
      const failed: string[] = [];
      for (const sp of ready) {
        try { jobs.push(on ? await api<Job>("POST", "/api/peers/" + encodeURIComponent(on) + "/jobs", sp) : await api<Job>("POST", "/api/jobs", sp)); }
        catch (e: any) { if (!many) throw e; failed.push(base(sp.input) + ": " + tMsg(e.message)); }
      }
      if (!jobs.length) { setMsg({ text: failed[0] || "", err: true }); return; }
      store.set("dest", dest);
      store.set("convertOn", on);
      closeOverlay(true);
      const what = jobs.length === 1 ? base(jobs[0].spec.output) : plural(jobs.length, "{n} job", "{n} jobs");
      toast(engine ? t("Queued on {engine}: {name}", { engine: engine.name, name: what }) : t("Queued: {name}", { name: what }), "ok");
      if (failed.length) toast(failed.length > 1 ? t("{first} (and {n} more)", { first: failed[0], n: failed.length - 1 }) : failed[0], "bad");
      if (many) stopSelecting();
      refreshJobs(true);
    } catch (e: any) { setMsg({ text: tMsg(e.message), err: true }); }
    finally { setBusy(false); }
  }
  props.ctl.go = go;

  async function pickDest() {
    const items: ChoiceItem<string>[] = anyShared ? [] : [{ label: many ? t("Next to each source") : t("Next to the source"), value: "", selected: dest === "" }];
    // This engine's places and recent folders, then each paired engine's under its name.
    const here = hereName();
    const groups: { who: string; list: ChoiceItem<string>[] }[] = [{ who: here, list: [] }];
    const groupOf = (who: string) => groups.find((g) => g.who === who) || (groups.push({ who, list: [] }), groups[groups.length - 1]);
    const onEngine = (p: string) => { const s = shownPath(p); const i = s.indexOf(": "); return p.indexOf("/peer/") === 0 && i > 0 ? s.slice(i + 2) : s; };
    for (const l of locations.value) groupOf(l.peer ? l.engine! : here).list.push({ label: placeLabel((l.peer ? l.place! : l.label).replace(/ · homebrew.*$/, " · homebrew")), sub: onEngine(l.path), value: l.path, selected: dest === l.path });
    for (const p of (store.get<string[]>("recent") || []).filter((p) => rootOf(p) && !locations.value.some((l) => l.path === p))) {
      const pd = peerDriveOf(p);
      groupOf(pd ? pd.engine : here).list.push({ label: base(p), sub: onEngine(p), value: p, selected: dest === p });
    }
    const shown = groups.filter((g) => g.list.length);
    for (const g of shown) {
      if (shown.length > 1) items.push({ heading: g.who });
      items.push(...g.list);
    }
    items.push({ label: t("Choose a folder…"), value: "__pick" });
    let v = await choose(t("Save to"), items);
    if (v === "__pick") v = await openDest(null, "pick", dest || dir(entry.path));
    if (v != null) { setDest(v); if (v) rememberFolder(v); }
  }

  const profiles = [{ value: "auto", label: t("Automatic") }].concat((info.value ? info.value.lz4_profiles : []).map((p) => ({ value: p.id, label: p.label }))).concat([{ value: "scan", label: t("Scan (untested)") }]);
  const rtNote = rt && !rt.present ? <div class="note">{t("No ampr_emu runtime yet: only games that ship it can be packed.") + " "}
      <a href="#" onClick={(ev) => { ev.preventDefault(); closeOverlay("deps"); }}>{t("Get it in Components")}</a></div>
    : rt && !rt.pinned ? <div class="note">{t("The runtime file is not the pinned ampr_emu 0.4.2.1 build.")}</div> : null;
  // LZ4 packs inside an FFPFSC work, but the console then undoes zlib and LZ4.
  const lz4NoteShown = target === "ffpfsc" && (lz4 || (packedAny && !unpack && !zstd));
  const firstPlan = plan || plans[Object.keys(plans)[0]] || null;
  const r: any = firstPlan ? rootOf(firstPlan.output) : null;
  const need = work.reduce((n, e) => n + (inspects && inspects.ok[e.path] ? inspects.ok[e.path].bytes : 0), 0);
  const outSpace = !firstPlan ? "" : r ? t("{size} free on {drive}", { size: bytes(r.free), drive: driveName(r.root) })
      + (need ? " · " + (many ? t("the games are {size} unpacked", { size: bytes(need) }) : t("the game is {size} unpacked", { size: bytes(need) })) : "")
    : serverOf(firstPlan.output) ? onServerText(serverOf(firstPlan.output)!) : "";
  const warnings = plan && plan.warnings ? plan.warnings : [];
  const amprShown = many ? !amprHidden && Object.keys(plans).some((k) => !!plans[k].ampr_needed) : !!plan && !amprHidden && !!plan.ampr_needed;
  const needs: Dep[] = [];
  for (const k of Object.keys(plans)) for (const d of plans[k].needs || []) if (!needs.some((x) => x.id === d.id)) needs.push(d);

  return (
    <Sheet title={t("Convert")} cls="wide" foot={<>
      <span class={"msg" + (planErr || msg.err || waited >= PLAN_LATE ? " err" : "")}>{planErr || msg.text || waitText(waited, planReady)}</span>
      <button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={busy || !planReady || !!problem} onClick={go}>{t("Add to queue")}</button>
    </>}>
      <div class="cv">
        <div>
          <div class="lbl">{t("Convert to")}</div>
          <div class="opts">
            {offered.map((f) => (
              <button key={f.value} class={"opt" + (target === f.value ? " sel" : "")} data-key={f.value} data-autofocus={target === f.value ? "" : undefined} onClick={() => setTo(f.value)}>
                <b>{f.label}</b><span>{t(f.hint)}</span>
              </button>
            ))}
          </div>
          <div class="lbl">{t("Options")}</div>
          <ChoiceRow label={t("Compression")} hint={t("Level 9 matches PS5PKGTool byte for byte")} hidden={target !== "ffpfsc"} value={level} onChange={setLevel}
            options={[{ value: 1, label: t("1 · fastest") }, { value: 6, label: t("6 · balanced") }, { value: 9, label: t("9 · smallest") }]} />
          {/* PS4 packages are always written stored. */}
          <ChoiceRow label={t("Compression")} hint={t("Executables stay uncompressed; matches ProsperoPkgTool byte for byte")}
            hidden={target !== "fpkg" || anyPs4} value={pkgComp} onChange={setPkgComp}
            options={[{ value: "stored", label: t("Stored · fastest") }, { value: "kraken", label: t("Kraken · smaller") }]} />
          <SwitchRow label={t("Install when done")} hint={t("The package goes to the console's installer and is deleted once the installer has read it; it needs room until then")}
            on={installWant} onChange={setInstall} hidden={!installable || target !== "fpkg"} dataKey="cv:install" />
          <div class="indent" hidden={!installable || target !== "fpkg" || !installWant}>
            <SwitchRow label={t("Keep the package")} hint={t("Leaves the .pkg after installing, to install again or elsewhere")} on={keepPkg} onChange={setKeepPkg} dataKey="cv:keep" />
          </div>
          <SwitchRow label={zpackedAll ? t("Switch to LZ4 packs") : t("Pack into LZ4 asset packs")}
            hint={zpackedAll ? t("Replaces the zstd packs; both AMPR emulators read LZ4 packs") : t("For ampr_emu games")}
            on={lz4} onChange={(on) => { setLz4(on); if (on && zpackedAll && oneFormat) setTo(oneFormat); }} hidden={!lz4Offered || zstd || zunpack} dataKey="cv:lz4" />
          <div class="indent" hidden={!lz4}>
            <ChoiceRow label={t("Profile")} hint={t("Automatic uses a console-tested profile when one matches")} options={profiles} value={profile} onChange={setProfile} />
            <ChoiceRow label={t("LZ4 level")} options={[{ value: 1, label: t("1 · fast") }, { value: 9, label: t("9 · default") }, { value: 12, label: t("12 · smallest") }]} value={lz4Level} onChange={setLz4Level} />
            {rtNote}
          </div>
          <SwitchRow label={packedAll ? t("Switch to zstd packs") : t("Pack into zstd packs")}
            hint={packedAll ? t("Replaces the LZ4 packs with smaller zstd ones; read only by ampr-emu-ng, which goes into the game")
              : t("Smaller than LZ4 and faster to make; read only by ampr-emu-ng, which goes into the game")}
            on={zstd} onChange={(on) => { setZstd(on); if (on && packedAll && oneFormat) setTo(oneFormat); }} hidden={!zstdOffered || lz4 || unpack} dataKey="cv:zstd" />
          <div class="indent" hidden={!zstd}>
            <ChoiceRow label={t("zstd level")} options={[{ value: 1, label: t("1 · fastest") }, { value: 5, label: t("5 · default") }, { value: 19, label: t("19 · smallest") }]} value={zstdLevel} onChange={setZstdLevel} />
            <div class="note">{t("Turn off ShadowMount+'s “update emulators”: it would swap ampr-emu-ng for another emulator that can't read these packs.")}</div>
          </div>
          {/* Shown instead of the pack switch for a game that is already packed. */}
          <SwitchRow label={t("Unpack LZ4 asset packs")} hint={t("Turns the packs back into ordinary files; ampr_emu's runtime stays in fakelib")} on={unpack} onChange={setUnpack} hidden={!packedAll || zstd} />
          <SwitchRow label={t("Unpack zstd packs")} hint={t("Turns the packs back into ordinary files; ampr-emu-ng stays in fakelib")} on={zunpack} onChange={setZunpack} hidden={!zpackedAll || lz4} />
          <div class="note" hidden={!lz4NoteShown}>
            {(packedAny && !many ? t("This game is already LZ4-packed.") + " " : "") + t("FFPFSC loads slower than exFAT here: the console undoes zlib, then LZ4, and zlib saves little on LZ4 data.")}
          </div>
          {/* ampr_emu for games that use the console's APR (not packages: installed, they have the real one). */}
          <SwitchRow label={t("Add the AMPR emulator when the game needs it")} hint={t("Games that use the console's APR stop at start from an image or folder without it")} on={ampr} onChange={setAmpr} hidden={amprHidden} />
          <SwitchRow label={many ? t("Delete the sources when done") : t("Delete the source when done")}
            hint={many ? t("Each one after its conversion succeeds") : entry.kind === "fpkg" || entry.kind === "ffpkg" ? t("Frees {size} after a successful conversion", { size: bytes(entry.size) }) : t("Frees its space after a successful conversion")}
            on={del} onChange={setDel} hidden={anyShared || (installable && installWant && target === "fpkg")} />
          <div class="indent" hidden={!del}>
            <SwitchRow label={t("Check the output first")} hint={t("Compares every file before deleting; recommended")} on={verify} onChange={setVerify} />
          </div>
          {/* ShadowMount+ never scans packages, so only images and folders need hiding. */}
          <SwitchRow label={t("Hide the source from ShadowMount+")} hint={t("Renames it to .name when done, so ShadowMount+ switches to the new image")} on={hideWant}
            onChange={(on) => { setHide(on); store.set("hideSource", on); }} hidden={hideHidden} />
          <div class="lbl">{t("Output")}</div>
          <button class="frow" onClick={pickDest}>
            <div class="fl"><b>{t("Save to")}</b></div>
            <div class="val"><span class="ellip" style="max-width:16rem">{destLabel(dest)}</span><span class="arr">›</span></div>
          </button>
          <div style="margin-bottom:.5rem" hidden={many}>
            <input class="input mono" spellcheck={false} autocapitalize="off" placeholder={plan ? base(plan.output) : ""} value={name}
              onInput={(e) => setName((e.target as HTMLInputElement).value)}
              onKeyDown={(e) => { if (e.key === "Enter") { e.preventDefault(); (e.target as HTMLInputElement).blur(); } }} />
          </div>
          <SwitchRow label={t("Replace the output if it exists")} on={over} onChange={setOver} />
          <ChoiceRow label={t("Convert on")} hint={t("The engine that does the work; the source and the output can be on any paired engine")} options={onChoices} value={where}
            onChange={setWhere} hidden={whereHidden} />
        </div>
        {many ? <ManySide entries={entries} work={work} skipped={skipped} target={target} plans={plans} errs={planErrs}
          inspects={inspects} outSpace={outSpace} amprShown={amprShown} ampr={ampr} needs={needs} /> : <div class="side">
          <div class="head">
            <EntryIco entry={entry} meta={m} />
            <div style="min-width:0"><b>{m && m.title ? m.title : entry.name}</b><div class="muted ellip"><PlatformPill meta={m} /><span class="mono">{m && m.title ? entry.name : ""}</span></div></div>
          </div>
          <div class="facts">
            {problem ? [<div>{t("Problem")}</div>, <div class="err">{problem}</div>]
              : !inspect ? [<div>{t("Reading")}</div>, <div class="muted">…</div>]
              : [
                <div>{t("Format")}</div>, <div>{inspect.format + (inspect.lz4_packed ? " · " + t("LZ4-packed") : inspect.zstd_packed ? " · " + t("zstd-packed") : "")}</div>,
                <div>{t("Title ID")}</div>, <div>{inspect.title_id || "—"}</div>,
                <div>{t("Contents")}</div>, <div>{plural(inspect.files, "{n} file", "{n} files") + " · " + bytes(inspect.bytes)}</div>,
                <div>{t("LZ4 profile")}</div>, <div>{inspect.lz4_profile === "scan" ? t("no tested profile") : inspect.lz4_profile}</div>,
              ]}
          </div>
          <div class="out">
            <div class="lbl" style="margin-top:.4rem">{t("Output")}</div>
            <div class="mono">{plan ? shownPath(plan.output) : ""}</div>
            <div class="muted">{outSpace}</div>
          </div>
          <div class="note" hidden={!amprShown}>{!amprShown ? "" : ampr ? t("This game uses the console's APR and ships no emulator: ampr_emu goes into its fakelib.")
            : t("This game uses the console's APR and ships no emulator: without it, it will likely stop at start.")}</div>
          <div class="note" hidden={!needs.length}>{needs.length ? t("Needs {list}. You'll be asked to download it when you add this to the queue.", { list: needs.map((d) => t(d.label) + " (" + bytes(d.bytes) + ")").join(", ") }) : ""}</div>
          <div class="note" hidden={!warnings.length}>{warnings.map(tMsg).join(" ")}</div>
        </div>}
      </div>
    </Sheet>
  );
}

// Several games: what each one becomes, or why it is left out.
function ManySide(props: { entries: Entry[]; work: Entry[]; skipped: Entry[]; target: string; plans: Record<string, Plan>; errs: Record<string, string>;
  inspects: { ok: Record<string, Inspect>; err: Record<string, string> } | null; outSpace: string; amprShown: boolean; ampr: boolean; needs: Dep[] }) {
  const { work, skipped, plans, errs, inspects } = props;
  const label = (FORMATS.find((f) => f.value === props.target) || { label: props.target }).label;
  return (
    <div class="side">
      <div class="lbl">{plural(props.entries.length, "{n} game", "{n} games")}</div>
      <div class="many">
        {work.map((e) => {
          const m = cachedMeta(e), p = plans[e.path], err = errs[e.path] || (inspects && inspects.err[e.path]);
          return (
            <div class="item">
              <div class="ellip"><b>{m && m.title ? m.title : e.name}</b></div>
              <div class={"ellip " + (err ? "err" : "mono muted")} style="font-size:.85rem">{err ? err : p ? "→ " + base(p.output) : "…"}</div>
              {p && p.warnings && p.warnings.length ? <div class="note" style="margin-top:.15rem">{p.warnings.map(tMsg).join(" ")}</div> : null}
            </div>
          );
        })}
        {skipped.map((e) => {
          const m = cachedMeta(e);
          return (
            <div class="item">
              <div class="ellip muted"><b>{m && m.title ? m.title : e.name}</b></div>
              <div class="muted" style="font-size:.85rem">{t("Already {format}: left out", { format: label })}</div>
            </div>
          );
        })}
      </div>
      <div class="muted" style="font-size:.88rem;margin-top:.4rem">{props.outSpace}</div>
      <div class="note" hidden={!props.amprShown}>{props.ampr ? t("Games that use the console's APR and ship no emulator get ampr_emu in their fakelib.")
        : t("Some of these games use the console's APR and ship no emulator: without it, they will likely stop at start.")}</div>
      <div class="note" hidden={!props.needs.length}>{props.needs.length ? t("Needs {list}. You'll be asked to download it when you add this to the queue.", { list: props.needs.map((d) => t(d.label) + " (" + bytes(d.bytes) + ")").join(", ") }) : ""}</div>
    </div>
  );
}

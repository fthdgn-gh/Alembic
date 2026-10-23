// SPDX-License-Identifier: GPL-3.0-or-later
// The Convert dialog: the target format and options on the left; what the
// source is and where the output goes on the right.
import { useEffect, useState } from "preact/hooks";
import type { Entry, Inspect, Plan, Job } from "./types";
import { info, peers, locations, rootOf, ftpOf, driveName, shownPath, peerDriveOf, hereName } from "./state";
import { api, base, bytes, dir, store } from "./util";
import { t, tMsg, plural, placeLabel } from "./i18n";
import { ChoiceRow, SwitchRow, EntryIco, cachedMeta } from "./ui";
import { Sheet, openOverlay, closeOverlay, choose, type ChoiceItem } from "./overlay";
import { goBack } from "./nav";
import { getNeeds, isHiddenName } from "./files";
import { openDest } from "./dest";
import { toast } from "./toasts";
import { refreshJobs } from "./queue";
import { showTab } from "./app";

export const FORMATS = [
  { value: "ffpfsc", label: "FFPFSC", hint: "Compressed image, smallest" },
  { value: "ffpkg", label: "FFPKG", hint: "UFS2 image, ShadowMount+'s pick" },
  { value: "ffpfs", label: "FFPFS", hint: "PFS image, uncompressed (experimental in ShadowMount+)" },
  { value: "exfat", label: "exFAT", hint: "Plain image, fastest to load" },
  { value: "folder", label: "Folder", hint: "Loose files" },
  { value: "fpkg", label: "FPKG", hint: "Debug package to install, uncompressed" },
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

export function openConvert(entry: Entry) {
  const ctl: Control = { go: () => {} };
  openOverlay<string>(() => <Convert entry={entry} ctl={ctl} />,
    { hints: [["dpad", t("Move, change")], ["x", t("Choose")], ["o", t("Cancel"), goBack], ["s", t("Add to queue"), () => ctl.go()]], onSquare: () => ctl.go() })
    .then((v) => { if (v === "deps") showTab("deps"); });
}

function initialDest(entry: Entry): string {
  let d = store.get<string>("dest") || "";
  // A shared file has no folder on the console: it goes to a place here.
  if (entry.shared && !d) d = (locations.value[0] || { path: "" }).path || "";
  if (d && !rootOf(d) && !ftpOf(d)) d = "";
  return d;
}

function Convert(props: { entry: Entry; ctl: Control }) {
  const entry = props.entry;
  const m = cachedMeta(entry);
  const rt = info.value ? info.value.lz4_runtime : null;
  const [to, setTo] = useState(entry.kind === "ffpfsc" ? "ffpkg" : "ffpfsc");
  const [dest, setDest] = useState(() => initialDest(entry));
  const [inspect, setInspect] = useState<Inspect | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [level, setLevel] = useState(6);
  const [profile, setProfile] = useState("auto");
  const [lz4Level, setLz4Level] = useState(9);
  const [lz4Want, setLz4] = useState(false);
  const [unpackWant, setUnpack] = useState(false);
  const [ampr, setAmpr] = useState(true);
  const [verify, setVerify] = useState(true);
  const [delWant, setDel] = useState(false);
  const [hideWant, setHide] = useState(!!store.get("hideSource"));
  const [name, setName] = useState("");
  const [over, setOver] = useState(false);
  // Which engine does the work (Settings > Other engines); the source and the output can be on any.
  const paired = ((peers.value && peers.value.engines) || []).filter((e) => e.paired);
  const onChoices = [{ value: "", label: t("This engine") }].concat(paired.map((e) => ({ value: e.id, label: e.name })));
  const [where, setWhere] = useState(() => { const s = store.get<string>("convertOn") || ""; return onChoices.some((c) => c.value === s) ? s : ""; });
  const whereHidden = !paired.length || !!entry.shared;
  const [plan, setPlan] = useState<Plan | null>(null);
  const [planErr, setPlanErr] = useState<string | null>(null); // the engine refuses these choices
  const [msg, setMsg] = useState<{ text: string; err: boolean }>({ text: "", err: false });
  const [busy, setBusy] = useState(false);

  // An LZ4-packed game can't be packed again, only unpacked.
  const packedSrc = !!(inspect && inspect.lz4_packed);
  const lz4 = lz4Want && !packedSrc, unpack = unpackWant && packedSrc;
  const del = !entry.shared && delWant;
  // The source's own format is no conversion, except when packing or unpacking
  // LZ4 (an exFAT game into an LZ4-packed exFAT, say).
  const sameFormat = entry.game ? "folder" : entry.kind;
  const offered = FORMATS.filter((f) => f.value !== sameFormat || lz4 || unpack);
  const target = offered.some((f) => f.value === to) ? to : offered[0].value;
  const hideHidden = entry.kind === "fpkg" || isHiddenName(entry.name) || del || target === "fpkg" || !!entry.shared;
  const amprHidden = target === "fpkg" || lz4; // packing adds its runtime anyway
  const spec = {
    input: entry.path, to: target, output_dir: dest, output_name: name.trim(), level, lz4, lz4_unpack: unpack,
    lz4_profile: profile, lz4_level: lz4Level, delete_source: del, verify, overwrite: over,
    ampr_emu: ampr ? "auto" : "off", hide_source: !hideHidden && hideWant && !del,
  };
  const specKey = JSON.stringify(spec);

  useEffect(() => {
    api<Inspect>("GET", "/api/inspect?path=" + encodeURIComponent(entry.path)).then(setInspect, (e) => setProblem(tMsg(e.message)));
  }, []);
  // The engine's plan for these choices: the output's name, what it needs, warnings.
  useEffect(() => {
    const timer = setTimeout(() => {
      api<Plan>("POST", "/api/plan", spec).then((p) => { setPlan(p); setPlanErr(null); setMsg({ text: "", err: false }); },
        (e) => setPlanErr(tMsg(e.message)));
    }, 150);
    return () => clearTimeout(timer);
  }, [specKey]);

  async function go() {
    if (busy || planErr || problem) return;
    setBusy(true);
    try {
      const on = whereHidden ? "" : where, engine = paired.find((e) => e.id === on);
      // Components are the converting engine's own: only this engine's are fetched from here.
      if (!on) {
        const p = await api<Plan>("POST", "/api/plan", spec);
        if (!await getNeeds(p, "This conversion needs {name}, which isn't downloaded yet.")) {
          setMsg({ text: t("Not queued: a component is needed."), err: false }); return;
        }
      }
      const job = on ? await api<Job>("POST", "/api/peers/" + encodeURIComponent(on) + "/jobs", spec) : await api<Job>("POST", "/api/jobs", spec);
      store.set("dest", spec.output_dir);
      store.set("convertOn", on);
      closeOverlay(true);
      toast(engine ? t("Queued on {engine}: {name}", { engine: engine.name, name: base(job.spec.output) }) : t("Queued: {name}", { name: base(job.spec.output) }), "ok");
      refreshJobs(true);
    } catch (e: any) { setMsg({ text: tMsg(e.message), err: true }); }
    finally { setBusy(false); }
  }
  props.ctl.go = go;

  async function pickDest() {
    const items: ChoiceItem<string>[] = entry.shared ? [] : [{ label: t("Next to the source"), value: "", selected: dest === "" }];
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
  const lz4NoteShown = target === "ffpfsc" && (lz4 || (packedSrc && !unpack));
  const r: any = plan ? rootOf(plan.output) : null, need = inspect ? inspect.bytes : 0;
  const outSpace = !plan ? "" : r ? t("{size} free on {drive}", { size: bytes(r.free), drive: driveName(r.root) }) + (need ? " · " + t("the game is {size} unpacked", { size: bytes(need) }) : "")
    : ftpOf(plan.output) ? t("On an FTP server") : "";
  const warnings = plan && plan.warnings ? plan.warnings : [];
  const amprShown = !!plan && !amprHidden && !!plan.ampr_needed;
  const needs = plan && plan.needs ? plan.needs : [];

  return (
    <Sheet title={t("Convert")} cls="wide" foot={<>
      <span class={"msg" + (planErr || msg.err ? " err" : "")}>{planErr || msg.text}</span>
      <button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={busy || !!planErr || !!problem} onClick={go}>{t("Add to queue")}</button>
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
          <SwitchRow label={t("Pack into LZ4 asset packs")} hint={t("For ampr_emu games")} on={lz4} onChange={setLz4} hidden={packedSrc} />
          <div class="indent" hidden={!lz4}>
            <ChoiceRow label={t("Profile")} hint={t("Automatic uses a console-tested profile when one matches")} options={profiles} value={profile} onChange={setProfile} />
            <ChoiceRow label={t("LZ4 level")} options={[{ value: 1, label: t("1 · fast") }, { value: 9, label: t("9 · default") }, { value: 12, label: t("12 · smallest") }]} value={lz4Level} onChange={setLz4Level} />
            {rtNote}
          </div>
          {/* Shown instead of the pack switch for a game that is already packed. */}
          <SwitchRow label={t("Unpack LZ4 asset packs")} hint={t("Turns the packs back into ordinary files; ampr_emu's runtime stays in fakelib")} on={unpack} onChange={setUnpack} hidden={!packedSrc} />
          <div class="note" hidden={!lz4NoteShown}>
            {(packedSrc ? t("This game is already LZ4-packed.") + " " : "") + t("FFPFSC loads slower than exFAT here: the console undoes zlib, then LZ4, and zlib saves little on LZ4 data.")}
          </div>
          {/* ampr_emu for games that use the console's APR (not packages: installed, they have the real one). */}
          <SwitchRow label={t("Add the AMPR emulator when the game needs it")} hint={t("Games that use the console's APR stop at start from an image or folder without it")} on={ampr} onChange={setAmpr} hidden={amprHidden} />
          <SwitchRow label={t("Delete the source when done")} hint={entry.kind === "fpkg" || entry.kind === "ffpkg" ? t("Frees {size} after a successful conversion", { size: bytes(entry.size) }) : t("Frees its space after a successful conversion")}
            on={del} onChange={setDel} hidden={!!entry.shared} />
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
          <div style="margin-bottom:.5rem">
            <input class="input mono" spellcheck={false} autocapitalize="off" placeholder={plan ? base(plan.output) : ""} value={name}
              onInput={(e) => setName((e.target as HTMLInputElement).value)}
              onKeyDown={(e) => { if (e.key === "Enter") { e.preventDefault(); (e.target as HTMLInputElement).blur(); } }} />
          </div>
          <SwitchRow label={t("Replace the output if it exists")} on={over} onChange={setOver} />
          <ChoiceRow label={t("Convert on")} hint={t("The engine that does the work; the source and the output can be on any paired engine")} options={onChoices} value={where}
            onChange={setWhere} hidden={whereHidden} />
        </div>
        <div class="side">
          <div class="head">
            <EntryIco entry={entry} meta={m} />
            <div style="min-width:0"><b>{m && m.title ? m.title : entry.name}</b><div class="muted mono">{m && m.title ? entry.name : ""}</div></div>
          </div>
          <div class="facts">
            {problem ? [<div>{t("Problem")}</div>, <div class="err">{problem}</div>]
              : !inspect ? [<div>{t("Reading")}</div>, <div class="muted">…</div>]
              : [
                <div>{t("Format")}</div>, <div>{inspect.format + (inspect.lz4_packed ? " · " + t("LZ4-packed") : "")}</div>,
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
        </div>
      </div>
    </Sheet>
  );
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The folder picker: copy and move queue a job; "pick" resolves with a folder.
import { useEffect, useRef, useState } from "preact/hooks";
import type { Entry, Listing } from "./types";
import { locations, rootOf, driveName, shownPath, hereName } from "./state";
import { api, bytes, dir } from "./util";
import { t, tMsg, placeLabel } from "./i18n";
import { ICONS, SwitchRow } from "./ui";
import { Sheet, openOverlay, closeOverlay } from "./overlay";
import { focusEl, goBack } from "./nav";
import { Crumbs } from "./files";
import { toast } from "./toasts";
import { refreshJobs } from "./queue";

type Kind = "copy" | "move" | "pick";
interface Chosen { folder: string; name: string; overwrite: boolean }
// What the picker's □ and its button do, set by the open picker.
interface Control { go: () => void }

export function openDest(entry: Entry | null, kind: "copy" | "move", start?: string): Promise<string | null>;
export function openDest(entry: Entry | null, kind: "pick", start?: string): Promise<string | null>;
export async function openDest(entry: Entry | null, kind: Kind, start?: string): Promise<string | null> {
  const pick = kind === "pick";
  const ctl: Control = { go: () => {} };
  const goLabel = pick ? t("Use this folder") : kind === "copy" ? t("Copy here") : t("Move here");
  const chosen = await openOverlay<Chosen>(() => <Picker entry={entry} kind={kind} start={start || (entry ? dir(entry.path) : "/")} ctl={ctl} />,
    { hints: [["x", t("Open folder")], ["o", t("Cancel"), goBack], ["s", goLabel, () => ctl.go()]], onSquare: () => ctl.go() });
  if (!chosen || pick || !entry) return chosen ? chosen.folder : null;
  const n = chosen.name.trim();
  if (!n || n.indexOf("/") >= 0) { toast(t("Enter a plain name"), "bad"); return null; }
  try {
    await api("POST", "/api/jobs", { kind, input: entry.path, output: chosen.folder + "/" + n, overwrite: chosen.overwrite });
    toast(kind === "copy" ? t("Copy queued") : t("Move queued"), "ok"); refreshJobs();
  } catch (e: any) { toast(tMsg(e.message), "bad"); }
  return chosen.folder;
}

function Picker(props: { entry: Entry | null; kind: Kind; start: string; ctl: Control }) {
  const { entry, kind } = props;
  const pick = kind === "pick";
  const [data, setData] = useState<Listing | null>(null);
  const [msg, setMsg] = useState<{ text: string; err: boolean }>({ text: "", err: false });
  const [name, setName] = useState(entry ? entry.name : "");
  const [over, setOver] = useState(false);
  const list = useRef<HTMLDivElement>(null);
  const cur = data ? data.path : null;

  async function load(path: string) {
    let d: Listing;
    try { d = await api<Listing>("GET", "/api/browse?path=" + encodeURIComponent(path || "/")); }
    catch (e: any) { setMsg({ text: tMsg(e.message), err: true }); return; }
    setMsg({ text: "", err: false });
    setData(d);
  }
  useEffect(() => { load(props.start); }, []);
  // A new folder: the focus goes to its first row, unless it is already in the list.
  useEffect(() => {
    const box = list.current;
    if (box && !box.contains(document.activeElement)) { const f = box.querySelector(".row") as HTMLElement | null; if (f) focusEl(f); }
  }, [data]);

  const go = () => { if (cur && cur !== "/") closeOverlay(pick ? { folder: cur, name: "", overwrite: false } : { folder: cur, name, overwrite: over }); };
  props.ctl.go = go;

  // Places by engine: this one's, then each paired engine's under its name.
  const groups: { who: string; list: typeof locations.value }[] = [];
  for (const l of locations.value.filter((x) => x.exists)) {
    const who = l.peer ? l.engine! : hereName();
    let g = groups.find((x) => x.who === who);
    if (!g) groups.push(g = { who, list: [] });
    g.list.push(l);
  }
  const folders = data ? data.entries.filter((x) => x.kind === "folder") : [];
  const r: any = !cur || cur === "/" ? null : rootOf(cur);
  const space = !data ? "" : !r && /^\/ftp\//.test(cur!) ? t("On an FTP server") + "  ·  " + cur
    : r ? t("{size} free on {drive}", { size: bytes(r.free), drive: driveName(r.root) }) + (entry && entry.size ? " · " + t("needs {size}", { size: bytes(entry.size) }) : "") + "  ·  " + shownPath(cur)
    : t("Pick a drive and folder.");
  const title = pick ? t("Choose a folder") : t(kind === "copy" ? "Copy {name}" : "Move {name}", { name: entry ? entry.name : "" });
  return (
    <Sheet title={title} foot={<>
      <span class={"msg" + (msg.err ? " err" : "")}>{msg.text}</span>
      <button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={!data || cur === "/"} onClick={go}>{pick ? t("Use this folder") : kind === "copy" ? t("Copy here") : t("Move here")}</button>
    </>}>
      <div>
        <div class="chips">
          {data ? groups.map((g) => (
            <div class="chipgroup">
              {groups.length > 1 ? <span class="who">{g.who}</span> : null}
              {g.list.map((l) => <button class={"chip" + (cur === l.path ? " on" : "")} title={shownPath(l.path)} onClick={() => load(l.path)}>
                {placeLabel((l.peer ? l.place! : l.label).replace(/ · homebrew.*$/, " · homebrew"))}</button>)}
            </div>
          )) : null}
        </div>
        {data ? <Crumbs path={cur} onPick={load} style="font-size:1rem;margin-bottom:.5rem" /> : <div class="crumbs" style="font-size:1rem;margin-bottom:.5rem" />}
        <div ref={list} class="picker scroll">
          {data && !folders.length ? <div class="empty" style="padding:1.2rem">{cur === "/" ? t("No drives.") : t("No folders here.")}</div> : null}
          {folders.map((f) => (
            <div class="row" tabIndex={0} onClick={() => load(f.path)}>
              <div class="ico folder">{cur === "/" ? ICONS.drive() : ICONS.folder()}</div>
              <div class="t ellip">{cur === "/" ? driveName(f.path) : f.name}</div>
            </div>
          ))}
        </div>
        {pick ? null : (
          <div>
            <div class="lbl">{t("Name")}</div>
            <input class="input mono" value={name} spellcheck={false} autocapitalize="off" onInput={(e) => setName((e.target as HTMLInputElement).value)} />
            <div style="margin-top:.5rem"><SwitchRow label={t("Replace if it already exists")} on={over} onChange={setOver} /></div>
          </div>
        )}
        <div class="muted" style="font-size:.88rem;margin-top:.6rem">{space}</div>
      </div>
    </Sheet>
  );
}
